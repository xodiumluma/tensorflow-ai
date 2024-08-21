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

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Complex/IR/Complex.h"
#include "mlir/Dialect/DLTI/DLTI.h"
#include "mlir/Dialect/Func/Extensions/AllExtensions.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/Transforms/InlinerInterfaceImpl.h"
#include "mlir/Dialect/LLVMIR/NVVMDialect.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"
#include "mlir/Transforms/Passes.h"
#include "xla/mlir_hlo/mhlo/IR/hlo_ops.h"
#include "xla/service/gpu/fusions/ir/xla_gpu_ops.h"
#include "xla/service/gpu/fusions/transforms/passes.h"

int main(int argc, char** argv) {
  mlir::DialectRegistry registry;
  registry.insert<mlir::DLTIDialect, mlir::tensor::TensorDialect,
                  mlir::func::FuncDialect, mlir::affine::AffineDialect,
                  mlir::arith::ArithDialect, mlir::complex::ComplexDialect,
                  mlir::math::MathDialect, mlir::scf::SCFDialect,
                  mlir::mhlo::MhloDialect, mlir::LLVM::LLVMDialect,
                  mlir::gpu::GPUDialect, mlir::mhlo::MhloDialect,
                  mlir::vector::VectorDialect, xla::gpu::XlaGpuDialect,
                  mlir::NVVM::NVVMDialect>();
  mlir::func::registerAllExtensions(registry);
  mlir::LLVM::registerInlinerInterface(registry);
  mlir::registerCanonicalizerPass();
  mlir::registerCSEPass();
  mlir::registerInliner();
  xla::gpu::registerGpuFusionTransformsPasses();
  mlir::registerPassPipeline(
      "xla-gpu-test-to-inline",
      "Test pipeline of passes up to inlining. No vectorization, also does not "
      "lower xla_gpu. Intended to simplify IR in tests.",
      [=](mlir::OpPassManager& pm, llvm::StringRef options,
          llvm::function_ref<mlir::LogicalResult(const llvm::Twine&)>
              errorHandler) {
        if (!options.empty()) return mlir::failure();

        pm.addNestedPass<mlir::func::FuncOp>(
            xla::gpu::CreateSimplifyArithPass());
        pm.addPass(xla::gpu::CreateEraseDeadFunctionsPass());
        pm.addPass(mlir::createCSEPass());
        pm.addPass(mlir::createInlinerPass({}, [&](mlir::OpPassManager& pm) {
          pm.addPass(mlir::createCSEPass());
        }));
        return mlir::success();
      },
      [](llvm::function_ref<void(const mlir::detail::PassOptions&)>) {});
  mlir::registerPassPipeline(
      "xla-gpu-test-vectorize",
      "Test pipeline for vectorization. Should run after "
      "xla-gpu-test-to-inline.",
      [=](mlir::OpPassManager& pm, llvm::StringRef options,
          llvm::function_ref<mlir::LogicalResult(const llvm::Twine&)>
              errorHandler) {
        if (!options.empty()) return mlir::failure();
        pm.addNestedPass<mlir::func::FuncOp>(
            xla::gpu::CreateLowerXlaGpuLoopsToScfPass());
        pm.addPass(mlir::createLoopInvariantCodeMotionPass());
        pm.addNestedPass<mlir::func::FuncOp>(
            xla::gpu::CreateUnswitchLoopsPass());
        pm.addPass(mlir::createLoopInvariantCodeMotionPass());
        pm.addPass(xla::gpu::CreateFlattenTensorsPass());
        pm.addNestedPass<mlir::func::FuncOp>(
            xla::gpu::CreateVectorizeLoadsAndStoresPass());
        return mlir::success();
      },
      [](llvm::function_ref<void(const mlir::detail::PassOptions&)>) {});

  return mlir::failed(
      MlirOptMain(argc, argv, "XLA MLIR Fusion Pass Driver\n", registry));
}
