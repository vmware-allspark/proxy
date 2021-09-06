#include "client_http.h"

namespace Tsm::ProxyTelemetry {

void HttpTelemetryStreamer::send(HttpTelemetryStreamer::LogEntry&& log_entry) {
  StreamHttpTelemetryMessage message;
  *message.add_entries() = std::move(log_entry);
  TelemetryStreamer::send(std::move(message));
}

} // namespace Tsm::ProxyTelemetry
