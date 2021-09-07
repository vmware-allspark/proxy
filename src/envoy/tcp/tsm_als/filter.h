#pragma once

#include "src/proxy_telemetry/client_tcp.h"

#include "common/common/logger.h"
#include "envoy/event/timer.h"
#include "envoy/network/filter.h"

#include <memory>

namespace Tsm::Filters::Tcp::Telemetry {

enum class ConnectionEvent { OPEN = 0, CONTINUE = 1, CLOSE = 2 };

class FilterConfig;

class NetworkFilter : public Envoy::Network::ReadFilter,
                      public Envoy::Logger::Loggable<Envoy::Logger::Id::filter> {
public:
  static constexpr char NAME[] = "tsm_als.filters.network.telemetry";

  explicit NetworkFilter(std::shared_ptr<const FilterConfig> config);
  ~NetworkFilter() override;

  // Network::ReadFilter
  Envoy::Network::FilterStatus onData(Envoy::Buffer::Instance& /* data */,
                                      bool /* end_stream */) override {
    return Envoy::Network::FilterStatus::Continue;
  };
  Envoy::Network::FilterStatus onNewConnection() override;
  void initializeReadFilterCallbacks(Envoy::Network::ReadFilterCallbacks& callbacks) override;

private:
  ProxyTelemetry::TcpTelemetryStreamer::LogEntry initLogEntry(ConnectionEvent event);

  Envoy::Network::ReadFilterCallbacks* filter_callbacks_{};
  std::shared_ptr<const FilterConfig> config_;

  Envoy::Event::TimerPtr timer_;
};

} // namespace Tsm::Filters::Tcp::Telemetry
