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

#ifndef XLA_SERVICE_GPU_TRANSFORMS_COLLECTIVE_SEND_RECV_COMBINER_H_
#define XLA_SERVICE_GPU_TRANSFORMS_COLLECTIVE_SEND_RECV_COMBINER_H_

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_module.h"
#include "xla/hlo/pass/hlo_pass_interface.h"

namespace xla {

// CollectiveSendRecvCombiner is a pass that scans for all send/recv pairs
// which are part of the same computation, and transforms them into wrapped
// single-op computations that are executed asynchronously. This pass also
// replaces the corresponding send-done and recv-done instructions with
// async-done functions. This pass is primarily used for pipelining send/recv
// and send-done/recv-done instructions across while loop iteration boundaries.
class CollectiveSendRecvCombiner : public HloModulePass {
 public:
  absl::string_view name() const override {
    return "collective-send-recv-combiner";
  }

  using HloPassInterface::Run;
  absl::StatusOr<bool> Run(
      HloModule* module,
      const absl::flat_hash_set<absl::string_view>& execution_threads) override;
};
}  // namespace xla

#endif  // XLA_SERVICE_GPU_TRANSFORMS_COLLECTIVE_SEND_RECV_COMBINER_H_
