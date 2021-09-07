#pragma once

#include "client.h"
#include "src/proxy_telemetry/telemetry.pb.h"

namespace Tsm::ProxyTelemetry {

class HttpTelemetryStreamer
    : public TelemetryStreamer<StreamHttpTelemetryMessage, StreamHttpTelemetryResponse> {
public:
  using LogEntry = envoy::data::accesslog::v3::HTTPAccessLogEntry;

  HttpTelemetryStreamer(Envoy::Grpc::AsyncClientFactoryPtr&& factory,
                        const Envoy::LocalInfo::LocalInfo& local_info)
      : TelemetryStreamer(std::move(factory), local_info,
                          "Tsm.ProxyTelemetry.HttpTelemetry.StreamHttpTelemetry") {}

  void send(LogEntry&&);
};

} // namespace Tsm::ProxyTelemetry
