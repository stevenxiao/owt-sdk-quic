// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "owt/quic_transport/sdk/impl/quic_transport_owt_client_impl.h"

#include <utility>

#include "base/memory/ptr_util.h"
#include "base/run_loop.h"
#include "base/threading/thread_task_runner_handle.h"
#include "net/base/net_errors.h"
#include "net/quic/address_utils.h"
#include "net/base/privacy_mode.h"
#include "net/http/http_request_info.h"
#include "net/http/http_response_info.h"
#include "net/log/net_log_source.h"
#include "net/log/net_log_with_source.h"
#include "net/quic/quic_chromium_alarm_factory.h"
#include "net/quic/quic_chromium_connection_helper.h"
#include "net/quic/quic_chromium_packet_reader.h"
#include "net/quic/quic_chromium_packet_writer.h"
#include "net/socket/udp_client_socket.h"
#include "net/spdy/spdy_http_utils.h"
#include "net/third_party/quiche/src/quiche/quic/core/crypto/quic_random.h"
#include "net/third_party/quiche/src/quiche/quic/core/http/spdy_utils.h"
#include "net/third_party/quiche/src/quiche/quic/core/quic_connection.h"
#include "net/third_party/quiche/src/quiche/quic/core/quic_packets.h"
#include "net/third_party/quiche/src/quiche/quic/core/quic_server_id.h"
#include "net/third_party/quiche/src/quiche/quic/platform/api/quic_flags.h"
#include "net/third_party/quiche/src/quiche/quic/tools/quic_simple_client_session.h"
//#include "net/third_party/quiche/src/spdy/core/spdy_header_block.h"

using std::string;

namespace net {

QuicTransportOwtClientImpl::QuicTransportOwtClientImpl(
    quic::QuicSocketAddress server_address,
    const quic::QuicServerId& server_id,
    const quic::ParsedQuicVersionVector& supported_versions,
    std::unique_ptr<quic::ProofVerifier> proof_verifier,
    base::Thread* io_thread,
    base::Thread* event_thread)
    : quic::QuicTransportOwtClientBase(
          server_id,
          supported_versions,
          quic::QuicConfig(),
          CreateQuicConnectionHelper(),
          CreateQuicAlarmFactory(),
          base::WrapUnique(CreateNetworkHelper()),
          std::move(proof_verifier),
          nullptr,
          io_thread->task_runner().get(),
          event_thread->task_runner().get()),
      event_runner_(event_thread->task_runner()),
      weak_factory_(this) {
  if (!io_thread) {
    LOG(INFO) << "Create a new IO stream.";
    io_thread_owned_ =
        std::make_unique<base::Thread>("quic_transport_client_io_thread");
    base::Thread::Options options;
    options.message_pump_type = base::MessagePumpType::IO;
    io_thread_owned_->StartWithOptions(base::Thread::Options(base::MessagePumpType::IO, 0));
    task_runner_ = io_thread_owned_->task_runner();
  } else {
    task_runner_ = io_thread->task_runner();
  }
  set_server_address(server_address);
}

QuicTransportOwtClientImpl::~QuicTransportOwtClientImpl() {
  if (connected()) {
    session()->connection()->CloseConnection(
        quic::QUIC_PEER_GOING_AWAY, "Shutting down",
        quic::ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
  }
}

QuicChromiumConnectionHelper* QuicTransportOwtClientImpl::CreateQuicConnectionHelper() {
  return new QuicChromiumConnectionHelper(&clock_,
                                          quic::QuicRandom::GetInstance());
}

QuicClientMessageLooplNetworkHelper* QuicTransportOwtClientImpl::CreateNetworkHelper() {
  created_helper_ = new QuicClientMessageLooplNetworkHelper(&clock_, this);
  return created_helper_;
}

void QuicTransportOwtClientImpl::Start() {
  task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&QuicTransportOwtClientImpl::StartOnCurrentThread, weak_factory_.GetWeakPtr()));
      return ;
}

void QuicTransportOwtClientImpl::StartOnCurrentThread() {
  if (!Initialize()) {
    std::cerr << "Failed to initialize client." << std::endl;
    if(visitor_) {
      visitor_->OnConnectionFailed();
    }
    return;
  }
  if (!Connect()) {
    std::cerr << "Failed to connect." << std::endl;
    if(visitor_) {
      visitor_->OnConnectionFailed();
    }
    return;
  }

  session_ = client_session();
  session_->set_visitor(this);
  if(visitor_) {
    printf("client call onConnected\n");
    visitor_->OnConnected();
  }
  std::cerr << "client connect to quic server succeed in thread:" << base::PlatformThread::CurrentId() << std::endl;
}

void QuicTransportOwtClientImpl::Stop() {
  task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&QuicTransportOwtClientImpl::StopOnCurrentThread, weak_factory_.GetWeakPtr()));
      return ;
}

void QuicTransportOwtClientImpl::StopOnCurrentThread() {
  Disconnect();
}

int QuicTransportOwtClientImpl::SocketPort() {
  return created_helper_->GetLatestClientAddress().port();
}

void QuicTransportOwtClientImpl::SetVisitor(owt::quic::QuicTransportClientInterface::Visitor* visitor) {
  visitor_ = visitor;
}

void QuicTransportOwtClientImpl::OnConnectionClosed(char* id, size_t len) {
  if(visitor_) {
    visitor_->OnConnectionClosed(id, len);
  }
}

void QuicTransportOwtClientImpl::NewStreamCreated(quic::QuicTransportOwtStreamImpl* stream) {
  if(visitor_) {
    visitor_->OnIncomingStream(stream);
  }
}

void QuicTransportOwtClientImpl::OnIncomingNewStream(quic::QuicTransportOwtStreamImpl* stream) {
  event_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](QuicTransportOwtClientImpl* client,
             quic::QuicTransportOwtStreamImpl* stream) {
            client->NewStreamCreated(stream);
          },
          base::Unretained(this), base::Unretained(stream)));
}

void QuicTransportOwtClientImpl::OnStreamClosed(uint32_t id) {
  if(visitor_) {
    visitor_->OnStreamClosed(id);
  }
}

const char* QuicTransportOwtClientImpl::Id() {
  std::cerr << "QuicTransportOwtClientImpl Get client session id:" << client_session()->connection()->connection_id().ToString();
  const std::string& session_id_str =
      client_session()->connection()->connection_id().ToString();
  char* id = new char[session_id_str.size() + 1];
  strcpy(id, session_id_str.c_str());

  return id;

}

void QuicTransportOwtClientImpl::CloseStreamOnCurrentThread(uint32_t id) {
  printf("QuicTransportOwtClientImpl::CloseStreamOnCurrentThread close stream:%d\n", id);
  session_->ResetStream(id, quic::QUIC_STREAM_CANCELLED);
}

void QuicTransportOwtClientImpl::CloseStream(uint32_t id) {
  event_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&QuicTransportOwtClientImpl::CloseStreamOnCurrentThread, base::Unretained(this), id));
}

uint8_t QuicTransportOwtClientImpl::length() {
  return client_session()->connection()->client_connection_id().length();
}

owt::quic::QuicTransportStreamInterface* QuicTransportOwtClientImpl::CreateBidirectionalStream() {
  std::cerr << "QuicTransportOwtClientImpl::CreateBidirectionalStream" << std::endl;
  owt::quic::QuicTransportStreamInterface* result(nullptr);
  base::WaitableEvent done(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                           base::WaitableEvent::InitialState::NOT_SIGNALED);
  event_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](QuicTransportOwtClientImpl* client,
             owt::quic::QuicTransportStreamInterface** result, base::WaitableEvent* event) {
            *result = client->CreateBidirectionalStreamOnCurrentThread();
            event->Signal();
          },
          base::Unretained(this), base::Unretained(&result),
          base::Unretained(&done)));
  done.Wait();
  return result;
}

owt::quic::QuicTransportStreamInterface* QuicTransportOwtClientImpl::CreateBidirectionalStreamOnCurrentThread() {
  std::cerr << "QuicTransportOwtClientImpl::CreateBidirectionalStreamOnCurrentThread" << std::endl;
  if (!connected()) {
    return nullptr;
  }

  auto* stream = static_cast<owt::quic::QuicTransportStreamInterface*>(
      client_session()->CreateOutgoingBidirectionalStream());
 
  std::cerr << "QuicTransportOwtClientImpl::CreateBidirectionalStreamOnCurrentThread succeed" << std::endl;
  return stream;
}

QuicChromiumAlarmFactory* QuicTransportOwtClientImpl::CreateQuicAlarmFactory() {
  return new QuicChromiumAlarmFactory(base::ThreadTaskRunnerHandle::Get().get(),
                                      &clock_);
}

}  // namespace quic
