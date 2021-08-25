#include "client_tcp.h"

namespace Mesh7::ProxyTelemetry {

void TcpTelemetryStreamer::send(TcpTelemetryStreamer::LogEntry&& log_entry) {
  envoy::service::accesslog::v3::StreamAccessLogsMessage message;
  *message.mutable_tcp_logs()->add_log_entry() = std::move(log_entry);
  TelemetryStreamer::send(std::move(message));
}

} // namespace Mesh7::ProxyTelemetry
