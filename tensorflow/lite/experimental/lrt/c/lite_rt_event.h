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

#ifndef TENSORFLOW_LITE_EXPERIMENTAL_LRT_C_LITE_RT_EVENT_H_
#define TENSORFLOW_LITE_EXPERIMENTAL_LRT_C_LITE_RT_EVENT_H_

#include <stdint.h>

#include "tensorflow/lite/experimental/lrt/c/lite_rt_common.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

LITE_RT_DEFINE_HANDLE(LrtEvent);

#if LRT_HAS_SYNC_FENCE_SUPPORT
LrtStatus LrtEventCreateFromSyncFenceFd(int sync_fence_fd, bool owns_fd,
                                        LrtEvent* event);

LrtStatus LrtEventGetSyncFenceFd(LrtEvent event, int* sync_fence_fd);
#endif  // LRT_HAS_SYNC_FENCE_SUPPORT

// Pass -1 for timeout_in_ms for indefinite wait.
LrtStatus LrtEventWait(LrtEvent event, int64_t timeout_in_ms);

LrtStatus LrtEventDestroy(LrtEvent event);

#ifdef __cplusplus
}
#endif  // __cplusplus

#endif  // TENSORFLOW_LITE_EXPERIMENTAL_LRT_C_LITE_RT_EVENT_H_
