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

#pragma once

#include "absl/strings/str_cat.h"
#include "envoy/config/core/v3/grpc_service.pb.h"
#include "extensions/common/context.h"
#include "google/api/monitored_resource.pb.h"
#include "grpcpp/grpcpp.h"

namespace Extensions {
namespace Stackdriver {
namespace Common {

// StackdriverStubOption includes all the configuration to construct stackdriver
// gRPC stubs.
struct StackdriverStubOption {
  std::string sts_port;
  std::string default_endpoint;
  std::string test_token_path;
  std::string test_root_pem_path;
  std::string secure_endpoint;
  std::string insecure_endpoint;
  std::string monitoring_endpoint;
  std::string project_id;
};

// Build Envoy GrpcService proto based on the given stub option.
void buildEnvoyGrpcService(
    const StackdriverStubOption &option,
    ::envoy::config::core::v3::GrpcService *grpc_service);

// Returns "owner" information for a node. If that information
// has been directly set, that value is returned. If not, and the owner
// can be entirely derived from platform metadata, this will derive the
// owner. Currently, this is only supported for GCE Instances. For
// anything else, this will return the empty string.
std::string getOwner(const ::Wasm::Common::FlatNode &node);

// Gets monitored resource proto based on the type and node metadata info.
// Only two types of monitored resource could be returned: k8s_container or
// k8s_pod.
void getMonitoredResource(const std::string &monitored_resource_type,
                          const ::Wasm::Common::FlatNode &local_node_info,
                          google::api::MonitoredResource *monitored_resource);

// Set secure exchange service gRPC call credential.
void setSTSCallCredentialOptions(
    ::envoy::config::core::v3::GrpcService_GoogleGrpc_CallCredentials_StsService
        *sts_service,
    const std::string &sts_port, const std::string &token_path);
void setSTSCallCredentialOptions(
    ::grpc::experimental::StsCredentialsOptions *sts_options,
    const std::string &sts_port, const std::string &token_path);

}  // namespace Common
}  // namespace Stackdriver
}  // namespace Extensions
