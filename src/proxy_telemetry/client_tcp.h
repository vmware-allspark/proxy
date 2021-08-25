#pragma once

#include "client.h"
#include "envoy/service/accesslog/v3/als.pb.h"

namespace Mesh7::ProxyTelemetry {

class TcpTelemetryStreamer
    : public TelemetryStreamer<envoy::service::accesslog::v3::StreamAccessLogsMessage,
                               envoy::service::accesslog::v3::StreamAccessLogsResponse> {
public:
  using LogEntry = envoy::data::accesslog::v3::TCPAccessLogEntry;

  TcpTelemetryStreamer(Envoy::Grpc::AsyncClientFactoryPtr&& factory,
                       const Envoy::LocalInfo::LocalInfo& local_info)
      : TelemetryStreamer(std::move(factory), local_info,
                          "envoy.service.accesslog.v3.AccessLogService.StreamAccessLogs") {}

  void send(LogEntry&&);
};

} // namespace Mesh7::ProxyTelemetry
