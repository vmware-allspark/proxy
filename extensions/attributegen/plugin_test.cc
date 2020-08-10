/* Copyright 2020 Istio Authors. All Rights Reserved.
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

#include <set>
#include <unordered_map>

#include "common/buffer/buffer_impl.h"
#include "common/http/message_impl.h"
#include "common/stats/isolated_store_impl.h"
#include "common/stream_info/stream_info_impl.h"
#include "envoy/server/lifecycle_notifier.h"
#include "extensions/common/wasm/wasm_state.h"
#include "extensions/filters/http/wasm/wasm_filter.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "test/mocks/grpc/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/server/mocks.h"
#include "test/mocks/ssl/mocks.h"
#include "test/mocks/stream_info/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/environment.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"
#include "test/test_common/wasm_base.h"

using testing::_;
using testing::AtLeast;
using testing::Eq;
using testing::InSequence;
using testing::Invoke;
using testing::Return;
using testing::ReturnPointee;
using testing::ReturnRef;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Wasm {

using envoy::config::core::v3::TrafficDirection;
using Envoy::Extensions::Common::Wasm::PluginSharedPtr;
using Envoy::Extensions::Common::Wasm::Wasm;
using Envoy::Extensions::Common::Wasm::WasmHandleSharedPtr;
using Envoy::Extensions::Common::Wasm::WasmState;
using GrpcService = envoy::config::core::v3::GrpcService;
using WasmFilterConfig = envoy::extensions::filters::http::wasm::v3::Wasm;

class TestFilter : public Envoy::Extensions::Common::Wasm::Context {
 public:
  TestFilter(Wasm* wasm, uint32_t root_context_id,
             Envoy::Extensions::Common::Wasm::PluginSharedPtr plugin)
      : Envoy::Extensions::Common::Wasm::Context(wasm, root_context_id,
                                                 plugin) {}
  void log(const Http::RequestHeaderMap* request_headers,
           const Http::ResponseHeaderMap* response_headers,
           const Http::ResponseTrailerMap* response_trailers,
           const StreamInfo::StreamInfo& stream_info) override {
    Envoy::Extensions::Common::Wasm::Context::log(
        request_headers, response_headers, response_trailers, stream_info);
  }
  // MOCK_CONTEXT_LOG_;
};

class TestRoot : public Envoy::Extensions::Common::Wasm::Context {
 public:
  TestRoot(Wasm* wasm, Envoy::Extensions::Common::Wasm::PluginSharedPtr plugin)
      : Context(wasm, plugin) {}

  // MOCK_CONTEXT_LOG_;

  proxy_wasm::WasmResult defineMetric(uint32_t type, std::string_view name,
                                      uint32_t* metric_id_ptr) override {
    auto rs = Envoy::Extensions::Common::Wasm::Context::defineMetric(
        type, name, metric_id_ptr);
    metrics_[std::string(name)] = *metric_id_ptr;
    // scriptLog_(spdlog::level::err, absl::StrCat(name, " = ",
    // *metric_id_ptr));
    return rs;
  }

  uint64_t readMetric(std::string_view name) {
    auto mid = metrics_.find(std::string(name));
    if (mid == metrics_.end()) {
      return 0;
    }
    uint64_t cnt = 0;
    Envoy::Extensions::Common::Wasm::Context::getMetric(mid->second, &cnt);
    return cnt;
  }

 private:
  std::map<std::string, uint32_t> metrics_;
};

struct TestParams {
  std::string runtime;  // null, v8, wavm
  // In order to load wasm files we need to specify base path relative to
  // WORKSPACE.
  std::string testdata_dir;
};

// Config params
// All default values are zero values
// So name flags accordingly.
struct ConfigParams {
  std::string name;
  std::string plugin_config;
  // relative from testdata_dir
  std::string plugin_config_file;
  bool do_not_add_filter;
  std::string root_id;
};

std::ostream& operator<<(std::ostream& os, const TestParams& s) {
  return (os << "{runtime: '" << s.runtime << "', testdata_dir: '"
             << s.testdata_dir << "' }");
}

std::string readfile(std::string relative_path) {
  std::string run_dir = TestEnvironment::runfilesDirectory("io_istio_proxy");
  return TestEnvironment::readFileToStringForTest(run_dir + relative_path);
}

class WasmHttpFilterTest : public testing::TestWithParam<TestParams> {
 public:
  WasmHttpFilterTest() {}
  ~WasmHttpFilterTest() {}

  virtual void setupConfig(ConfigParams c) {
    auto params = GetParam();
    if (!c.plugin_config_file.empty()) {
      c.plugin_config =
          readfile(params.testdata_dir + "/" + c.plugin_config_file);
    }

    auto code = (params.runtime == "null")
                    ? c.name
                    : readfile(params.testdata_dir + "/" + c.name);

    WasmFilterConfig proto_config;
    proto_config.mutable_config()->mutable_vm_config()->set_vm_id("vm_id");
    proto_config.mutable_config()->mutable_vm_config()->set_runtime(
        absl::StrCat("envoy.wasm.runtime.", params.runtime));
    proto_config.mutable_config()
        ->mutable_vm_config()
        ->mutable_code()
        ->mutable_local()
        ->set_inline_bytes(code);
    ProtobufWkt::StringValue plugin_configuration;
    plugin_configuration.set_value(c.plugin_config);
    proto_config.mutable_config()->mutable_configuration()->PackFrom(
        plugin_configuration);
    Api::ApiPtr api = Api::createApiForTest(stats_store_);
    scope_ = Stats::ScopeSharedPtr(stats_store_.createScope("wasm."));

    auto vm_id = "";
    plugin_ = std::make_shared<Extensions::Common::Wasm::Plugin>(
        c.name, c.root_id, vm_id, params.runtime, c.plugin_config, false,
        TrafficDirection::INBOUND, local_info_, &listener_metadata_);
    // creates a base VM
    // This is synchronous, even though it happens thru a callback due to null
    // vm.
    Extensions::Common::Wasm::createWasm(
        proto_config.config().vm_config(), plugin_, scope_, cluster_manager_,
        init_manager_, dispatcher_, random_, *api, lifecycle_notifier_,
        remote_data_provider_,
        [this](WasmHandleSharedPtr wasm) { wasm_ = wasm; },
        [](Wasm* wasm, const std::shared_ptr<Common::Wasm::Plugin>& plugin) {
          return new TestRoot(wasm, plugin);
        });
    if (wasm_) {
      wasm_ = getOrCreateThreadLocalWasm(
          wasm_, plugin_, dispatcher_,
          [root_context = &root_context_](
              Wasm* wasm, const std::shared_ptr<Common::Wasm::Plugin>& plugin) {
            *root_context = new TestRoot(wasm, plugin);
            return *root_context;
          });
    }
    if (!c.do_not_add_filter) {
      setupFilter(c.root_id);
    }
  }

  void setupFilter(const std::string root_id = "") {
    filter_ = std::make_unique<TestFilter>(
        wasm_->wasm().get(), wasm_->wasm()->getRootContext(root_id)->id(),
        plugin_);
    filter_->setDecoderFilterCallbacks(decoder_callbacks_);
    filter_->setEncoderFilterCallbacks(encoder_callbacks_);

    ON_CALL(decoder_callbacks_.stream_info_, filterState())
        .WillByDefault(ReturnRef(request_stream_info_.filterState()));
    ON_CALL(encoder_callbacks_.stream_info_, filterState())
        .WillByDefault(ReturnRef(request_stream_info_.filterState()));
  }

  std::shared_ptr<Envoy::StreamInfo::FilterState> makeTestRequest(
      Http::TestRequestHeaderMapImpl& request_headers,
      Http::TestResponseHeaderMapImpl& response_headers,
      std::string bdata = "data") {
    auto fs = request_stream_info_.filterState();

    uint32_t response_code = 200;
    auto resp_code = response_headers.get_(":status");
    if (!resp_code.empty()) {
      EXPECT_EQ(absl::SimpleAtoi(resp_code, &response_code), true);
    }

    ON_CALL(encoder_callbacks_.stream_info_, responseCode())
        .WillByDefault(Invoke([response_code]() { return response_code; }));

    EXPECT_EQ(Http::FilterHeadersStatus::Continue,
              filter_->decodeHeaders(request_headers, true));

    Buffer::OwnedImpl data(bdata);
    EXPECT_EQ(Http::FilterDataStatus::Continue,
              filter_->decodeData(data, true));

    EXPECT_EQ(Http::FilterHeadersStatus::Continue,
              filter_->encodeHeaders(response_headers, true));

    filter_->log(&request_headers, nullptr, nullptr, request_stream_info_);
    return fs;
  }

  // Many of the following are not used yet, but are useful
  Stats::IsolatedStoreImpl stats_store_;
  Stats::ScopeSharedPtr scope_;
  NiceMock<ThreadLocal::MockInstance> tls_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  NiceMock<Random::MockRandomGenerator> random_;
  NiceMock<Upstream::MockClusterManager> cluster_manager_;
  NiceMock<Init::MockManager> init_manager_;
  WasmHandleSharedPtr wasm_;
  PluginSharedPtr plugin_;
  std::unique_ptr<TestFilter> filter_;
  NiceMock<Envoy::Ssl::MockConnectionInfo> ssl_;
  NiceMock<Envoy::Network::MockConnection> connection_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_;
  NiceMock<Http::MockStreamEncoderFilterCallbacks> encoder_callbacks_;
  NiceMock<Envoy::StreamInfo::MockStreamInfo> request_stream_info_;
  NiceMock<LocalInfo::MockLocalInfo> local_info_;
  NiceMock<Server::MockServerLifecycleNotifier> lifecycle_notifier_;
  envoy::config::core::v3::Metadata listener_metadata_;
  TestRoot* root_context_ = nullptr;
  Config::DataSource::RemoteAsyncDataProviderPtr remote_data_provider_;
};

}  // namespace Wasm
}  // namespace HttpFilters

namespace Common {
namespace Wasm {
namespace Null {
namespace Plugin {
namespace AttributeGen {
using HttpFilters::Wasm::ConfigParams;
using HttpFilters::Wasm::TestParams;
using HttpFilters::Wasm::WasmHttpFilterTest;

std::vector<TestParams> generateTestParams() {
  return std::vector<TestParams>{
      {.runtime = "null", .testdata_dir = "/extensions/attributegen/testdata"},
      // {.runtime = "v8", .testdata_dir =
      // "/extensions/attributegen/testdata"},
  };
}

class AttributeGenFilterTest : public WasmHttpFilterTest {
 public:
  void verifyRequest(Http::TestRequestHeaderMapImpl& request_headers,
                     Http::TestResponseHeaderMapImpl& response_headers,
                     const std::string& base_attribute, bool found,
                     const std::string& value = "") {
    auto fs = makeTestRequest(request_headers, response_headers);
    auto attribute = "wasm." + base_attribute;

    ASSERT_EQ(fs->hasData<WasmState>(attribute), found)
        << absl::StrCat(attribute, "=?", value);
    if (found) {
      ASSERT_EQ(fs->getDataReadOnly<WasmState>(attribute).value(), value)
          << absl::StrCat(attribute, "=?", value);
    }
  }

  void setupConfig(ConfigParams c) override {
    if (c.name.empty()) {
      c.name = "envoy.wasm.attributegen";
    }

    WasmHttpFilterTest::setupConfig(c);
  }
};

INSTANTIATE_TEST_SUITE_P(Runtimes, AttributeGenFilterTest,
                         testing::ValuesIn(generateTestParams()));

TEST_P(AttributeGenFilterTest, OneMatch) {
  const std::string attribute = "istio.operationId";
  const char* plugin_config = R"EOF(
                    {"attributes": [{"output_attribute": "istio.operationId",
                    "match": [{"value":
                            "GetStatus", "condition": "request.url_path.startsWith('/status')"}]}]}
  )EOF";
  setupConfig({.plugin_config = plugin_config});

  Http::TestRequestHeaderMapImpl request_headers{{":path", "/status/207"}};
  Http::TestResponseHeaderMapImpl response_headers{{":status", "404"}};

  verifyRequest(request_headers, response_headers, attribute, true,
                "GetStatus");
}

TEST_P(AttributeGenFilterTest, ExprEvalError) {
  const std::string attribute = "istio.operationId";
  const char* plugin_config = R"EOF(
                    {"attributes": [{"output_attribute": "istio.operationId",
                    "match": [{"value":
                            "GetStatus", "condition": "request.url_path"}]}]}
  )EOF";
  setupConfig({.plugin_config = plugin_config});

  Http::TestRequestHeaderMapImpl request_headers{{":path", "/status/207"}};
  Http::TestResponseHeaderMapImpl response_headers{{":status", "404"}};

  verifyRequest(request_headers, response_headers, attribute, false);
}

TEST_P(AttributeGenFilterTest, UnparseableConfig) {
  const char* plugin_config = R"EOF(
                    attributes = [ output_attribute ];
  )EOF";
  setupConfig({.plugin_config = plugin_config});
  EXPECT_EQ(root_context_->readMetric(
                "wasm_filter.attributegen.type.config.error_count"),
            2);
}

TEST_P(AttributeGenFilterTest, BadExpr) {
  const char* plugin_config = R"EOF(
                    {"attributes": [{"output_attribute": "istio.operationId",
                    "match": [{"value":
                            "GetStatus", "condition": "if a = b then return
                            5"}]}]}
  )EOF";
  setupConfig({.plugin_config = plugin_config});
  EXPECT_EQ(root_context_->readMetric(
                "wasm_filter.attributegen.type.config.error_count"),
            2);
}

TEST_P(AttributeGenFilterTest, NoMatch) {
  const std::string attribute = "istio.operationId";
  const char* plugin_config = R"EOF(
                    {"attributes": [{"output_attribute": "istio.operationId",
                    "match": [{"value":
                            "GetStatus", "condition":
                            "request.url_path.startsWith('/status') &&
                            request.method == 'POST'"}]}]}
  )EOF";
  setupConfig({.plugin_config = plugin_config});

  Http::TestRequestHeaderMapImpl request_headers{{":path", "/status/207"},
                                                 {":method", "GET"}};
  Http::TestResponseHeaderMapImpl response_headers{{":status", "404"}};

  verifyRequest(request_headers, response_headers, attribute, false);
}

TEST_P(AttributeGenFilterTest, OperationFileList) {
  const std::string attribute = "istio.operationId";

  setupConfig(
      {.plugin_config_file = "operation.json"});  // testdata/operation.json

  Http::TestRequestHeaderMapImpl request_headers{{":path", "/books"},
                                                 {":method", "GET"}};
  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};

  verifyRequest(request_headers, response_headers, attribute, true,
                "ListBooks");
}

TEST_P(AttributeGenFilterTest, OperationFileListNoMatch) {
  const std::string attribute = "istio.operationId";

  setupConfig(
      {.plugin_config_file = "operation.json"});  // testdata/operation.json

  // needs GET to match
  Http::TestRequestHeaderMapImpl request_headers{{":path", "/books"},
                                                 {":method", "POST"}};
  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};

  verifyRequest(request_headers, response_headers, attribute, false);
}

TEST_P(AttributeGenFilterTest, OperationFileGet) {
  const std::string attribute = "istio.operationId";

  setupConfig(
      {.plugin_config_file = "operation.json"});  // testdata/operation.json

  Http::TestRequestHeaderMapImpl request_headers{
      {":path", "/shelves/a101/books/b1122"}, {":method", "GET"}};
  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};

  verifyRequest(request_headers, response_headers, attribute, true, "GetBook");
}

TEST_P(AttributeGenFilterTest, OperationFileGetNoMatch) {
  const std::string attribute = "istio.operationId";

  setupConfig(
      {.plugin_config_file = "operation.json"});  // testdata/operation.json
  // match requires alphanumeric ids.
  Http::TestRequestHeaderMapImpl request_headers{
      {":path", "/shelves/-----/books/b1122"}, {":method", "GET"}};
  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};

  verifyRequest(request_headers, response_headers, attribute, false, "GetBook");
}

TEST_P(AttributeGenFilterTest, ResponseCodeFileMatch1) {
  const std::string attribute = "istio.responseClass";

  setupConfig({.plugin_config_file =
                   "responseCode.json"});  // testdata/responseCode.json

  Http::TestRequestHeaderMapImpl request_headers{{":path", "/books"},
                                                 {":method", "GET"}};
  Http::TestResponseHeaderMapImpl response_headers{{":status", "207"}};

  verifyRequest(request_headers, response_headers, attribute, true, "2xx");
}

TEST_P(AttributeGenFilterTest, ResponseCodeFileMatch2) {
  const std::string attribute = "istio.responseClass";

  setupConfig({.plugin_config_file =
                   "responseCode.json"});  // testdata/responseCode.json

  Http::TestRequestHeaderMapImpl request_headers{{":path", "/books"},
                                                 {":method", "GET"}};
  Http::TestResponseHeaderMapImpl response_headers{{":status", "404"}};
  // 404 is not classified.
  verifyRequest(request_headers, response_headers, attribute, true, "404");
}

TEST_P(AttributeGenFilterTest, ResponseCodeFileMatch3) {
  const std::string attribute = "istio.responseClass";

  setupConfig({.plugin_config_file =
                   "responseCode.json"});  // testdata/responseCode.json

  Http::TestRequestHeaderMapImpl request_headers{{":path", "/books"},
                                                 {":method", "GET"}};
  Http::TestResponseHeaderMapImpl response_headers{{":status", "504"}};
  verifyRequest(request_headers, response_headers, attribute, true, "5xx");
}

}  // namespace AttributeGen

// WASM_EPILOG
#ifdef NULL_PLUGIN
}  // namespace Plugin
}  // namespace Null
}  // namespace Wasm
}  // namespace Common
}  // namespace Extensions
}  // namespace Envoy
#endif
