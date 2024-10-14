/* Copyright 2024 The OpenXLA Authors.

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

#include "xla/stream_executor/cuda/cuda_stream.h"

#include <stdalign.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

#include "absl/base/casts.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "third_party/gpus/cuda/include/cuda.h"
#include "xla/stream_executor/cuda/cuda_context.h"
#include "xla/stream_executor/cuda/cuda_event.h"
#include "xla/stream_executor/cuda/cuda_status.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/event.h"
#include "xla/stream_executor/gpu/context.h"
#include "xla/stream_executor/gpu/gpu_executor.h"
#include "xla/stream_executor/gpu/scoped_activate_context.h"
#include "xla/stream_executor/platform.h"
#include "xla/stream_executor/stream.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/statusor.h"

namespace stream_executor {
namespace gpu {

namespace {
absl::Status WaitStreamOnEvent(Context* context, CUstream stream,
                               CUevent event) {
  ScopedActivateContext activation(context);
  return cuda::ToStatus(cuStreamWaitEvent(stream, event, 0 /* = flags */));
}

absl::Status RecordEvent(Context* context, CUevent event, CUstream stream) {
  ScopedActivateContext activated{context};
  return cuda::ToStatus(cuEventRecord(event, stream),
                        "Error recording CUDA event");
}

int GetGpuStreamPriority(Context* context,
                         stream_executor::StreamPriority stream_priority) {
  ScopedActivateContext activation(context);
  if (stream_priority == stream_executor::StreamPriority::Default) {
    return 0;
  }
  int lowest, highest;
  auto status = cuda::ToStatus(cuCtxGetStreamPriorityRange(&lowest, &highest));
  if (!status.ok()) {
    LOG(ERROR)
        << "Could not query stream priority range. Returning default priority.";
    return 0;
  }
  return stream_priority == stream_executor::StreamPriority::Highest ? highest
                                                                     : lowest;
}

absl::StatusOr<CUstream> CreateStream(Context* context, int priority) {
  ScopedActivateContext activated(context);
  CUstream stream;
  // If the priority is 0, then use the previous api to create the stream with
  // the default priority for backward compatibility. Probably there is no
  // difference in using the new api call but leaving it as is for now.
  if (priority == 0) {
    TF_RETURN_IF_ERROR(
        cuda::ToStatus(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING)));
  } else {
    TF_RETURN_IF_ERROR(cuda::ToStatus(
        cuStreamCreateWithPriority(&stream, CU_STREAM_NON_BLOCKING, priority)));
  }

  VLOG(2) << "successfully created stream " << stream << " for context "
          << context << " on thread";
  return stream;
}

absl::StatusOr<bool> StreamIsCapturing(CUstream stream) {
  VLOG(2) << "Checking if stream " << stream << " is capturing";

  CUstreamCaptureStatus status;
  TF_RETURN_IF_ERROR(cuda::ToStatus(cuStreamIsCapturing(stream, &status),
                                    "Failed to check stream capturing status"));

  return status == CU_STREAM_CAPTURE_STATUS_ACTIVE;
}

absl::Status AsynchronousMemcpyD2H(Context* context, void* host_dst,
                                   CUdeviceptr gpu_src, uint64_t size,
                                   CUstream stream) {
  ScopedActivateContext activation(context);

  TF_RETURN_IF_ERROR(
      cuda::ToStatus(cuMemcpyDtoHAsync(host_dst, gpu_src, size, stream)));

  VLOG(2) << "successfully enqueued async memcpy d2h of " << size
          << " bytes from " << absl::bit_cast<void*>(gpu_src) << " to "
          << host_dst << " on stream " << stream;
  return absl::OkStatus();
}

absl::Status AsynchronousMemcpyH2D(Context* context, CUdeviceptr gpu_dst,
                                   const void* host_src, uint64_t size,
                                   CUstream stream) {
  ScopedActivateContext activation(context);
  TF_RETURN_IF_ERROR(
      cuda::ToStatus(cuMemcpyHtoDAsync(gpu_dst, host_src, size, stream)));

  VLOG(2) << "successfully enqueued async memcpy h2d of " << size << " bytes"
          << " from " << host_src << " to " << absl::bit_cast<void*>(gpu_dst)
          << " on stream " << stream;
  return absl::OkStatus();
}

absl::Status AsynchronousMemcpyD2D(Context* context, CUdeviceptr gpu_dst,
                                   CUdeviceptr gpu_src, uint64_t size,
                                   CUstream stream) {
  ScopedActivateContext activation(context);

  // In graph capture mode we never have operations that access peer memory, so
  // we can always make a call to cuMemcpyDtoDAsync.
  TF_ASSIGN_OR_RETURN(bool is_capturing, StreamIsCapturing(stream));

  if ((gpu_dst == 0 || gpu_src == 0) || is_capturing) {
    // GetContextMap()->GetAnyContext() doesn't work when ptr == 0.
    // This happens when the size is 0.
    TF_RETURN_IF_ERROR(
        cuda::ToStatus(cuMemcpyDtoDAsync(gpu_dst, gpu_src, size, stream)));
  } else {
    // Any context work here.
    CUcontext dst_context = CudaContext::GetContextMap()->GetAnyContext(
        absl::bit_cast<void*>(gpu_dst));
    CUcontext src_context = CudaContext::GetContextMap()->GetAnyContext(
        absl::bit_cast<void*>(gpu_src));

    if (dst_context == src_context) {
      // Since the CUDA context is the same, the src and dst are within the same
      // GPU. So we can use cuMemcpyDtoD.
      TF_RETURN_IF_ERROR(
          cuda::ToStatus(cuMemcpyDtoDAsync(gpu_dst, gpu_src, size, stream)));
    } else {
      TF_RETURN_IF_ERROR(cuda::ToStatus(cuMemcpyPeerAsync(
          gpu_dst, dst_context, gpu_src, src_context, size, stream)));
    }
  }

  VLOG(2) << "successfully enqueued async memcpy d2d of " << size << " bytes"
          << " from " << absl::bit_cast<void*>(gpu_src) << " to "
          << absl::bit_cast<void*>(gpu_dst) << " on stream " << stream;
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<std::unique_ptr<CudaStream>> CudaStream::Create(
    GpuExecutor* executor,
    std::optional<std::variant<StreamPriority, int>> priority) {
  int stream_priority = [&]() {
    if (priority.has_value() && std::holds_alternative<int>(priority.value())) {
      return std::get<int>(priority.value());
    }
    return GetGpuStreamPriority(
        executor->gpu_context(),
        std::get<StreamPriority>(priority.value_or(StreamPriority::Default)));
  }();
  TF_ASSIGN_OR_RETURN(auto stream_handle,
                      CreateStream(executor->gpu_context(), stream_priority));

  TF_ASSIGN_OR_RETURN(auto completed_event,
                      CudaEvent::Create(executor->gpu_context(),
                                        /*allow_timing=*/false));

  return std::unique_ptr<CudaStream>(new CudaStream(
      executor, std::move(completed_event), priority, stream_handle));
}

absl::Status CudaStream::WaitFor(Stream* other) {
  CudaStream* other_stream = static_cast<CudaStream*>(other);

  TF_RETURN_IF_ERROR(other_stream->RecordCompletedEvent());
  return WaitStreamOnEvent(executor_->gpu_context(), gpu_stream(),
                           other_stream->completed_event_.GetHandle());
}

absl::Status CudaStream::RecordEvent(Event* event) {
  return stream_executor::gpu::RecordEvent(
      executor_->gpu_context(), static_cast<CudaEvent*>(event)->GetHandle(),
      gpu_stream());
}

absl::Status CudaStream::WaitFor(Event* event) {
  return WaitStreamOnEvent(executor_->gpu_context(), gpu_stream(),
                           static_cast<CudaEvent*>(event)->GetHandle());
}

absl::Status CudaStream::RecordCompletedEvent() {
  return RecordEvent(&completed_event_);
}

CudaStream::~CudaStream() {
  BlockHostUntilDone().IgnoreError();
  executor_->DeallocateStream(this);
}

absl::Status CudaStream::Memset32(DeviceMemoryBase* location, uint32_t pattern,
                                  uint64_t size) {
  if (absl::bit_cast<uintptr_t>(location->opaque()) % alignof(uint32_t) != 0) {
    return absl::InvalidArgumentError("location must be 4 byte aligned.");
  }
  if (size % sizeof(uint32_t) != 0) {
    return absl::InvalidArgumentError("size must be a multiple of 4 bytes.");
  }
  ScopedActivateContext activation(executor_->gpu_context());
  return cuda::ToStatus(
      cuMemsetD32Async(absl::bit_cast<CUdeviceptr>(location->opaque()), pattern,
                       size / 4, gpu_stream()),
      "Failed to enqueue async memset operation");
}

absl::Status CudaStream::MemZero(DeviceMemoryBase* location, uint64_t size) {
  if (reinterpret_cast<uintptr_t>(location->opaque()) % alignof(uint32_t) ==
          0 &&
      size % sizeof(uint32_t) == 0) {
    return Memset32(location, 0x0, size);
  } else {
    ScopedActivateContext activation(executor_->gpu_context());
    return cuda::ToStatus(
        cuMemsetD8Async(absl::bit_cast<CUdeviceptr>(location->opaque()), 0x0,
                        size, gpu_stream()),
        "Failed to enqueue async memset operation");
  }
}

absl::Status CudaStream::Memcpy(DeviceMemoryBase* gpu_dst,
                                const DeviceMemoryBase& gpu_src,
                                uint64_t size) {
  return AsynchronousMemcpyD2D(
      executor_->gpu_context(), absl::bit_cast<CUdeviceptr>(gpu_dst->opaque()),
      absl::bit_cast<CUdeviceptr>(gpu_src.opaque()), size, gpu_stream());
}

absl::Status CudaStream::Memcpy(DeviceMemoryBase* gpu_dst, const void* host_src,
                                uint64_t size) {
  return AsynchronousMemcpyH2D(executor_->gpu_context(),
                               absl::bit_cast<CUdeviceptr>(gpu_dst->opaque()),
                               host_src, size, gpu_stream());
}

absl::Status CudaStream::Memcpy(void* host_dst, const DeviceMemoryBase& gpu_src,
                                uint64_t size) {
  return AsynchronousMemcpyD2H(executor_->gpu_context(), host_dst,
                               absl::bit_cast<CUdeviceptr>(gpu_src.opaque()),
                               size, gpu_stream());
}
}  // namespace gpu
}  // namespace stream_executor
