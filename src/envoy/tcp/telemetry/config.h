#pragma once

#include "src/envoy/tcp/telemetry/config.pb.h"
#include "src/proxy_telemetry/client_tcp.h"

#include "envoy/server/filter_config.h"
#include "envoy/thread_local/thread_local.h"

namespace Mesh7::Filters::Tcp::Telemetry {

class FilterConfig : public Envoy::Logger::Loggable<Envoy::Logger::Id::config> {
  struct ThreadLocalStreamer : public Envoy::ThreadLocal::ThreadLocalObject {
    ThreadLocalStreamer(ProxyTelemetry::TcpTelemetryStreamer&& streamer);

    ProxyTelemetry::TcpTelemetryStreamer streamer_;
  };

public:
  FilterConfig(Proto::Config const&, Envoy::Server::Configuration::FactoryContext& context);

  Proto::Config const& proto() const { return proto_; }

  bool outbound() const { return outbound_; }

  std::chrono::milliseconds continueLogInterval() const { return continue_log_interval_; }

  ProxyTelemetry::TcpTelemetryStreamer& streamer() const {
    return tls_slot_->getTyped<ThreadLocalStreamer>().streamer_;
  }

private:
  const Proto::Config proto_;
  const bool outbound_;
  const std::chrono::milliseconds continue_log_interval_;
  const Envoy::ThreadLocal::SlotPtr tls_slot_;
};

} // namespace Mesh7::Filters::Tcp::Telemetry
