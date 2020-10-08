/* Copyright 2019 Istio Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "extensions/stackdriver/stackdriver.h"

#include <google/protobuf/util/json_util.h>

#include <random>
#include <string>
#include <unordered_map>

#include "extensions/common/proto_util.h"
#include "extensions/stackdriver/edges/mesh_edges_service_client.h"
#include "extensions/stackdriver/log/exporter.h"
#include "extensions/stackdriver/metric/registry.h"

#ifndef NULL_PLUGIN
#include "api/wasm/cpp/proxy_wasm_intrinsics.h"
#else

#include "include/proxy-wasm/null_plugin.h"

namespace proxy_wasm {
namespace null_plugin {
#endif
namespace Stackdriver {

using namespace opencensus::exporters::stats;
using namespace google::protobuf::util;
using namespace ::Extensions::Stackdriver::Common;
using namespace ::Extensions::Stackdriver::Metric;
using ::Extensions::Stackdriver::Edges::EdgeReporter;
using Extensions::Stackdriver::Edges::MeshEdgesServiceClientImpl;
using Extensions::Stackdriver::Log::ExporterImpl;
using ::Extensions::Stackdriver::Log::Logger;
using stackdriver::config::v1alpha1::PluginConfig;
using ::Wasm::Common::kDownstreamMetadataIdKey;
using ::Wasm::Common::kDownstreamMetadataKey;
using ::Wasm::Common::kUpstreamMetadataIdKey;
using ::Wasm::Common::kUpstreamMetadataKey;
using ::Wasm::Common::RequestInfo;
using ::Wasm::Common::TCPConnectionState;

constexpr char kStackdriverExporter[] = "stackdriver_exporter";
constexpr char kExporterRegistered[] = "registered";
constexpr int kDefaultTickerMilliseconds = 10000;  // 10s

namespace {

// Get metric export interval from node metadata. Returns 60 seconds if interval
// is not found in metadata.
int getMonitoringExportInterval() {
  std::string interval_s = "";
  if (getValue({"node", "metadata", kMonitoringExportIntervalKey},
               &interval_s)) {
    return std::stoi(interval_s);
  }
  return 60;
}

// Get proxy timer interval from node metadata in milliseconds. Returns 10
// seconds if interval is not found in metadata.
int getProxyTickerIntervalMilliseconds() {
  std::string interval_s = "";
  if (getValue({"node", "metadata", kProxyTickerIntervalKey}, &interval_s)) {
    return std::stoi(interval_s) * 1000;
  }
  return kDefaultTickerMilliseconds;
}

// Get logging export interval from node metadata in nanoseconds. Returns 60
// seconds if interval is not found in metadata.
long int getTcpLogEntryTimeoutNanoseconds() {
  std::string interval_s = "";
  if (getValue({"node", "metadata", kTcpLogEntryTimeoutKey}, &interval_s)) {
    return std::stoi(interval_s) * 1000000000;
  }
  return kDefaultTcpLogEntryTimeoutNanoseconds;
}

// Get port of security token exchange server from node metadata, if not
// provided or "0" is provided, emtpy will be returned.
std::string getSTSPort() {
  std::string sts_port;
  if (getValue({"node", "metadata", kSTSPortKey}, &sts_port) &&
      sts_port != "0") {
    return sts_port;
  }
  return "";
}

// Get file name for the token test override.
std::string getTokenFile() {
  std::string token_file;
  if (!getValue({"node", "metadata", kTokenFile}, &token_file)) {
    return "";
  }
  return token_file;
}

// Get file name for the root CA PEM file test override.
std::string getCACertFile() {
  std::string ca_cert_file;
  if (!getValue({"node", "metadata", kCACertFile}, &ca_cert_file)) {
    return "";
  }
  return ca_cert_file;
}

// Get secure stackdriver endpoint for e2e testing.
std::string getSecureEndpoint() {
  std::string secure_endpoint;
  if (!getValue({"node", "metadata", kSecureStackdriverEndpointKey},
                &secure_endpoint)) {
    return "";
  }
  return secure_endpoint;
}

// Get insecure stackdriver endpoint for e2e testing.
std::string getInsecureEndpoint() {
  std::string insecure_endpoint;
  if (!getValue({"node", "metadata", kInsecureStackdriverEndpointKey},
                &insecure_endpoint)) {
    return "";
  }
  return insecure_endpoint;
}

// Get GCP monitoring endpoint. When this is provided, it will override the
// default production endpoint. This should be used to test staging monitoring
// endpoint.
std::string getMonitoringEndpoint() {
  std::string monitoring_endpoint;
  if (!getValue({"node", "metadata", kMonitoringEndpointKey},
                &monitoring_endpoint)) {
    return "";
  }
  return monitoring_endpoint;
}

// Get GCP project number.
std::string getProjectNumber() {
  std::string project_number;
  if (!getValue({"node", "metadata", "PLATFORM_METADATA", kGCPProjectNumberKey},
                &project_number)) {
    return "";
  }
  return project_number;
}

void clearTcpMetrics(::Wasm::Common::RequestInfo& request_info) {
  request_info.tcp_connections_opened = 0;
  request_info.tcp_sent_bytes = 0;
  request_info.tcp_received_bytes = 0;
}

// Get local node metadata. If mesh id is not filled or does not exist,
// fall back to default format `proj-<project-number>`.
flatbuffers::DetachedBuffer getLocalNodeMetadata() {
  google::protobuf::Struct node;
  auto local_node_info = ::Wasm::Common::extractLocalNodeFlatBuffer();
  ::Wasm::Common::extractStructFromNodeFlatBuffer(
      *flatbuffers::GetRoot<::Wasm::Common::FlatNode>(local_node_info.data()),
      &node);
  const auto mesh_id_it = node.fields().find("MESH_ID");
  if (mesh_id_it != node.fields().end() &&
      !mesh_id_it->second.string_value().empty() &&
      absl::StartsWith(mesh_id_it->second.string_value(), "proj-")) {
    // do nothing
  } else {
    // Insert or update mesh id to default format as it is missing, empty, or
    // not properly set.
    auto project_number = getProjectNumber();
    auto* mesh_id_field =
        (*node.mutable_fields())["MESH_ID"].mutable_string_value();
    if (!project_number.empty()) {
      *mesh_id_field = absl::StrCat("proj-", project_number);
    }
  }
  return ::Wasm::Common::extractNodeFlatBufferFromStruct(node);
}

}  // namespace

// onConfigure == false makes the proxy crash.
// Only policy plugins should return false.
bool StackdriverRootContext::onConfigure(size_t size) {
  initialized_ = configure(size);
  return true;
}

bool StackdriverRootContext::configure(size_t configuration_size) {
  // onStart is called prior to onConfigure
  int proxy_tick_ms = getProxyTickerIntervalMilliseconds();
  proxy_set_tick_period_milliseconds(getProxyTickerIntervalMilliseconds());
  // Parse configuration JSON string.
  std::string configuration = "{}";
  if (configuration_size > 0) {
    auto configuration_data = getBufferBytes(
        WasmBufferType::PluginConfiguration, 0, configuration_size);
    configuration = configuration_data->toString();
  }

  // TODO: add config validation to reject the listener if project id is not in
  // metadata. Parse configuration JSON string.
  JsonParseOptions json_options;
  json_options.ignore_unknown_fields = true;
  Status status = JsonStringToMessage(configuration, &config_, json_options);
  if (status != Status::OK) {
    logWarn("Cannot parse Stackdriver plugin configuration JSON string " +
            configuration + ", " + status.message().ToString());
    return false;
  }
  local_node_info_ = getLocalNodeMetadata();

  if (config_.has_log_report_duration()) {
    log_report_duration_nanos_ =
        ::google::protobuf::util::TimeUtil::DurationToNanoseconds(
            config_.log_report_duration());
    long int proxy_tick_ns = proxy_tick_ms * 1000;
    if (log_report_duration_nanos_ < (proxy_tick_ns) ||
        log_report_duration_nanos_ % proxy_tick_ns != 0) {
      logWarn(absl::StrCat(
          "The duration set is less than or not a multiple of default timer's "
          "period. Default Timer MS: ",
          proxy_tick_ms,
          " Lod Duration Nanosecond: ", log_report_duration_nanos_));
    }
  }

  direction_ = ::Wasm::Common::getTrafficDirection();
  use_host_header_fallback_ = !config_.disable_host_header_fallback();
  const ::Wasm::Common::FlatNode& local_node =
      *flatbuffers::GetRoot<::Wasm::Common::FlatNode>(local_node_info_.data());

  // Common stackdriver stub option for logging, edge and monitoring.
  ::Extensions::Stackdriver::Common::StackdriverStubOption stub_option;
  stub_option.sts_port = getSTSPort();
  stub_option.test_token_path = getTokenFile();
  stub_option.test_root_pem_path = getCACertFile();
  stub_option.secure_endpoint = getSecureEndpoint();
  stub_option.insecure_endpoint = getInsecureEndpoint();
  stub_option.monitoring_endpoint = getMonitoringEndpoint();
  stub_option.enable_log_compression = config_.has_enable_log_compression() &&
                                       config_.enable_log_compression().value();
  const auto platform_metadata = local_node.platform_metadata();
  if (platform_metadata) {
    const auto project_iter = platform_metadata->LookupByKey(kGCPProjectKey);
    if (project_iter) {
      stub_option.project_id = flatbuffers::GetString(project_iter->value());
    }
  }

  if (!logger_ && enableAccessLog()) {
    // logger should only be initiated once, for now there is no reason to
    // recreate logger because of config update.
    auto logging_stub_option = stub_option;
    logging_stub_option.default_endpoint = kLoggingService;
    auto exporter = std::make_unique<ExporterImpl>(this, logging_stub_option);
    // logger takes ownership of exporter.
    if (config_.max_log_batch_size_in_bytes() > 0) {
      logger_ = std::make_unique<Logger>(local_node, std::move(exporter),
                                         config_.max_log_batch_size_in_bytes());
    } else {
      logger_ = std::make_unique<Logger>(local_node, std::move(exporter));
    }
    tcp_log_entry_timeout_ = getTcpLogEntryTimeoutNanoseconds();
  }

  if (!edge_reporter_ && enableEdgeReporting()) {
    // edge reporter should only be initiated once, for now there is no reason
    // to recreate edge reporter because of config update.
    auto edge_stub_option = stub_option;
    edge_stub_option.default_endpoint = kMeshTelemetryService;
    auto edges_client =
        std::make_unique<MeshEdgesServiceClientImpl>(this, edge_stub_option);

    if (config_.max_edges_batch_size() > 0 &&
        config_.max_edges_batch_size() <= 1000) {
      edge_reporter_ = std::make_unique<EdgeReporter>(
          local_node, std::move(edges_client), config_.max_edges_batch_size());
    } else {
      edge_reporter_ = std::make_unique<EdgeReporter>(
          local_node, std::move(edges_client),
          ::Extensions::Stackdriver::Edges::kDefaultAssertionBatchSize);
    }
  }

  if (config_.has_mesh_edges_reporting_duration()) {
    auto duration = ::google::protobuf::util::TimeUtil::DurationToNanoseconds(
        config_.mesh_edges_reporting_duration());
    // if the interval duration is longer than the epoch duration, use the
    // epoch duration.
    if (duration >= kDefaultEdgeEpochReportDurationNanoseconds) {
      duration = kDefaultEdgeEpochReportDurationNanoseconds;
    }
    edge_new_report_duration_nanos_ = duration;
  } else {
    edge_new_report_duration_nanos_ = kDefaultEdgeNewReportDurationNanoseconds;
  }
  long int proxy_tick_ns = proxy_tick_ms * 1000;
  if (edge_new_report_duration_nanos_ < proxy_tick_ns ||
      edge_new_report_duration_nanos_ % proxy_tick_ns != 0) {
    logWarn(absl::StrCat(
        "The duration set is less than or not a multiple of default timer's "
        "period. "
        "Default Timer MS: ",
        proxy_tick_ms,
        " Edge Report Duration Nanosecond: ", edge_new_report_duration_nanos_));
  }

  // Register OC Stackdriver exporter and views to be exported.
  // Note exporter and views are global singleton so they should only be
  // registered once.
  WasmDataPtr registered;
  if (WasmResult::Ok == getSharedData(kStackdriverExporter, &registered)) {
    return true;
  }

  setSharedData(kStackdriverExporter, kExporterRegistered);
  auto monitoring_stub_option = stub_option;
  monitoring_stub_option.default_endpoint = kMonitoringService;
  opencensus::exporters::stats::StackdriverExporter::Register(
      getStackdriverOptions(local_node, monitoring_stub_option));
  opencensus::stats::StatsExporter::SetInterval(
      absl::Seconds(getMonitoringExportInterval()));

  // Register opencensus measures and views.
  registerViews();

  return true;
}

bool StackdriverRootContext::onStart(size_t) { return true; }

void StackdriverRootContext::onTick() {
  auto cur = static_cast<long int>(getCurrentTimeNanoseconds());
  if (enableEdgeReporting()) {
    if ((cur - last_edge_epoch_report_call_nanos_) >
        edge_epoch_report_duration_nanos_) {
      // end of epoch
      edge_reporter_->reportEdges(true /* report ALL edges from epoch*/);
      last_edge_epoch_report_call_nanos_ = cur;
      last_edge_new_report_call_nanos_ = cur;
    } else if ((cur - last_edge_new_report_call_nanos_) >
               edge_new_report_duration_nanos_) {
      // end of intra-epoch interval
      edge_reporter_->reportEdges(false /* only report new edges*/);
      last_edge_new_report_call_nanos_ = cur;
    }
  }

  for (auto const& item : tcp_request_queue_) {
    // requestinfo is null, so continue.
    if (item.second == nullptr) {
      continue;
    }
    Context* context = getContext(item.first);
    if (context == nullptr) {
      continue;
    }
    context->setEffectiveContext();
    if (recordTCP(item.first)) {
      // Clear existing data in TCP metrics, so that we don't double count the
      // metrics.
      clearTcpMetrics(*(item.second->request_info));
    }
  }

  if (enableAccessLog() &&
      (cur - last_log_report_call_nanos_ > log_report_duration_nanos_)) {
    logger_->exportLogEntry(/* is_on_done= */ false);
    last_log_report_call_nanos_ = cur;
  }
}

bool StackdriverRootContext::onDone() {
  bool done = true;
  // Check if logger is empty. In base Wasm VM, only onStart and onDone are
  // called, but onConfigure is not triggered. onConfigure is only triggered in
  // thread local VM, which makes it possible that logger_ is empty ptr even
  // when logging is enabled.
  if (logger_ && enableAccessLog() &&
      logger_->exportLogEntry(/* is_on_done= */ true)) {
    done = false;
  }
  // TODO: add on done for edge.
  for (auto const& item : tcp_request_queue_) {
    // requestinfo is null, so continue.
    if (item.second == nullptr) {
      continue;
    }
    recordTCP(item.first);
  }
  tcp_request_queue_.clear();
  return done;
}

void StackdriverRootContext::record() {
  const bool outbound = isOutbound();
  std::string peer;
  bool peer_found = getValue(
      {outbound ? kUpstreamMetadataKey : kDownstreamMetadataKey}, &peer);
  const ::Wasm::Common::FlatNode& peer_node =
      *flatbuffers::GetRoot<::Wasm::Common::FlatNode>(
          peer_found ? reinterpret_cast<const uint8_t*>(peer.data())
                     : empty_node_info_.data());
  const ::Wasm::Common::FlatNode& local_node = getLocalNode();
  const ::Wasm::Common::FlatNode& destination_node_info =
      outbound ? peer_node : local_node;

  ::Wasm::Common::RequestInfo request_info;
  ::Wasm::Common::populateHTTPRequestInfo(
      outbound, useHostHeaderFallback(), &request_info,
      flatbuffers::GetString(destination_node_info.namespace_()));
  ::Extensions::Stackdriver::Metric::record(
      outbound, local_node, peer_node, request_info,
      !config_.disable_http_size_metrics());
  bool extended_info_populated = false;
  if ((enableAllAccessLog() ||
       (enableAccessLogOnError() &&
        (request_info.response_code >= 400 ||
         request_info.response_flag != ::Wasm::Common::NONE))) &&
      shouldLogThisRequest(request_info)) {
    ::Wasm::Common::populateExtendedHTTPRequestInfo(&request_info);
    extended_info_populated = true;
    logger_->addLogEntry(request_info, peer_node, outbound, false /* audit */);
  }

  if (enableAuditLog() && shouldAuditThisRequest()) {
    if (!extended_info_populated) {
      ::Wasm::Common::populateExtendedHTTPRequestInfo(&request_info);
    }
    logger_->addLogEntry(request_info, peer_node, outbound, true /* audit */);
  }

  if (enableEdgeReporting()) {
    std::string peer_id;
    if (!getPeerId(peer_id)) {
      LOG_DEBUG(absl::StrCat(
          "cannot get metadata for: ", ::Wasm::Common::kDownstreamMetadataIdKey,
          "; skipping edge."));
      return;
    }
    edge_reporter_->addEdge(request_info, peer_id, peer_node);
  }
}

bool StackdriverRootContext::recordTCP(uint32_t id) {
  const bool outbound = isOutbound();
  std::string peer_id;
  getPeerId(peer_id);
  std::string peer;
  bool peer_found = getValue(
      {outbound ? kUpstreamMetadataKey : kDownstreamMetadataKey}, &peer);
  const ::Wasm::Common::FlatNode& peer_node =
      *flatbuffers::GetRoot<::Wasm::Common::FlatNode>(
          peer_found ? reinterpret_cast<const uint8_t*>(peer.data())
                     : empty_node_info_.data());
  const ::Wasm::Common::FlatNode& local_node = getLocalNode();
  const ::Wasm::Common::FlatNode& destination_node_info =
      outbound ? peer_node : local_node;

  auto req_iter = tcp_request_queue_.find(id);
  if (req_iter == tcp_request_queue_.end() || req_iter->second == nullptr) {
    return false;
  }
  StackdriverRootContext::TcpRecordInfo& record_info = *(req_iter->second);
  ::Wasm::Common::RequestInfo& request_info = *(record_info.request_info);

  // For TCP, if peer metadata is not available, peer id is set as not found.
  // Otherwise, we wait for metadata exchange to happen before we report  any
  // metric before a timeout.
  // We keep waiting if response flags is zero, as that implies, there has
  // been no error in connection.
  uint64_t response_flags = 0;
  getValue({"response", "flags"}, &response_flags);
  auto cur = static_cast<long int>(
      proxy_wasm::null_plugin::getCurrentTimeNanoseconds());
  bool waiting_for_metadata =
      !peer_found && peer_id != ::Wasm::Common::kMetadataNotFoundValue;
  bool no_error = response_flags == 0;
  bool log_open_on_timeout =
      !record_info.tcp_open_entry_logged &&
      (cur - request_info.start_time) > tcp_log_entry_timeout_;
  if (waiting_for_metadata && no_error && !log_open_on_timeout) {
    return false;
  }
  if (!request_info.is_populated) {
    ::Wasm::Common::populateTCPRequestInfo(
        outbound, &request_info,
        flatbuffers::GetString(destination_node_info.namespace_()));
  }
  // Record TCP Metrics.
  ::Extensions::Stackdriver::Metric::recordTCP(outbound, local_node, peer_node,
                                               request_info);
  bool extended_info_populated = false;
  // Add LogEntry to Logger. Log Entries are batched and sent on timer
  // to Stackdriver Logging Service.
  if (enableAllAccessLog() || (enableAccessLogOnError() && !no_error)) {
    ::Wasm::Common::populateExtendedRequestInfo(&request_info);
    extended_info_populated = true;
    // It's possible that for a short lived TCP connection, we log TCP
    // Connection Open log entry on connection close.
    if (!record_info.tcp_open_entry_logged &&
        request_info.tcp_connection_state ==
            ::Wasm::Common::TCPConnectionState::Close) {
      record_info.request_info->tcp_connection_state =
          ::Wasm::Common::TCPConnectionState::Open;
      logger_->addTcpLogEntry(*record_info.request_info, peer_node,
                              record_info.request_info->start_time, outbound,
                              false /* audit */);
      record_info.request_info->tcp_connection_state =
          ::Wasm::Common::TCPConnectionState::Close;
    }
    logger_->addTcpLogEntry(request_info, peer_node,
                            getCurrentTimeNanoseconds(), outbound,
                            false /* audit */);
  }

  if (enableAuditLog() && shouldAuditThisRequest()) {
    if (!extended_info_populated) {
      ::Wasm::Common::populateExtendedRequestInfo(&request_info);
    }
    // It's possible that for a short lived TCP connection, we audit log TCP
    // Connection Open log entry on connection close.
    if (!record_info.tcp_open_entry_logged &&
        request_info.tcp_connection_state ==
            ::Wasm::Common::TCPConnectionState::Close) {
      record_info.request_info->tcp_connection_state =
          ::Wasm::Common::TCPConnectionState::Open;
      logger_->addTcpLogEntry(*record_info.request_info, peer_node,
                              record_info.request_info->start_time, outbound,
                              true /* audit */);
      record_info.request_info->tcp_connection_state =
          ::Wasm::Common::TCPConnectionState::Close;
    }
    logger_->addTcpLogEntry(*record_info.request_info, peer_node,
                            record_info.request_info->start_time, outbound,
                            true /* audit */);
  }

  if (log_open_on_timeout) {
    // If we logged the request on timeout, for outbound requests, we try to
    // populate the request info again when metadata is available.
    request_info.is_populated = outbound ? false : true;
  }
  if (!record_info.tcp_open_entry_logged) {
    record_info.tcp_open_entry_logged = true;
  }
  return true;
}

inline bool StackdriverRootContext::isOutbound() {
  return direction_ == ::Wasm::Common::TrafficDirection::Outbound;
}

inline bool StackdriverRootContext::enableAccessLog() {
  return enableAllAccessLog() || enableAccessLogOnError();
}

inline bool StackdriverRootContext::enableAllAccessLog() {
  // TODO(gargnupur): Remove (!config_.disable_server_access_logging() &&
  // !isOutbound) once disable_server_access_logging config is removed.
  return (!config_.disable_server_access_logging() && !isOutbound()) ||
         config_.access_logging() ==
             stackdriver::config::v1alpha1::PluginConfig::FULL;
}

inline bool StackdriverRootContext::enableAccessLogOnError() {
  return config_.access_logging() ==
         stackdriver::config::v1alpha1::PluginConfig::ERRORS_ONLY;
}

inline bool StackdriverRootContext::enableAuditLog() {
  return config_.enable_audit_log();
}

inline bool StackdriverRootContext::enableEdgeReporting() {
  return config_.enable_mesh_edges_reporting() && !isOutbound();
}

bool StackdriverRootContext::shouldLogThisRequest(
    ::Wasm::Common::RequestInfo& request_info) {
  std::string shouldLog = "";
  if (!getValue({::Wasm::Common::kAccessLogPolicyKey}, &shouldLog)) {
    LOG_DEBUG("cannot get envoy access log info from filter state.");
    return true;
  }
  // Add label log_sampled if Access Log Policy sampling was applied to logs.
  request_info.log_sampled = (shouldLog != "no");
  return request_info.log_sampled;
}

bool StackdriverRootContext::shouldAuditThisRequest() {
  return Wasm::Common::getAuditPolicy();
}

void StackdriverRootContext::addToTCPRequestQueue(uint32_t id) {
  std::unique_ptr<::Wasm::Common::RequestInfo> request_info =
      std::make_unique<::Wasm::Common::RequestInfo>();
  request_info->tcp_connections_opened++;
  request_info->start_time = static_cast<long int>(
      proxy_wasm::null_plugin::getCurrentTimeNanoseconds());
  std::unique_ptr<StackdriverRootContext::TcpRecordInfo> record_info =
      std::make_unique<StackdriverRootContext::TcpRecordInfo>();
  record_info->request_info = std::move(request_info);
  record_info->tcp_open_entry_logged = false;
  tcp_request_queue_[id] = std::move(record_info);
}

void StackdriverRootContext::deleteFromTCPRequestQueue(uint32_t id) {
  tcp_request_queue_.erase(id);
}

void StackdriverRootContext::incrementReceivedBytes(uint32_t id, size_t size) {
  tcp_request_queue_[id]->request_info->tcp_received_bytes += size;
  tcp_request_queue_[id]->request_info->tcp_total_received_bytes += size;
}

void StackdriverRootContext::incrementSentBytes(uint32_t id, size_t size) {
  tcp_request_queue_[id]->request_info->tcp_sent_bytes += size;
  tcp_request_queue_[id]->request_info->tcp_total_sent_bytes += size;
}

void StackdriverRootContext::incrementConnectionClosed(uint32_t id) {
  tcp_request_queue_[id]->request_info->tcp_connections_closed++;
}

void StackdriverRootContext::setConnectionState(
    uint32_t id, ::Wasm::Common::TCPConnectionState state) {
  tcp_request_queue_[id]->request_info->tcp_connection_state = state;
}

// TODO(bianpengyuan) Add final export once root context supports onDone.
// https://github.com/envoyproxy/envoy-wasm/issues/240

StackdriverRootContext* StackdriverContext::getRootContext() {
  RootContext* root = this->root();
  return dynamic_cast<StackdriverRootContext*>(root);
}

void StackdriverContext::onLog() {
  if (!is_initialized_) {
    return;
  }
  if (is_tcp_) {
    getRootContext()->incrementConnectionClosed(context_id_);
    getRootContext()->setConnectionState(
        context_id_, ::Wasm::Common::TCPConnectionState::Close);
    getRootContext()->recordTCP(context_id_);
    getRootContext()->deleteFromTCPRequestQueue(context_id_);
    return;
  }
  // Record telemetry based on request info.
  getRootContext()->record();
}

}  // namespace Stackdriver

#ifdef NULL_PLUGIN
}  // namespace null_plugin
}  // namespace proxy_wasm
#endif
