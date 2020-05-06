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

#include "extensions/stackdriver/common/utils.h"

#include "extensions/stackdriver/common/constants.h"
#include "grpcpp/grpcpp.h"

namespace Extensions {
namespace Stackdriver {
namespace Common {

namespace {

const std::string getContainerName(
    const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>
        *containers) {
  if (containers && containers->size() == 1) {
    return flatbuffers::GetString(containers->Get(0));
  }

  return kIstioProxyContainerName;
}

}  // namespace

using google::api::MonitoredResource;

void buildEnvoyGrpcService(
    const StackdriverStubOption &stub_option,
    ::envoy::config::core::v3::GrpcService *grpc_service) {
  if (!stub_option.insecure_endpoint.empty()) {
    // Do not set up credential if insecure endpoint is provided. This is only
    // for testing.
    grpc_service->mutable_google_grpc()->set_target_uri(
        stub_option.insecure_endpoint);
  } else {
    grpc_service->mutable_google_grpc()->set_target_uri(
        stub_option.secure_endpoint.empty() ? stub_option.default_endpoint
                                            : stub_option.secure_endpoint);
    if (stub_option.sts_port.empty()) {
      // Security token exchange is not enabled. Use default GCE credential.
      grpc_service->mutable_google_grpc()
          ->add_call_credentials()
          ->mutable_google_compute_engine();
    } else {
      ::Extensions::Stackdriver::Common::setSTSCallCredentialOptions(
          grpc_service->mutable_google_grpc()
              ->add_call_credentials()
              ->mutable_sts_service(),
          stub_option.sts_port,
          stub_option.test_token_path.empty()
              ? ::Extensions::Stackdriver::Common::kSTSSubjectTokenPath
              : stub_option.test_token_path);
      auto initial_metadata = grpc_service->add_initial_metadata();
      initial_metadata->set_key("x-goog-user-project");
      initial_metadata->set_value(stub_option.project_id);
    }

    grpc_service->mutable_google_grpc()
        ->mutable_channel_credentials()
        ->mutable_ssl_credentials()
        ->mutable_root_certs()
        ->set_filename(
            stub_option.test_root_pem_path.empty()
                ? ::Extensions::Stackdriver::Common::kDefaultRootCertFile
                : stub_option.test_root_pem_path);
  }
}

std::string getOwner(const ::Wasm::Common::FlatNode &node) {
  // do not override supplied owner
  if (node.owner()) {
    return flatbuffers::GetString(node.owner());
  }

  auto platform_metadata = node.platform_metadata();
  if (!platform_metadata) {
    return "";
  }

  // only attempt for GCE Instances at this point, first check for MIG.
  auto created_by = platform_metadata->LookupByKey(kGCECreatedByKey.data());
  if (created_by) {
    return absl::StrCat("//compute.googleapis.com/",
                        created_by->value()->string_view());
  }

  // then handle unmanaged GCE Instance case
  auto instance_id = platform_metadata->LookupByKey(kGCPGCEInstanceIDKey);
  auto project = platform_metadata->LookupByKey(kGCPProjectNumberKey.data());
  auto location = platform_metadata->LookupByKey(kGCPLocationKey);
  if (instance_id && project && location) {
    // Should be of the form:
    // //compute.googleapis.com/projects/%s/zones/%s/instances/%s
    return absl::StrCat("//compute.googleapis.com/projects/",
                        project->value()->string_view(), "/zones/",
                        location->value()->string_view(), "/instances/",
                        instance_id->value()->string_view());
  }

  return "";
}

void getMonitoredResource(const std::string &monitored_resource_type,
                          const ::Wasm::Common::FlatNode &local_node_info,
                          MonitoredResource *monitored_resource) {
  if (!monitored_resource) {
    return;
  }

  monitored_resource->set_type(monitored_resource_type);
  auto platform_metadata = local_node_info.platform_metadata();

  if (platform_metadata) {
    auto project_key = platform_metadata->LookupByKey(kGCPProjectKey);
    if (project_key) {
      (*monitored_resource->mutable_labels())[kProjectIDLabel] =
          flatbuffers::GetString(project_key->value());
    }
  }

  if (monitored_resource_type == kGCEInstanceMonitoredResource) {
    // gce_instance
    if (platform_metadata) {
      auto instance_id_label =
          platform_metadata->LookupByKey(kGCPGCEInstanceIDKey);
      if (instance_id_label) {
        (*monitored_resource->mutable_labels())[kGCEInstanceIDLabel] =
            flatbuffers::GetString(instance_id_label->value());
      }
      auto zone_label = platform_metadata->LookupByKey(kGCPLocationKey);
      if (zone_label) {
        (*monitored_resource->mutable_labels())[kZoneLabel] =
            flatbuffers::GetString(zone_label->value());
      }
    }
  } else {
    // k8s_pod or k8s_container
    if (platform_metadata) {
      auto location_label = platform_metadata->LookupByKey(kGCPLocationKey);
      if (location_label) {
        (*monitored_resource->mutable_labels())[kLocationLabel] =
            flatbuffers::GetString(location_label->value());
      }
      auto cluster_name = platform_metadata->LookupByKey(kGCPClusterNameKey);
      if (cluster_name) {
        (*monitored_resource->mutable_labels())[kClusterNameLabel] =
            flatbuffers::GetString(cluster_name->value());
      }
    }

    (*monitored_resource->mutable_labels())[kNamespaceNameLabel] =
        flatbuffers::GetString(local_node_info.namespace_());
    (*monitored_resource->mutable_labels())[kPodNameLabel] =
        flatbuffers::GetString(local_node_info.name());

    if (monitored_resource_type == kContainerMonitoredResource) {
      // Fill in container_name of k8s_container monitored resource.
      auto container = getContainerName(local_node_info.app_containers());
      (*monitored_resource->mutable_labels())[kContainerNameLabel] = container;
    }
  }
}

void setSTSCallCredentialOptions(
    ::envoy::config::core::v3::GrpcService_GoogleGrpc_CallCredentials_StsService
        *sts_service,
    const std::string &sts_port, const std::string &token_path) {
  if (!sts_service) {
    return;
  }
  sts_service->set_token_exchange_service_uri("http://localhost:" + sts_port +
                                              "/token");
  sts_service->set_subject_token_path(token_path);
  sts_service->set_subject_token_type(kSTSSubjectTokenType);
  sts_service->set_scope(kSTSScope);
}

void setSTSCallCredentialOptions(
    ::grpc::experimental::StsCredentialsOptions *sts_options,
    const std::string &sts_port, const std::string &token_path) {
  if (!sts_options) {
    return;
  }
  sts_options->token_exchange_service_uri =
      "http://localhost:" + sts_port + "/token";
  sts_options->subject_token_path = token_path;
  sts_options->subject_token_type = kSTSSubjectTokenType;
  sts_options->scope = kSTSScope;
}

}  // namespace Common
}  // namespace Stackdriver
}  // namespace Extensions
