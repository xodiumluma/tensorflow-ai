/* Copyright 2015 The OpenXLA Authors.

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

#include <stdint.h>
#include <stdlib.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/base/casts.h"
#include "absl/container/inlined_vector.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "third_party/gpus/cuda/include/cuda.h"
#include "third_party/gpus/cuda/include/cuda_runtime_api.h"
#include "third_party/gpus/cuda/include/driver_types.h"
#include "xla/stream_executor/cuda/cuda_context.h"
#include "xla/stream_executor/cuda/cuda_status.h"
#include "xla/stream_executor/gpu/context.h"
#include "xla/stream_executor/gpu/context_map.h"
#include "xla/stream_executor/gpu/gpu_diagnostics.h"
#include "xla/stream_executor/gpu/gpu_driver.h"
#include "xla/stream_executor/gpu/gpu_types.h"
#include "xla/stream_executor/gpu/scoped_activate_context.h"
#include "xla/stream_executor/platform.h"
#include "xla/stream_executor/stream_executor.h"
#include "tsl/platform/casts.h"
#include "tsl/platform/env.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/logging.h"
#include "tsl/platform/numbers.h"
#include "tsl/platform/status.h"
#include "tsl/platform/statusor.h"

namespace stream_executor {
namespace gpu {

absl::Status GpuDriver::CreateGraph(CUgraph* graph) {
  VLOG(2) << "Create new CUDA graph";
  TF_RETURN_IF_ERROR(cuda::ToStatus(cuGraphCreate(graph, /*flags=*/0),
                                    "Failed to create CUDA graph"));
  VLOG(2) << "Created CUDA graph " << *graph;
  return absl::OkStatus();
}

absl::Status GpuDriver::DestroyGraph(CUgraph graph) {
  VLOG(2) << "Destroy CUDA graph " << graph;
  return cuda::ToStatus(cuGraphDestroy(graph), "Failed to destroy CUDA graph");
}

static std::string_view StreamCaptureModeToString(
    GpuDriver::StreamCaptureMode mode) {
  switch (mode) {
    case GpuDriver::StreamCaptureMode::kGlobal:
      return "global";
    case GpuDriver::StreamCaptureMode::kThreadLocal:
      return "threadlocal";
    case GpuDriver::StreamCaptureMode::kRelaxed:
      return "relaxed";
  }
}

absl::Status GpuDriver::StreamBeginCapture(CUstream stream,
                                           StreamCaptureMode mode) {
  CUstreamCaptureMode cu_mode;
  switch (mode) {
    case StreamCaptureMode::kGlobal:
      cu_mode = CU_STREAM_CAPTURE_MODE_GLOBAL;
      break;
    case StreamCaptureMode::kThreadLocal:
      cu_mode = CU_STREAM_CAPTURE_MODE_THREAD_LOCAL;
      break;
    case StreamCaptureMode::kRelaxed:
      cu_mode = CU_STREAM_CAPTURE_MODE_RELAXED;
      break;
  }

  VLOG(2) << "Beginning stream " << stream << " capture in "
          << StreamCaptureModeToString(mode) << " mode";
  return cuda::ToStatus(cuStreamBeginCapture(stream, cu_mode),
                        "Failed to begin stream capture");
}

absl::Status GpuDriver::StreamBeginCaptureToGraph(CUstream stream,
                                                  CUgraph graph,
                                                  StreamCaptureMode mode) {
  CUstreamCaptureMode cu_mode;
  switch (mode) {
    case StreamCaptureMode::kGlobal:
      cu_mode = CU_STREAM_CAPTURE_MODE_GLOBAL;
      break;
    case StreamCaptureMode::kThreadLocal:
      cu_mode = CU_STREAM_CAPTURE_MODE_THREAD_LOCAL;
      break;
    case StreamCaptureMode::kRelaxed:
      cu_mode = CU_STREAM_CAPTURE_MODE_RELAXED;
      break;
  }

#if CUDA_VERSION >= 12030
  VLOG(2) << "Beginning stream " << stream << " capture in "
          << StreamCaptureModeToString(mode) << " mode to graph " << graph;
  return cuda::ToStatus(
      cuStreamBeginCaptureToGraph(stream, graph,
                                  /*dependencies=*/nullptr,
                                  /*dependencyData=*/nullptr,
                                  /*numDependencies=*/0, cu_mode),
      "Failed to begin stream capture to graph");
#else
  return absl::UnimplementedError(
      "StreamBeginCaptureToGraph is not implemented");
#endif  // CUDA_VERSION >= 12030
}

absl::Status GpuDriver::StreamEndCapture(CUstream stream, CUgraph* graph) {
  VLOG(2) << "End stream " << stream << " capture";

  return cuda::ToStatus(cuStreamEndCapture(stream, graph),
                        "Failed to end stream capture");
}

absl::Status GpuDriver::GraphInstantiate(CUgraphExec* exec, CUgraph graph,
                                         const GraphInstantiateFlags& flags) {
  VLOG(2) << "Instantiate CUDA executable graph from graph " << graph << " ("
          << "auto_free_on_launch=" << flags.auto_free_on_launch << ", "
          << "device_launch=" << flags.device_launch << ", "
          << "use_node_priority=" << flags.use_node_prirotiy << ", "
          << "upload=" << flags.upload << ")";

#if CUDA_VERSION >= 12000
  uint64_t cu_flags = 0;
  if (flags.auto_free_on_launch)
    cu_flags |= CUDA_GRAPH_INSTANTIATE_FLAG_AUTO_FREE_ON_LAUNCH;
  if (flags.use_node_prirotiy)
    cu_flags |= CUDA_GRAPH_INSTANTIATE_FLAG_USE_NODE_PRIORITY;
  if (flags.device_launch)
    cu_flags |= CUDA_GRAPH_INSTANTIATE_FLAG_DEVICE_LAUNCH;
  if (flags.upload) cu_flags |= CUDA_GRAPH_INSTANTIATE_FLAG_UPLOAD;

  return cuda::ToStatus(cuGraphInstantiate(exec, graph, cu_flags),
                        "Failed to instantiate CUDA graph");
#else
  return cuda::ToStatus(cuGraphInstantiate(exec, graph, nullptr, nullptr, 0),
                        "Failed to instantiate CUDA graph");
#endif  // CUDA_VERSION >= 12000
}

absl::Status GpuDriver::GraphLaunch(CUgraphExec exec, CUstream stream) {
  VLOG(2) << "Launching CUDA executable graph " << exec << " on a stream "
          << stream;
  return cuda::ToStatus(cuGraphLaunch(exec, stream),
                        "Failed to launch CUDA graph");
}

absl::Status GpuDriver::GraphNodeSetEnabled(CUgraphExec exec, CUgraphNode node,
                                            bool enabled) {
  // Node is enabled if value != 0, otherwise the node is disabled.
  unsigned value = enabled ? 1 : 0;
  VLOG(2) << "Set CUDA executable graph " << exec << " node " << node
          << " enabled flag to " << value;
  return cuda::ToStatus(cuGraphNodeSetEnabled(exec, node, value),
                        "Failed to set CUDA graph node enabled flag");
}

absl::Status GpuDriver::GraphExecUpdate(CUgraphExec exec, CUgraph graph,
                                        GraphExecUpdateResultInfo* result) {
  VLOG(2) << "Update CUDA graph executable " << exec << " with graph " << graph;

#if CUDA_VERSION >= 12000
  CUgraphExecUpdateResultInfo cu_result;
  memset(&cu_result, 0, sizeof(cu_result));
  CUresult err_code = cuGraphExecUpdate(exec, graph, &cu_result);
  auto cu_result_enum = cu_result.result;
  if (cu_result.errorFromNode) {
    result->error_from_node = cu_result.errorFromNode;
  }
  if (cu_result.errorNode) {
    result->error_node = cu_result.errorNode;
  }
#else
  CUgraphExecUpdateResult cu_result;
  CUresult err_code = cuGraphExecUpdate(exec, graph, nullptr, &cu_result);
  auto cu_result_enum = cu_result;
#endif  // CUDA_VERSION >= 12000

  switch (cu_result_enum) {
    case CU_GRAPH_EXEC_UPDATE_SUCCESS:
      result->result = GraphExecUpdateResult::kSuccess;
      break;
    case CU_GRAPH_EXEC_UPDATE_ERROR:
      result->result = GraphExecUpdateResult::kError;
      break;
    case CU_GRAPH_EXEC_UPDATE_ERROR_TOPOLOGY_CHANGED:
      result->result = GraphExecUpdateResult::kTopologyChanged;
      break;
    case CU_GRAPH_EXEC_UPDATE_ERROR_NODE_TYPE_CHANGED:
      result->result = GraphExecUpdateResult::kNodeTypeChanged;
      break;
    case CU_GRAPH_EXEC_UPDATE_ERROR_FUNCTION_CHANGED:
      result->result = GraphExecUpdateResult::kFunctionChanged;
      break;
    case CU_GRAPH_EXEC_UPDATE_ERROR_PARAMETERS_CHANGED:
      result->result = GraphExecUpdateResult::kParametersChanged;
      break;
    case CU_GRAPH_EXEC_UPDATE_ERROR_NOT_SUPPORTED:
      result->result = GraphExecUpdateResult::kNotSupported;
      break;
#if CUDA_VERSION >= 12000
    case CU_GRAPH_EXEC_UPDATE_ERROR_UNSUPPORTED_FUNCTION_CHANGE:
      result->result = GraphExecUpdateResult::kUnsupportedFunctionChange;
      break;
    case CU_GRAPH_EXEC_UPDATE_ERROR_ATTRIBUTES_CHANGED:
      result->result = GraphExecUpdateResult::kAttributesChanged;
      break;
#endif  // CUDA_VERSION >= 12000
    default:
      return absl::InternalError("Unknown graph update result");
  }
  return cuda::ToStatus(err_code, "Failed to update CUDA graph");
}

absl::StatusOr<std::vector<GpuGraphNodeHandle>>
GpuDriver::GraphNodeGetDependencies(GpuGraphNodeHandle node) {
  VLOG(2) << "Get CUDA graph node " << node << " dependencies";

  std::vector<CUgraphNode> dependencies;

  size_t num_dependencies = 0;
  TF_RETURN_IF_ERROR(cuda::ToStatus(
      cuGraphNodeGetDependencies(node, nullptr, &num_dependencies),
      "Failed to get CUDA graph node depedencies size"));

  dependencies.resize(num_dependencies, nullptr);
  TF_RETURN_IF_ERROR(cuda::ToStatus(
      cuGraphNodeGetDependencies(node, dependencies.data(), &num_dependencies),
      "Failed to get CUDA graph node depedencies"));

  return dependencies;
}

absl::Status GpuDriver::DestroyGraphExec(CUgraphExec exec) {
  VLOG(2) << "Destroying CUDA executable graph " << exec;
  return cuda::ToStatus(cuGraphExecDestroy(exec),
                        "Failed to destroy CUDA executable graph");
}

absl::StatusOr<std::string> GpuDriver::GraphDebugDotPrint(
    CUgraph graph, const char* path, bool return_printed_graph) {
#if CUDA_VERSION >= 12000
  VLOG(2) << "Print CUDA graph " << graph << " debug dot file to " << path;

  int flags = CU_GRAPH_DEBUG_DOT_FLAGS_VERBOSE;
  TF_RETURN_IF_ERROR(cuda::ToStatus(cuGraphDebugDotPrint(graph, path, flags),
                                    "Failed to print gpu graph debug file"));

  if (return_printed_graph) {
    std::string data;
    if (tsl::ReadFileToString(tsl::Env::Default(), path, &data).ok()) {
      return data;
    } else {
      LOG(WARNING) << "failed to read gpu graph debug file " << path;
    }
  }
#endif  // CUDA_VERSION >= 12000

  return std::string(path);
}

absl::Status GpuDriver::GraphConditionalHandleCreate(
    GpuGraphConditionalHandle* handle, CUgraph graph, Context* context,
    unsigned int default_launch_value, unsigned int flags) {
  VLOG(2) << "Create conditional handle for a graph " << graph
          << "; context: " << context
          << "; default_launch_value: " << default_launch_value
          << "; flags: " << flags;

#if CUDA_VERSION >= 12030
  return cuda::ToStatus(
      cuGraphConditionalHandleCreate(
          handle, graph,
          tensorflow::down_cast<CudaContext*>(context)->context(),
          default_launch_value, flags),
      "Failed to create conditional handle for a CUDA graph");
#else
  return absl::UnimplementedError(
      "CUDA graph conditional nodes are not implemented");
#endif  // CUDA_VERSION >= 12030
}

static std::string ConditionalTypeToString(
    GpuDriver::GpuGraphConditionalNodeParams::Type type) {
  switch (type) {
    case GpuDriver::GpuGraphConditionalNodeParams::Type::kIf:
      return "IF";
    case GpuDriver::GpuGraphConditionalNodeParams::Type::kWhile:
      return "WHILE";
  }
}

absl::StatusOr<GpuDriver::GpuGraphNodeResult> GpuDriver::GraphAddNode(
    CUgraphNode* node, CUgraph graph, absl::Span<const CUgraphNode> deps,
    const GpuGraphNodeParams& params) {
#if CUDA_VERSION >= 12030
  // Add conditional node to a graph.
  if (auto* conditional = std::get_if<GpuGraphConditionalNodeParams>(&params)) {
    VLOG(2) << "Add conditional node to a graph " << graph
            << "; type: " << ConditionalTypeToString(conditional->type)
            << "; deps: " << deps.size();

    CUgraphNodeParams cu_params;
    memset(&cu_params, 0, sizeof(cu_params));
    CudaContext* gpu_context =
        tensorflow::down_cast<CudaContext*>(conditional->context);

    cu_params.type = CU_GRAPH_NODE_TYPE_CONDITIONAL;
    cu_params.conditional.handle = conditional->handle;
    cu_params.conditional.ctx = gpu_context->context();
    cu_params.conditional.size = 1;

    switch (conditional->type) {
      case GpuDriver::GpuGraphConditionalNodeParams::Type::kIf:
        cu_params.conditional.type = CU_GRAPH_COND_TYPE_IF;
        break;
      case GpuDriver::GpuGraphConditionalNodeParams::Type::kWhile:
        cu_params.conditional.type = CU_GRAPH_COND_TYPE_WHILE;
        break;
    }

    TF_RETURN_IF_ERROR(cuda::ToStatus(
        cuGraphAddNode(node, graph, deps.data(), deps.size(), &cu_params),
        "Failed to add conditional node to a CUDA graph"));

    GpuGraphConditionalNodeParams::Result result;
    result.graph = cu_params.conditional.phGraph_out[0];

    VLOG(2) << "Created conditional CUDA graph " << result.graph;
    return result;
  }
#endif  // CUDA_VERSION >= 12030

  return absl::UnimplementedError("unsupported node type");
}

absl::Status GpuDriver::GraphAddEmptyNode(CUgraphNode* node, CUgraph graph,
                                          absl::Span<const CUgraphNode> deps) {
  VLOG(2) << "Add empty node to a graph " << graph << "; deps: " << deps.size();

  return cuda::ToStatus(
      cuGraphAddEmptyNode(node, graph, deps.data(), deps.size()),
      "Failed to add empty node to a CUDA graph");
}

absl::Status GpuDriver::GraphAddKernelNode(
    CUgraphNode* node, CUgraph graph, absl::Span<const CUgraphNode> deps,
    absl::string_view kernel_name, CUfunction function, unsigned int grid_dim_x,
    unsigned int grid_dim_y, unsigned int grid_dim_z, unsigned int block_dim_x,
    unsigned int block_dim_y, unsigned int block_dim_z,
    unsigned int shared_mem_bytes, void** kernel_params, void** extra) {
  VLOG(2) << "Add kernel node to a graph " << graph
          << "; kernel: " << kernel_name << "; gdx: " << grid_dim_x
          << " gdy: " << grid_dim_y << " gdz: " << grid_dim_z
          << " bdx: " << block_dim_x << " bdy: " << block_dim_y
          << " bdz: " << block_dim_z << "; shmem: " << shared_mem_bytes
          << "; deps: " << deps.size();

  CUDA_KERNEL_NODE_PARAMS params;
  memset(&params, 0, sizeof(params));

  params.func = function;
  params.gridDimX = grid_dim_x;
  params.gridDimY = grid_dim_y;
  params.gridDimZ = grid_dim_z;
  params.blockDimX = block_dim_x;
  params.blockDimY = block_dim_y;
  params.blockDimZ = block_dim_z;
  params.sharedMemBytes = shared_mem_bytes;
  params.kernelParams = kernel_params;
  params.extra = extra;

  // TODO(ezhulenev): Why do we do it on every call to launch kernel? This
  // should be moved one level up to se::Kernel level, and done just once (or
  // updated once we get a new larger shared memory request).
  if (shared_mem_bytes != 0) {
    TF_RETURN_IF_ERROR(cuda::ToStatus(
        cuFuncSetAttribute(function,
                           CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES,
                           shared_mem_bytes),
        "Failed to set shared memory size"));
  }

  return cuda::ToStatus(
      cuGraphAddKernelNode(node, graph, deps.data(), deps.size(), &params),
      "Failed to add kernel node to a CUDA graph");
}

/*static*/ absl::Status GpuDriver::GraphExecKernelNodeSetParams(
    CUgraphExec exec, CUgraphNode node, absl::string_view kernel_name,
    CUfunction function, unsigned int grid_dim_x, unsigned int grid_dim_y,
    unsigned int grid_dim_z, unsigned int block_dim_x, unsigned int block_dim_y,
    unsigned int block_dim_z, unsigned int shared_mem_bytes,
    void** kernel_params, void** extra) {
  VLOG(2) << "Set kernel node params " << node << " in graph executable "
          << exec << "; kernel: " << kernel_name << "; gdx: " << grid_dim_x
          << " gdy: " << grid_dim_y << " gdz: " << grid_dim_z
          << " bdx: " << block_dim_x << " bdy: " << block_dim_y
          << " bdz: " << block_dim_z << "; shmem: " << shared_mem_bytes;

  CUDA_KERNEL_NODE_PARAMS params;
  memset(&params, 0, sizeof(params));

  params.func = function;
  params.gridDimX = grid_dim_x;
  params.gridDimY = grid_dim_y;
  params.gridDimZ = grid_dim_z;
  params.blockDimX = block_dim_x;
  params.blockDimY = block_dim_y;
  params.blockDimZ = block_dim_z;
  params.sharedMemBytes = shared_mem_bytes;
  params.kernelParams = kernel_params;
  params.extra = extra;

  // TODO(ezhulenev): Why do we do it on every call to launch kernel? This
  // should be moved one level up to se::Kernel level, and done just once (or
  // updated once we get a new larger shared memory request).
  if (shared_mem_bytes != 0) {
    TF_RETURN_IF_ERROR(cuda::ToStatus(
        cuFuncSetAttribute(function,
                           CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES,
                           shared_mem_bytes),
        "Failed to set shared memory size"));
  }

  return cuda::ToStatus(cuGraphExecKernelNodeSetParams(exec, node, &params),
                        "Failed to set CUDA graph kernel node params");
}

absl::Status GpuDriver::GraphAddMemcpyD2DNode(
    Context* context, CUgraphNode* node, CUgraph graph,
    absl::Span<const CUgraphNode> deps, CUdeviceptr gpu_dst,
    CUdeviceptr gpu_src, uint64_t size) {
  CudaContext* gpu_context = tensorflow::down_cast<CudaContext*>(context);
  VLOG(2) << "Add memcpy d2d node to a graph " << graph
          << "; dst: " << reinterpret_cast<void*>(gpu_dst)
          << "; src: " << reinterpret_cast<void*>(gpu_src) << "; size: " << size
          << "; context: " << gpu_context->context()
          << "; deps: " << deps.size();

  CUDA_MEMCPY3D params;
  memset(&params, 0, sizeof(params));

  params.srcMemoryType = CU_MEMORYTYPE_DEVICE;
  params.srcDevice = gpu_src;
  params.dstMemoryType = CU_MEMORYTYPE_DEVICE;
  params.dstDevice = gpu_dst;
  params.WidthInBytes = size;
  params.Height = 1;
  params.Depth = 1;

  return cuda::ToStatus(
      cuGraphAddMemcpyNode(node, graph, deps.data(), deps.size(), &params,
                           gpu_context->context()),
      "Failed to add memcpy d2d node to a CUDA graph");
}

absl::Status GpuDriver::GraphExecMemcpyD2DNodeSetParams(
    Context* context, GpuGraphExecHandle exec, GpuGraphNodeHandle node,
    CUdeviceptr gpu_dst, CUdeviceptr gpu_src, uint64_t size) {
  CudaContext* gpu_context = tensorflow::down_cast<CudaContext*>(context);
  VLOG(2) << "Set memcpy d2d node params " << node << " in graph executable "
          << exec << "; dst: " << reinterpret_cast<void*>(gpu_dst)
          << "; src: " << reinterpret_cast<void*>(gpu_src) << "; size: " << size
          << "; context: " << gpu_context->context();

  CUDA_MEMCPY3D params;
  memset(&params, 0, sizeof(params));

  params.srcMemoryType = CU_MEMORYTYPE_DEVICE;
  params.srcDevice = gpu_src;
  params.dstMemoryType = CU_MEMORYTYPE_DEVICE;
  params.dstDevice = gpu_dst;
  params.WidthInBytes = size;
  params.Height = 1;
  params.Depth = 1;

  return cuda::ToStatus(cuGraphExecMemcpyNodeSetParams(exec, node, &params,
                                                       gpu_context->context()),
                        "Failed to set memcpy d2d node params");
}

namespace {

struct BitPatternToString {
  std::string operator()(uint8_t pattern) {
    return absl::StrCat("u8:", pattern);
  }
  std::string operator()(uint16_t pattern) {
    return absl::StrCat("u16:", pattern);
  }
  std::string operator()(uint32_t pattern) {
    return absl::StrCat("u32:", pattern);
  }
};

// Broadcasts a pattern value of 1/2/4 bytes to a 4 byte value.
struct BitPatternToValue {
  std::pair<unsigned, unsigned> operator()(uint8_t pattern) {
    unsigned value = pattern;
    return {(value << 24) | (value << 16) | (value << 8) | value,
            /*element_size=*/1};
  }
  std::pair<unsigned, unsigned> operator()(uint16_t pattern) {
    unsigned value = pattern;
    return {(value << 16) | value, /*element_size=*/2};
  }
  std::pair<unsigned, unsigned> operator()(uint32_t pattern) {
    return {pattern, /*element_size=*/4};
  }
};

}  // namespace

absl::Status GpuDriver::GraphAddMemsetNode(
    Context* context, CUgraphNode* node, GpuGraphHandle graph,
    absl::Span<const CUgraphNode> deps, CUdeviceptr dst,
    std::variant<uint8_t, uint16_t, uint32_t> bit_pattern,
    uint64_t num_elements) {
  CudaContext* gpu_context = tensorflow::down_cast<CudaContext*>(context);
  VLOG(2) << "Add memset node to a graph " << graph
          << "; dst: " << reinterpret_cast<void*>(dst)
          << "; bit_pattern: " << std::visit(BitPatternToString(), bit_pattern)
          << "; num_elements: " << num_elements
          << "; context: " << gpu_context->context()
          << "; deps: " << deps.size();

  CUDA_MEMSET_NODE_PARAMS params;
  memset(&params, 0, sizeof(params));

  auto [value, element_size] = std::visit(BitPatternToValue(), bit_pattern);

  params.dst = dst;
  params.elementSize = element_size;
  params.height = 1;
  params.pitch = 0;  // unused if height is 1
  params.value = value;
  params.width = num_elements;

  return cuda::ToStatus(
      cuGraphAddMemsetNode(node, graph, deps.data(), deps.size(), &params,
                           gpu_context->context()),
      "Failed to add memset node to a CUDA graph");
}

absl::Status GpuDriver::GraphExecMemsetNodeSetParams(
    Context* context, CUgraphExec exec, CUgraphNode node, CUdeviceptr dst,
    std::variant<uint8_t, uint16_t, uint32_t> bit_pattern,
    uint64_t num_elements) {
  CudaContext* gpu_context = tensorflow::down_cast<CudaContext*>(context);
  VLOG(2) << "Set memset node params " << node << " in graph executable "
          << exec << "; dst: " << reinterpret_cast<void*>(dst)
          << "; bit_pattern: " << std::visit(BitPatternToString(), bit_pattern)
          << "; num_elements: " << num_elements
          << "; context: " << gpu_context->context();

  CUDA_MEMSET_NODE_PARAMS params;
  memset(&params, 0, sizeof(params));

  auto [value, element_size] = std::visit(BitPatternToValue(), bit_pattern);

  params.dst = dst;
  params.elementSize = element_size;
  params.height = 1;
  params.pitch = 0;  // unused if height is 1
  params.value = value;
  params.width = num_elements;

  return cuda::ToStatus(cuGraphExecMemsetNodeSetParams(exec, node, &params,
                                                       gpu_context->context()),
                        "Failed to set memset node params");
}

absl::Status GpuDriver::GraphAddChildNode(CUgraphNode* node, CUgraph graph,
                                          absl::Span<const CUgraphNode> deps,
                                          CUgraph child) {
  VLOG(2) << "Create a new node by cloning the child graph " << child
          << " and add it to " << graph << "; deps: " << deps.size();

  return cuda::ToStatus(
      cuGraphAddChildGraphNode(node, graph, deps.data(), deps.size(), child),
      "Failed to create a child graph node and add it to a CUDA graph");
}

/*static*/ absl::Status GpuDriver::GraphExecChildNodeSetParams(CUgraphExec exec,
                                                               CUgraphNode node,
                                                               CUgraph child) {
  VLOG(2) << "Set child node params " << node << " in graph executable " << exec
          << "to params contained in " << child;

  return cuda::ToStatus(cuGraphExecChildGraphNodeSetParams(exec, node, child),
                        "Failed to set CUDA graph child node params");
}

absl::Status GpuDriver::LaunchKernel(
    Context* context, absl::string_view kernel_name, CUfunction function,
    unsigned int grid_dim_x, unsigned int grid_dim_y, unsigned int grid_dim_z,
    unsigned int block_dim_x, unsigned int block_dim_y,
    unsigned int block_dim_z, unsigned int shared_mem_bytes, CUstream stream,
    void** kernel_params, void** extra) {
  ScopedActivateContext activation(context);
  VLOG(2) << "launching kernel: " << kernel_name << "; gdx: " << grid_dim_x
          << " gdy: " << grid_dim_y << " gdz: " << grid_dim_z
          << " bdx: " << block_dim_x << " bdy: " << block_dim_y
          << " bdz: " << block_dim_z
          << "; shared_mem_bytes: " << shared_mem_bytes;

  // TODO(ezhulenev): Why do we do it on every call to launch kernel? This
  // should be moved one level up to se::Kernel level, and done just once (or
  // updated once we get a new larger shared memory request).
  if (shared_mem_bytes != 0) {
    TF_RETURN_IF_ERROR(cuda::ToStatus(
        cuFuncSetAttribute(function,
                           CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES,
                           shared_mem_bytes),
        "Failed to set shared memory size"));
  }

  return cuda::ToStatus(
      cuLaunchKernel(function, grid_dim_x, grid_dim_y, grid_dim_z, block_dim_x,
                     block_dim_y, block_dim_z, shared_mem_bytes, stream,
                     kernel_params, extra),
      absl::StrCat("Failed to launch CUDA kernel: ", kernel_name,
                   "; block dims: ", block_dim_x, "x", block_dim_y, "x",
                   block_dim_z, "; grid dims: ", grid_dim_x, "x", grid_dim_y,
                   "x", grid_dim_z,
                   "; shared memory size: ", shared_mem_bytes));
}

absl::Status GpuDriver::LaunchKernel(
    Context* context, absl::string_view kernel_name, CUfunction function,
    unsigned int cluster_dim_x, unsigned int cluster_dim_y,
    unsigned int cluster_dim_z, unsigned int grid_dim_x,
    unsigned int grid_dim_y, unsigned int grid_dim_z, unsigned int block_dim_x,
    unsigned int block_dim_y, unsigned int block_dim_z,
    unsigned int shared_mem_bytes, GpuStreamHandle stream, void** kernel_params,
    void** extra) {
  ScopedActivateContext activation(context);
  VLOG(2) << "launching kernel: " << kernel_name << "; cdx: " << cluster_dim_x
          << " cdy: " << cluster_dim_y << " cdz: " << cluster_dim_z
          << " gdx: " << grid_dim_x << " gdy: " << grid_dim_y
          << " gdz: " << grid_dim_z << " bdx: " << block_dim_x
          << " bdy: " << block_dim_y << " bdz: " << block_dim_z
          << "; shared_mem_bytes: " << shared_mem_bytes;

  // TODO(ezhulenev): Why do we do it on every call to launch kernel? This
  // should be moved one level up to se::Kernel level, and done just once (or
  // updated once we get a new larger shared memory request).
  if (shared_mem_bytes != 0) {
    TF_RETURN_IF_ERROR(cuda::ToStatus(
        cuFuncSetAttribute(function,
                           CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES,
                           shared_mem_bytes),
        "Failed to set shared memory size"));
  }

  CUlaunchConfig launch_config;
  memset(&launch_config, 0, sizeof(launch_config));
  launch_config.blockDimX = block_dim_x;
  launch_config.blockDimY = block_dim_y;
  launch_config.blockDimZ = block_dim_z;
  launch_config.gridDimX = grid_dim_x;
  launch_config.gridDimY = grid_dim_y;
  launch_config.gridDimZ = grid_dim_z;
  launch_config.hStream = stream;
  launch_config.sharedMemBytes = shared_mem_bytes;

  CUlaunchAttribute cluster_dims;
  memset(&cluster_dims, 0, sizeof(cluster_dims));
  cluster_dims.id = CU_LAUNCH_ATTRIBUTE_CLUSTER_DIMENSION;
  cluster_dims.value.clusterDim.x = cluster_dim_x;
  cluster_dims.value.clusterDim.y = cluster_dim_y;
  cluster_dims.value.clusterDim.z = cluster_dim_z;

  launch_config.attrs = &cluster_dims;
  launch_config.numAttrs = 1;

  return cuda::ToStatus(
      cuLaunchKernelEx(&launch_config, function, kernel_params, extra),
      absl::StrCat("Failed to launch CUDA kernel: ", kernel_name,
                   "; cluster dims: ", cluster_dim_x, "x", cluster_dim_y, "x",
                   cluster_dim_z, "; block dims: ", block_dim_x, "x",
                   block_dim_y, "x", block_dim_z, "; grid dims: ", grid_dim_x,
                   "x", grid_dim_y, "x", grid_dim_z,
                   "; shared memory size: ", shared_mem_bytes));
}

absl::Status GpuDriver::AddStreamCallback(Context* context, CUstream stream,
                                          StreamCallback callback, void* data) {
  // Note: flags param is required to be zero according to CUDA 6.0.
  return cuda::ToStatus(cuLaunchHostFunc(stream, callback, data));
}


void GpuDriver::DestroyStream(Context* context, GpuStreamHandle stream) {
  if (stream == nullptr) {
    return;
  }

  ScopedActivateContext activated{context};
  CUresult res = cuStreamQuery(stream);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "stream not idle on destroy: " << cuda::ToStatus(res);
  }

  auto status = cuda::ToStatus(cuStreamDestroy(stream));
  if (!status.ok()) {
    LOG(ERROR) << "failed to destroy CUDA stream for context " << context
               << ": " << status;
  } else {
    VLOG(2) << "successfully destroyed stream " << stream << " for context "
            << context;
  }
}

absl::Status GpuDriver::SynchronizeStream(Context* context, CUstream stream) {
  ScopedActivateContext activated{context};
  CHECK(stream != nullptr);
  return cuda::ToStatus(cuStreamSynchronize(stream),
                        "Could not synchronize CUDA stream");
}

int GpuDriver::GetDeviceCount() {
  int device_count = 0;
  auto status = cuda::ToStatus(cuDeviceGetCount(&device_count));
  if (!status.ok()) {
    LOG(ERROR) << "could not retrieve CUDA device count: " << status;
    return 0;
  }

  return device_count;
}

absl::Status GpuDriver::GetPointerAddressRange(CUdeviceptr dptr,
                                               CUdeviceptr* base,
                                               size_t* size) {
  return cuda::ToStatus(cuMemGetAddressRange(base, size, dptr));
}

absl::StatusOr<int32_t> GpuDriver::GetDriverVersion() {
  int32_t version;
  TF_RETURN_IF_ERROR(cuda::ToStatus(cuDriverGetVersion(&version),
                                    "Could not get driver version"));
  return version;
}

absl::StatusOr<int> GpuDriver::GetMaxOccupiedBlocksPerCore(
    Context* context, CUfunction kernel, int threads_per_block,
    size_t dynamic_shared_memory_bytes) {
  ScopedActivateContext activation(context);

  int max_blocks;
  TF_RETURN_IF_ERROR(cuda::ToStatus(
      cuOccupancyMaxActiveBlocksPerMultiprocessorWithFlags(
          &max_blocks, kernel, threads_per_block, dynamic_shared_memory_bytes,
          CU_OCCUPANCY_DISABLE_CACHING_OVERRIDE),
      absl::StrFormat("Failed to calculate occupancy of kernel %p", kernel)));
  return max_blocks;
}

absl::StatusOr<size_t> GpuDriver::GraphGetNodeCount(GpuGraphHandle graph) {
  size_t num_nodes;
  TF_RETURN_IF_ERROR(
      cuda::ToStatus(cuGraphGetNodes(graph, /*nodes=*/nullptr, &num_nodes)));
  return num_nodes;
}

}  // namespace gpu
}  // namespace stream_executor
