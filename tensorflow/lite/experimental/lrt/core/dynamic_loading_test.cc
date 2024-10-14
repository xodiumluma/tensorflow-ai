// Copyright 2024 Google LLC.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tensorflow/lite/experimental/lrt/core/dynamic_loading.h"

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "testing/base/public/unique-test-directory.h"
#include "absl/strings/string_view.h"
#include "tensorflow/lite/experimental/lrt/test/common.h"

namespace {

using ::lrt::testing::TouchTestFile;

constexpr absl::string_view kNotLrtSo = "notLibLrt.so";
constexpr absl::string_view kLrtSo1 = "libLrtPlugin_1.so";
constexpr absl::string_view kLrtSo2 = "libLrtPlugin_2.so";

TEST(TestDynamicLoading, GlobNoMatch) {
  const auto dir = testing::UniqueTestDirectory();
  TouchTestFile(kNotLrtSo, dir);

  std::vector<std::string> results;
  ASSERT_STATUS_OK(lrt::FindLrtSharedLibs(dir, results));
  EXPECT_EQ(results.size(), 0);
}

TEST(TestDynamicLoading, GlobOneMatch) {
  const auto dir = testing::UniqueTestDirectory();
  TouchTestFile(kLrtSo1, dir);
  TouchTestFile(kNotLrtSo, dir);

  std::vector<std::string> results;
  ASSERT_STATUS_OK(lrt::FindLrtSharedLibs(dir, results));
  EXPECT_EQ(results.size(), 1);
  EXPECT_TRUE(absl::string_view(results.front()).ends_with(kLrtSo1));
}

TEST(TestDynamicLoading, GlobMultiMatch) {
  const auto dir = testing::UniqueTestDirectory();
  TouchTestFile(kLrtSo1, dir);
  TouchTestFile(kLrtSo2, dir);
  TouchTestFile(kNotLrtSo, dir);

  std::vector<std::string> results;
  ASSERT_STATUS_OK(lrt::FindLrtSharedLibs(dir, results));
  EXPECT_EQ(results.size(), 2);
  EXPECT_THAT(results, testing::Contains(testing::HasSubstr(kLrtSo1)));
  EXPECT_THAT(results, testing::Contains(testing::HasSubstr(kLrtSo2)));
}

}  // namespace
