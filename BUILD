# Copyright 2021 curoky(cccuroky@gmail.com).
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

load("@rules_cc//cc:defs.bzl", "cc_library")
load("//:build/bazel/copts.bzl", "DEFAULT_CPP_COPTS")

cc_library(
    name = "wangle",
    srcs = glob(
        ["wangle/**/*.cpp"],
        exclude = [
            "wangle/**/test/**/*.cpp",
            "wangle/**/example/**/*.cpp",
        ],
    ),
    hdrs = glob(["wangle/**/*.h"]),
    copts = DEFAULT_CPP_COPTS,
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        "//source/folly",
        "@com_github_facebookincubator_fizz//:fizz",
    ],
)
