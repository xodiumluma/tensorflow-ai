/* Copyright 2019 The OpenXLA Authors.

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

// Defines the GpuStream type - the CUDA-specific implementation of the generic
// StreamExecutor Stream interface.

#ifndef XLA_STREAM_EXECUTOR_GPU_GPU_STREAM_H_
#define XLA_STREAM_EXECUTOR_GPU_GPU_STREAM_H_

#include <memory>
#include <optional>
#include <variant>

#include "absl/functional/any_invocable.h"
#include "absl/log/check.h"
#include "absl/strings/string_view.h"
#include "xla/stream_executor/event_based_timer.h"
#include "xla/stream_executor/gpu/gpu_executor.h"
#include "xla/stream_executor/gpu/gpu_types.h"
#include "xla/stream_executor/kernel.h"
#include "xla/stream_executor/launch_dim.h"
#include "xla/stream_executor/platform.h"
#include "xla/stream_executor/stream.h"
#include "xla/stream_executor/stream_common.h"

namespace stream_executor {
namespace gpu {

// Wraps a GpuStreamHandle in order to satisfy the platform-independent
// StreamInterface.
//
// Thread-safe post-initialization.
class GpuStream : public StreamCommon {
 public:
  GpuStream(GpuExecutor* parent,
            std::optional<std::variant<StreamPriority, int>> priority,
            GpuStreamHandle gpu_stream)
      : StreamCommon(parent), parent_(parent), gpu_stream_(gpu_stream) {
    if (priority.has_value()) {
      stream_priority_ = priority.value();
    }
  }

  // Note: teardown is handled by a parent's call to DeallocateStream.
  ~GpuStream() override;

  std::variant<StreamPriority, int> priority() const override {
    return stream_priority_;
  }
  PlatformSpecificHandle platform_specific_handle() const override;

  // Returns the GpuStreamHandle value for passing to the CUDA API.
  //
  // Precond: this GpuStream has been allocated (otherwise passing a nullptr
  // into the NVIDIA library causes difficult-to-understand faults).
  GpuStreamHandle gpu_stream() const {
    DCHECK(gpu_stream_ != nullptr);
    return gpu_stream_;
  }

  absl::Status DoHostCallbackWithStatus(
      absl::AnyInvocable<absl::Status() &&> callback) override;

  void set_name(absl::string_view name) override;
  absl::StatusOr<std::unique_ptr<EventBasedTimer>> CreateEventBasedTimer(
      bool use_delay_kernel) override;
  absl::Status Launch(const ThreadDim& thread_dims, const BlockDim& block_dims,
                      const Kernel& k, const KernelArgs& args) override;
  absl::Status Launch(const ThreadDim& thread_dims, const BlockDim& block_dims,
                      const ClusterDim& cluster_dims, const Kernel& k,
                      const KernelArgs& args) override;

 private:
  // Helper method to launch a kernel with optional cluster dimensions.
  absl::Status Launch(const ThreadDim& thread_dims, const BlockDim& block_dims,
                      const std::optional<ClusterDim>& cluster_dims,
                      const Kernel& kernel, const KernelArgs& args);

  GpuExecutor* parent_;         // Executor that spawned this stream.
  GpuStreamHandle gpu_stream_;  // Wrapped CUDA stream handle.
  std::variant<StreamPriority, int> stream_priority_;
};

// Helper functions to simplify extremely common flows.
// Converts a Stream to the underlying GpuStream implementation.
GpuStream* AsGpuStream(Stream* stream);

// Extracts a GpuStreamHandle from a GpuStream-backed Stream object.
GpuStreamHandle AsGpuStreamValue(Stream* stream);
}  // namespace gpu
}  // namespace stream_executor

#endif  // XLA_STREAM_EXECUTOR_GPU_GPU_STREAM_H_
