#include "filter.h"
#include "config.h"

#include "src/envoy/tcp/telemetry/config.pb.validate.h"

#include "extensions/filters/network/common/factory_base.h"
#include "envoy/registry/registry.h"

namespace Mesh7::Filters::Tcp::Telemetry {

class FilterFactory : public Envoy::Extensions::NetworkFilters::Common::FactoryBase<Proto::Config> {
public:
  FilterFactory() : FactoryBase(NetworkFilter::NAME) {} // NOLINT(modernize-use-equals-default)

private:
  Envoy::Network::FilterFactoryCb createFilterFactoryFromProtoTyped(
      const Proto::Config& proto_config,
      Envoy::Server::Configuration::FactoryContext& context) override;
};

Envoy::Network::FilterFactoryCb FilterFactory::createFilterFactoryFromProtoTyped(
    const Proto::Config& proto_config, Envoy::Server::Configuration::FactoryContext& context) {
  auto filter_config = std::make_shared<FilterConfig>(proto_config, context);
  return [filter_config{std::move(filter_config)}](Envoy::Network::FilterManager& manager) -> void {
    manager.addReadFilter(std::make_shared<NetworkFilter>(filter_config));
  };
}

/// Static registration for this filter. @see RegisterFactory.
REGISTER_FACTORY(FilterFactory, Envoy::Server::Configuration::NamedNetworkFilterConfigFactory);

} // namespace Mesh7::Filters::Tcp::Telemetry
