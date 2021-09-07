#include "filter.h"
#include "config.h"

#include "common/protobuf/utility.h"
#include "envoy/network/connection.h"
#include "extensions/access_loggers/grpc/grpc_access_log_utils.h"

#include <google/protobuf/util/time_util.h>

namespace Tsm::Filters::Tcp::Telemetry {

namespace {
constexpr absl::string_view ConnectionEventToString(ConnectionEvent event) {
  switch (event) {
    case ConnectionEvent::OPEN:
      return "open";
    case ConnectionEvent::CONTINUE:
      return "continue";
    case ConnectionEvent::CLOSE:
      return "close";
  }
}
} // namespace

NetworkFilter::NetworkFilter(std::shared_ptr<const FilterConfig> config)
    : config_(std::move(config)) {}

ProxyTelemetry::TcpTelemetryStreamer::LogEntry NetworkFilter::initLogEntry(ConnectionEvent event) {
  ProxyTelemetry::TcpTelemetryStreamer::LogEntry entry;

  auto& connection = filter_callbacks_->connection();
  auto& stream_info = connection.streamInfo();

  auto metadata = Envoy::MessageUtil::keyValueStruct(
      {{"event", std::string{ConnectionEventToString(event)}},
       {"reporter", config_->outbound() ? "outbound" : "inbound"}});

  using namespace std::literals;
  using Envoy::ValueUtil;
  auto duration = google::protobuf::util::TimeUtil::NanosecondsToDuration(
      (connection.dispatcher().timeSource().monotonicTime() - stream_info.startTimeMonotonic())
      / 1ns);

  Envoy::ProtobufWkt::Struct time_struct;
  (*time_struct.mutable_fields())["seconds"] = ValueUtil::numberValue(duration.seconds());
  (*time_struct.mutable_fields())["nanos"] = ValueUtil::numberValue(duration.nanos());
  (*metadata.mutable_fields())["duration"] = ValueUtil::structValue(time_struct);

  auto ts = google::protobuf::util::TimeUtil::NanosecondsToTimestamp(
      connection.dispatcher().timeSource().systemTime().time_since_epoch() / 1ns);
  (*time_struct.mutable_fields())["seconds"] = ValueUtil::numberValue(ts.seconds());
  (*time_struct.mutable_fields())["nanos"] = ValueUtil::numberValue(ts.nanos());
  (*metadata.mutable_fields())["timestamp"] = ValueUtil::structValue(time_struct);

  stream_info.setDynamicMetadata(NAME, metadata);

  Envoy::Extensions::AccessLoggers::GrpcCommon::Utility::extractCommonAccessLogProperties(
      *entry.mutable_common_properties(), stream_info, {});

  auto& connection_properties = *entry.mutable_connection_properties();
  connection_properties.set_received_bytes(stream_info.bytesReceived());
  connection_properties.set_sent_bytes(stream_info.bytesSent());

  return entry;
}

Envoy::Network::FilterStatus NetworkFilter::onNewConnection() {
  ENVOY_LOG(trace, "NetworkFilter::onNewConnection()");

  config_->streamer().send(initLogEntry(ConnectionEvent::OPEN));

  timer_ = filter_callbacks_->connection().dispatcher().createTimer([this]() {
    config_->streamer().send(initLogEntry(ConnectionEvent::CONTINUE));
    timer_->enableTimer(config_->continueLogInterval());
  });

  timer_->enableTimer(config_->continueLogInterval());

  return Envoy::Network::FilterStatus::Continue;
}

void NetworkFilter::initializeReadFilterCallbacks(Envoy::Network::ReadFilterCallbacks& callbacks) {
  filter_callbacks_ = &callbacks;
}

NetworkFilter::~NetworkFilter() {
  config_->streamer().send(initLogEntry(ConnectionEvent::CLOSE));
  if (timer_ && timer_->enabled()) {
    timer_->disableTimer();
  }
}

} // namespace Tsm::Filters::Tcp::Telemetry
