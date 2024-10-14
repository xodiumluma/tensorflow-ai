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

#ifndef TENSORFLOW_LITE_EXPERIMENTAL_LRT_VENDORS_PIXEL_DISPATCH_LRT_DISPATCH_GRAPH_H_
#define TENSORFLOW_LITE_EXPERIMENTAL_LRT_VENDORS_PIXEL_DISPATCH_LRT_DISPATCH_GRAPH_H_

#include <memory>
#include <set>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "third_party/odml/infra/southbound/sb_api.h"
#include "tensorflow/lite/experimental/lrt/c/lite_rt_dispatch.h"

class LrtDispatchGraphT {
 public:
  LrtDispatchGraphT(ThrGraph* thr_graph,
                    LrtDispatchDeviceContext device_context)
      : thr_graph_(thr_graph), device_context_(device_context) {}

  ThrGraph* thr_graph() { return thr_graph_; }

  LrtDispatchDeviceContext device_context() { return device_context_; }

  int NextNodeInputIndex(LrtDispatchNodeId node_id) {
    return NextNodeIoIndex(node_id, next_node_input_index_);
  }

  int NextNodeOutputIndex(LrtDispatchNodeId node_id) {
    return NextNodeIoIndex(node_id, next_node_output_index_);
  }

  int NextGraphInputIndex() { return next_graph_input_index_++; }

  int NextGraphOutputIndex() { return next_graph_output_index_++; }

  void AddInputEdge(int input_index, LrtDispatchEdgeId edge_id) {
    input_edges_[input_index] = edge_id;
  }

  void AddOutputEdge(int output_index, LrtDispatchEdgeId edge_id) {
    output_edges_[output_index] = edge_id;
  }

  absl::StatusOr<LrtDispatchEdgeId> InputEdge(int input_index) const {
    return IoEdge(input_index, input_edges_);
  }

  absl::StatusOr<LrtDispatchEdgeId> OutputEdge(int output_index) const {
    return IoEdge(output_index, output_edges_);
  }

  size_t NumOutputs() const { return output_edges_.size(); }

 private:
  using NextNodeIoIndexMap = std::map<LrtDispatchNodeId, int>;
  using IoIndexToEdgeIdMap = std::map<int, LrtDispatchEdgeId>;

  int NextNodeIoIndex(LrtDispatchNodeId node_id, NextNodeIoIndexMap& map) {
    return map[node_id]++;
  }

  absl::StatusOr<LrtDispatchEdgeId> IoEdge(
      int io_index, const IoIndexToEdgeIdMap& map) const {
    auto iter = map.find(io_index);
    if (iter == map.end()) {
      return absl::NotFoundError("Unexpected graph input/output index");
    }
    return iter->second;
  }

  ThrGraph* thr_graph_;
  LrtDispatchDeviceContext device_context_;
  NextNodeIoIndexMap next_node_input_index_;
  NextNodeIoIndexMap next_node_output_index_;
  int next_graph_input_index_ = 0;
  int next_graph_output_index_ = 0;
  IoIndexToEdgeIdMap input_edges_;
  IoIndexToEdgeIdMap output_edges_;
};

#endif  // TENSORFLOW_LITE_EXPERIMENTAL_LRT_VENDORS_PIXEL_DISPATCH_LRT_DISPATCH_GRAPH_H_
