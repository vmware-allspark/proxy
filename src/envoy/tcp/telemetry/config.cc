#include "config.h"

#include "common/protobuf/utility.h"

namespace Mesh7::Filters::Tcp::Telemetry {

using DurationUtil = Envoy::DurationUtil;

FilterConfig::ThreadLocalStreamer::ThreadLocalStreamer(
    ProxyTelemetry::TcpTelemetryStreamer&& streamer)
    : streamer_(std::move(streamer)) {}

FilterConfig::FilterConfig(Proto::Config const& config,
                           Envoy::Server::Configuration::FactoryContext& context)
    : proto_(config),
      outbound_(context.direction() == envoy::config::core::v3::OUTBOUND),
      continue_log_interval_(PROTOBUF_GET_MS_OR_DEFAULT(config, continue_log_interval, 10000)),
      tls_slot_(context.threadLocal().allocateSlot()) {
  tls_slot_->set([this, &context](Envoy::Event::Dispatcher&) {
    return std::make_shared<ThreadLocalStreamer>(ProxyTelemetry::TcpTelemetryStreamer(
        context.clusterManager().grpcAsyncClientManager().factoryForGrpcService(
            proto_.grpc_service(), context.scope(), false),
        context.localInfo()));
  });
}

} // namespace Mesh7::Filters::Tcp::Telemetry
