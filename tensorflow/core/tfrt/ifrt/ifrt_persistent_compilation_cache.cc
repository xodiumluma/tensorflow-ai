/* Copyright 2024 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/core/tfrt/ifrt/ifrt_persistent_compilation_cache.h"

#include <memory>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xla/pjrt/pjrt_executable.h"
#include "xla/python/ifrt/array.h"
#include "xla/python/ifrt/device_list.h"
#include "xla/python/ifrt/executable.h"
#include "xla/python/ifrt/hlo/hlo_program.h"
#include "xla/python/ifrt/host_callback.h"
#include "xla/python/ifrt/program.h"
#include "xla/tsl/concurrency/ref_count.h"

namespace tensorflow {
namespace ifrt_serving {

absl::StatusOr<std::unique_ptr<xla::ifrt::LoadedExecutable>>
IfrtPersistentCompilationCache::LookupLoadedExecutableOrCreate(
    std::unique_ptr<xla::ifrt::HloProgram> hlo_program,
    tsl::RCReference<xla::ifrt::DeviceList> device_list,
    const xla::CompileOptions& xla_compile_options,
    const std::vector<tsl::RCReference<xla::ifrt::LoadedHostCallback>>&
        loaded_host_callbacks,
    xla::ifrt::Client* client,
    absl::AnyInvocable<
        absl::StatusOr<std::unique_ptr<xla::ifrt::LoadedExecutable>>(
            std::unique_ptr<xla::ifrt::Program> program,
            std::unique_ptr<xla::ifrt::CompileOptions> options)>
        value_fn) {
  return absl::UnimplementedError("LookupLoadedExecutable is not implemented");
}

}  // namespace ifrt_serving
}  // namespace tensorflow
