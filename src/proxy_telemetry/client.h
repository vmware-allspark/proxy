#pragma once

#include "source/common/common/logger.h"
#include "source/common/grpc/typed_async_client.h"
#include "envoy/local_info/local_info.h"
#include "envoy/grpc/async_client_manager.h"

namespace Tsm::ProxyTelemetry {

/// A semi-generic Telemetry streamer class
/// RequestProto is assumed to contain a field `Tsm::Proxy::Identifier identifier`
template <class RequestProto, class ResponseProto>
class TelemetryStreamer : public Envoy::Grpc::AsyncStreamCallbacks<ResponseProto>,
                          public Envoy::Logger::Loggable<Envoy::Logger::Id::filter> {
public:
  TelemetryStreamer(TelemetryStreamer&&) noexcept = default;
  TelemetryStreamer(Envoy::Grpc::AsyncClientFactoryPtr&& factory,
                    const Envoy::LocalInfo::LocalInfo& local_info,
                    std::string const& service_method)
      : local_info_(local_info),
        service_method_(
            *Envoy::Protobuf::DescriptorPool::generated_pool()->FindMethodByName(service_method)),
        client_(factory->createUncachedRawAsyncClient()) {}

  ~TelemetryStreamer() override = default;

  void send(RequestProto&& message);

  // Grpc::AsyncStreamCallbacks
  void onCreateInitialMetadata(Envoy::Http::RequestHeaderMap&) override {}
  void onReceiveInitialMetadata(Envoy::Http::ResponseHeaderMapPtr&&) override {}
  void onReceiveMessage(std::unique_ptr<ResponseProto>&&) override {}
  void onReceiveTrailingMetadata(Envoy::Http::ResponseTrailerMapPtr&&) override {}
  void onRemoteClose(Envoy::Grpc::Status::GrpcStatus status, const std::string& msg) override {
    ENVOY_LOG(info, "GRPC remote closed, status = {}, msg = {}", status, msg);
    stream_ = nullptr;
  }

private:
  const Envoy::LocalInfo::LocalInfo& local_info_;
  const Envoy::Protobuf::MethodDescriptor& service_method_;

  Envoy::Grpc::AsyncStream<RequestProto> stream_{};
  Envoy::Grpc::AsyncClient<RequestProto, ResponseProto> client_;
};

template <class RequestProto, class ResponseProto>
void TelemetryStreamer<RequestProto, ResponseProto>::send(RequestProto&& message) {
  if (stream_ == nullptr) {
    ENVOY_LOG(info, "Starting new GRPC stream");
    stream_ = client_->start(service_method_, *this, Envoy::Http::AsyncClient::StreamOptions());
    if (stream_ == nullptr) {
      ENVOY_LOG(warn, "Failed to start telemetry GRPC client");
      return;
    }
    // For perf reasons, the identifier is only sent on establishing the stream.
    auto* identifier = message.mutable_identifier();
    *identifier->mutable_node() = local_info_.node();
  }
  ENVOY_LOG(trace, "Sending telemetry entry: {}", message.DebugString());
  stream_->sendMessage(message, false);
}

} // namespace Tsm::ProxyTelemetry
