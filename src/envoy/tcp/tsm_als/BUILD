load(
    "@envoy//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
)
load("@envoy_api//bazel:api_build_system.bzl", "api_cc_py_proto_library")

api_cc_py_proto_library(
    name = "config",
    srcs = ["config.proto"],
    deps = [
        "@envoy_api//envoy/config/core/v3:pkg",
    ],
)

envoy_cc_library(
    name = "filter_factory",
    srcs = ["filter_factory.cc"],
    repository = "@envoy",
    visibility = ["//visibility:public"],
    deps = [
        ":filter",
        "@envoy//envoy/registry",
        "@envoy//source/extensions/filters/network/common:factory_base_lib",
    ],
)

envoy_cc_library(
    name = "filter",
    srcs = [
        "config.cc",
        "filter.cc",
    ],
    hdrs = [
        "config.h",
        "filter.h",
    ],
    repository = "@envoy",
    deps = [
        ":config_cc_proto",
        "//src/proxy_telemetry:client_tcp",
        "@envoy//envoy/network:filter_interface",
        "@envoy//envoy/server:filter_config_interface",
        "@envoy//source/common/protobuf:utility_lib",
        "@envoy//source/extensions/access_loggers/grpc:grpc_access_log_utils",
    ],
)
