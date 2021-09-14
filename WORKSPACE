# Copyright 2021 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# This file uses repository rules to fetch external dependencies.
#
# We require that all uses of repository rules are effectively deterministic -
# they may fail, but otherwise must always produce exactly the same output for
# given parameters. For the http_archive rule, this means that you must specify
# a sha256 parameter; consequently, make sure to use stable URLs, rather than
# e.g. one that names a git branch.
#
# This is so that all(*) inputs to the build are fully determined (i.e. a
# change to build inputs requires a change to our sources), which avoids
# confusing outcomes from caching. If it is ever productive to clear your Bazel
# cache, that's a bug.
#
# (*) A Bazel build depends on local compiler toolchains (and Bazel itself), so
# it can be useful to pick a particular container image too (like some version
# of http://l.gcr.io/bazel).
#
# The repository namespace
# ------------------------
#
# Bazel's handling of @repository_names// is very broken. There is a single,
# global namespace for repositories. Conflicts are silently ignored. This is
# problematic for common dependencies. As much as possible, we use TensorFlow's
# dependencies. Things become especially difficult if we try to use our own
# version of grpc or protobuf. Current overrides:
#
#  - @com_github_gflags_gflags: tf uses 2.2.1 and we need 2.2.2 apparently.
#  - @com_google_googletest: Need to patch in support for absl flags

workspace(name = "com_google_fcp")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# GoogleTest/GoogleMock framework. Used by most unit-tests.
http_archive(
    name = "com_google_googletest",
    patches = ["//fcp/patches:googletest.patch"],
    sha256 = "fcfac631041fce253eba4fc014c28fd620e33e3758f64f8ed5487cc3e1840e3d",
    strip_prefix = "googletest-5a509dbd2e5a6c694116e329c5a20dc190653724",
    urls = ["https://github.com/google/googletest/archive/5a509dbd2e5a6c694116e329c5a20dc190653724.zip"],
)

http_archive(
    name = "com_github_gflags_gflags",
    sha256 = "34af2f15cf7367513b352bdcd2493ab14ce43692d2dcd9dfc499492966c64dcf",
    strip_prefix = "gflags-2.2.2",
    urls = ["https://github.com/gflags/gflags/archive/v2.2.2.tar.gz"],
)

http_archive(
    name = "com_github_google_glog",
    sha256 = "21bc744fb7f2fa701ee8db339ded7dce4f975d0d55837a97be7d46e8382dea5a",
    strip_prefix = "glog-0.5.0",
    urls = ["https://github.com/google/glog/archive/v0.5.0.zip"],
)

# When this cl was made, v2.5 is the most stable release, but had an older
# version of absl pinned. HEAD at time of writing has the same version of absl
# we depend on, so we use tf at this commit.
http_archive(
    name = "org_tensorflow",
    sha256 = "385484c7ae50263e718c36da358db321314836212623d313a94c41f5ec463b6d",
    strip_prefix = "tensorflow-0b562f8f44077e01e0c4b2ae4e8959376427ef0b",
    patches = [
        # This patch removes an import generated by the old version of
        # googletest that TensorFlow uses, and isn't generated by the newer
        # version of googletest that we use.
        "//fcp/patches:tensorflow_googletest.patch",
    ],
    patch_tool = "patch",
    urls = [
        "https://github.com/tensorflow/tensorflow/archive/0b562f8f44077e01e0c4b2ae4e8959376427ef0b.tar.gz",
    ],
)

# The following is copied from TensorFlow's own WORKSPACE, see
# https://github.com/tensorflow/tensorflow/blob/v2.5.0/WORKSPACE#L9

load("@org_tensorflow//tensorflow:workspace3.bzl", "tf_workspace3")

tf_workspace3()

load("@org_tensorflow//tensorflow:workspace2.bzl", "tf_workspace2")

tf_workspace2()

load("@org_tensorflow//tensorflow:workspace1.bzl", "tf_workspace1")

tf_workspace1()

load("@org_tensorflow//tensorflow:workspace0.bzl", "tf_workspace0")

tf_workspace0()

http_archive(
    name = "com_google_absl_py",
    sha256 = "0d3aa64ef42ef592d5cf12060082397d01291bfeb1ac7b6d9dcfb32a07fff311",
    strip_prefix = "abseil-py-9954557f9df0b346a57ff82688438c55202d2188",
    urls = [
        "https://github.com/abseil/abseil-py/archive/9954557f9df0b346a57ff82688438c55202d2188.tar.gz",
    ],
)

http_archive(
    name = "com_google_benchmark",
    sha256 = "23082937d1663a53b90cb5b61df4bcc312f6dee7018da78ba00dd6bd669dfef2",
    strip_prefix = "benchmark-1.5.1",
    urls = [
        "https://github.com/google/benchmark/archive/v1.5.1.tar.gz",
    ],
)

# Cpp ProtoDataStore
http_archive(
    name = "protodatastore_cpp",
    sha256 = "d2627231fce0c9944100812b42d33203f7e03e78215d10123e78041b491005c3",
    strip_prefix = "protodatastore-cpp-0cd8b124bc65bcac105bce4a706923218ae5d625",
    url = "https://github.com/google/protodatastore-cpp/archive/0cd8b124bc65bcac105bce4a706923218ae5d625.zip",
)
