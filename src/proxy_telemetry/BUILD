package(default_visibility = ["//visibility:public"])

load("@envoy//bazel:envoy_build_system.bzl", "envoy_cc_library")

envoy_cc_library(
    name = "client",
    hdrs = [
        "client.h",
    ],
    repository = "@envoy",
    deps = [
        "@envoy//include/envoy/local_info:local_info_interface",
        "@envoy//include/envoy/grpc:async_client_manager_interface",
        "@envoy//source/common/grpc:typed_async_client_lib",
    ]
)

envoy_cc_library(
    name = "client_http",
    srcs = [
        "client_http.cc",
    ],
    hdrs = [
        "client_http.h",
    ],
    repository = "@envoy",
    deps = [
        ":client",
        ":pkg_cc_proto",
    ]
)

envoy_cc_library(
    name = "client_tcp",
    srcs = [
        "client_tcp.cc",
    ],
    hdrs = [
        "client_tcp.h",
    ],
    repository = "@envoy",
    deps = [
        ":client",
        "@envoy_api//envoy/service/accesslog/v3:pkg_cc_proto",
    ]
)

load("@envoy_api//bazel:api_build_system.bzl", "api_proto_package")

licenses(["notice"])  # Apache 2

api_proto_package(
    has_services = True,
    deps = [
        "@envoy_api//envoy/data/accesslog/v3:pkg",
        "@envoy_api//envoy/config/core/v3:pkg",
    ],
)
