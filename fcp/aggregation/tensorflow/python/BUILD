# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

load("@org_tensorflow//tensorflow:tensorflow.bzl", "pybind_extension")
load("@rules_python//python:defs.bzl", "py_test")

package(
    default_visibility = ["//fcp/aggregation:internal"],
    licenses = ["notice"],  # Apache 2.0
)

pybind_extension(
    name = "aggregation_protocols",
    srcs = ["aggregation_protocols.cc"],
    pytype_deps = [
        "//fcp/aggregation/protocol/python:aggregation_protocol",
    ],
    deps = [
        "//fcp/aggregation/protocol:aggregation_protocol",
        "//fcp/aggregation/protocol:configuration_cc_proto",
        "//fcp/aggregation/protocol:resource_resolver",
        "//fcp/aggregation/protocol/simple_aggregation",
        "//fcp/aggregation/tensorflow:tensorflow_checkpoint_builder_factory",
        "//fcp/aggregation/tensorflow:tensorflow_checkpoint_parser_factory",
        "@com_google_absl//absl/status:statusor",
        "@pybind11_abseil//pybind11_abseil:status_casters",
        "@pybind11_protobuf//pybind11_protobuf:native_proto_caster",
    ],
)

py_test(
    name = "aggregation_protocols_test",
    srcs = ["aggregation_protocols_test.py"],
    data = ["@pybind11_abseil//pybind11_abseil:status.so"],
    deps = [
        ":aggregation_protocols",
        "//fcp/aggregation/core:tensor_py_pb2",
        "//fcp/aggregation/protocol:configuration_py_pb2",
        "//fcp/aggregation/protocol:py_pb2",
        "@pypi_absl_py//:pkg",
        "@pypi_tensorflow//:pkg",
    ],
)
