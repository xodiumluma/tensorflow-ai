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

#include "xla/service/gpu/fusions/triton/emitter_helpers.h"

#include <cstdint>
#include <variant>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/TargetParser/Triple.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypeInterfaces.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/Value.h"
#include "mlir/IR/ValueRange.h"
#include "mlir/Support/LLVM.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/mlir_hlo/mhlo/IR/hlo_ops.h"
#include "xla/mlir_hlo/mhlo/transforms/map_mhlo_to_scalar_op.h"
#include "xla/mlir_hlo/mhlo/transforms/transformation_helpers.h"
#include "xla/primitive_util.h"
#include "xla/service/gpu/target_util.h"
#include "xla/service/llvm_ir/llvm_util.h"
#include "xla/stream_executor/device_description.h"
#include "xla/xla.pb.h"
#include "tsl/platform/statusor.h"
#include "triton/Dialect/Triton/IR/Dialect.h"

namespace xla::gpu::triton {

using ::llvm::SmallVector;
using ::mlir::ArrayRef;
using ::mlir::ImplicitLocOpBuilder;
using ::mlir::ShapedType;
using ::mlir::Type;
using ::mlir::Value;
using ::mlir::ValueRange;

namespace ma = ::mlir::arith;
namespace mm = ::mlir::math;
namespace mt = ::mlir::triton;

ScalarOrTensor::ScalarOrTensor(mlir::Value value) {
  if (auto tt = mlir::dyn_cast<mlir::RankedTensorType>(value.getType())) {
    CHECK_GT(tt.getRank(), 0);
    value_ = TensorValue{value};
  } else {
    value_ = ScalarValue{value};
  }
}

SmallVector<int64_t> GetPaddedTileSizes(ArrayRef<int64_t> tile_sizes) {
  SmallVector<int64_t> result;
  result.reserve(tile_sizes.size());
  for (int64_t value : tile_sizes) {
    result.push_back(llvm::PowerOf2Ceil(value));
  }
  return result;
}

absl::StatusOr<Type> TritonType(mlir::OpBuilder b, PrimitiveType t) {
  switch (t) {
    case F64:
      return b.getF64Type();
    case F32:
      return b.getF32Type();
    case F16:
      return b.getF16Type();
    case BF16:
      return b.getBF16Type();
    case S64:
      return b.getI64Type();
    case S32:
      return b.getI32Type();
    case S16:
      return b.getI16Type();
    case PRED:
      return b.getI1Type();
    case S8:
      return b.getI8Type();
    case S4:  // The unpacking to i8 is supported by the emitter.
      // We pass the s4 tensor as i8 tensor with the minor dimension having 2x
      // less elements and unpack in the inner loop of the triton kernel.
      return b.getI8Type();
    case F8E5M2:
      return b.getFloat8E5M2Type();
    case F8E4M3FN:
      return b.getFloat8E4M3FNType();
    default:
      return absl::UnimplementedError(
          absl::StrCat("This type is not supported yet: ",
                       primitive_util::LowercasePrimitiveTypeName(t)));
  }
}

Type StorageType(mlir::OpBuilder b, Type t) {
  if (t.isInteger(1)) {
    return b.getI8Type();
  }
  return t;
}

bool IsFp8Type(Type t) {
  return t.isFloat8E5M2() || t.isFloat8E4M3FN() || t.isFloat8E5M2FNUZ() ||
         t.isFloat8E4M3FNUZ() || t.isFloat8E4M3B11FNUZ();
}

Value Cast(ImplicitLocOpBuilder& b, Value value, Type dst_element_ty) {
  Type src_ty = value.getType();
  Type src_element_ty = src_ty;
  Type fp32_ty = b.getF32Type();
  Type dst_ty = dst_element_ty;
  if (auto src_shaped_ty = mlir::dyn_cast<ShapedType>(src_ty)) {
    src_element_ty = src_shaped_ty.getElementType();
    dst_ty = src_shaped_ty.clone(src_shaped_ty.getShape(), dst_element_ty);
    fp32_ty = src_shaped_ty.clone(src_shaped_ty.getShape(), b.getF32Type());
  }
  if (src_ty == dst_ty) {
    return value;
  }

  // All operations on bf16 are done through f32.
  if (src_element_ty.isBF16()) {
    return Cast(b, b.create<ma::ExtFOp>(fp32_ty, value), dst_element_ty);
  }
  if (dst_element_ty.isBF16()) {
    // S8 -> BF16 is directly supported and doesn't need to go through f32.
    if (!src_element_ty.isInteger(8)) {
      return b.create<ma::TruncFOp>(dst_ty, Cast(b, value, b.getF32Type()));
    }
  }

  // float => float
  auto src_fp_element_ty = mlir::dyn_cast<mlir::FloatType>(src_element_ty);
  auto dst_fp_element_ty = mlir::dyn_cast<mlir::FloatType>(dst_element_ty);
  if (src_fp_element_ty && dst_fp_element_ty) {
    // F8 <-> FP16, BF16, FP32, FP64 need to be handled via Triton's tt.fp_to_fp
    // because LLVM doesn't support casts from/to FP8.
    // TODO(b/266862493): Add end-to-end test once FP8 support lands in XLA as
    // we can't test the code below without patching the feature.
    if (IsFp8Type(src_element_ty)) {
      return b.create<mt::FpToFpOp>(dst_ty, value);
    }
    if (IsFp8Type(dst_element_ty)) {
      return b.create<mt::FpToFpOp>(
          dst_ty, value,
          mt::RoundingModeAttr::get(b.getContext(), mt::RoundingMode::RTNE));
    }

    if (src_fp_element_ty.getFPMantissaWidth() >
        dst_fp_element_ty.getFPMantissaWidth()) {
      return b.create<ma::TruncFOp>(dst_ty, value);
    } else {
      return b.create<ma::ExtFOp>(dst_ty, value);
    }
  }
  // int => int
  if (mlir::isa<mlir::IntegerType>(src_element_ty) &&
      mlir::isa<mlir::IntegerType>(dst_element_ty)) {
    if (src_element_ty.getIntOrFloatBitWidth() <
        dst_element_ty.getIntOrFloatBitWidth()) {
      if (src_element_ty.isInteger(1)) {
        return b.create<ma::ExtUIOp>(dst_ty, value);
      }
      return b.create<ma::ExtSIOp>(dst_ty, value);
    }
    return b.create<ma::TruncIOp>(dst_ty, value);
  }
  // int => float
  if (mlir::isa<mlir::IntegerType>(src_element_ty) && dst_fp_element_ty) {
    // TODO(b/266862493): Support unsigned integer types.
    if (src_element_ty.isInteger(1)) {
      return b.create<ma::UIToFPOp>(dst_ty, value);
    }
    return b.create<ma::SIToFPOp>(dst_ty, value);
  }
  // float => int
  if (src_fp_element_ty && mlir::isa<mlir::IntegerType>(dst_element_ty)) {
    if (dst_element_ty.isInteger(1)) {
      return b.create<ma::CmpFOp>(ma::CmpFPredicate::UNE, value,
                                  ZerosLike(b, value));
    }
    // TODO(b/266862493): Support unsigned integer types.
    // The current logic handles signed integer types only. Additional handling
    // is needed for unsigned integer types.
    auto cst_int = [&](int64_t x) {
      if (auto src_shaped_ty = mlir::dyn_cast<ShapedType>(src_ty)) {
        return CreateConst(b, dst_element_ty, x, src_shaped_ty.getShape())
            .UnwrapUnsafe();
      } else {
        return CreateConst(b, dst_element_ty, x).UnwrapUnsafe();
      }
    };
    auto cst_float = [&](int64_t x) {
      if (auto src_shaped_ty = mlir::dyn_cast<ShapedType>(src_ty)) {
        return CreateConst(b, src_fp_element_ty, x, src_shaped_ty.getShape())
            .UnwrapUnsafe();
      } else {
        return CreateConst(b, src_fp_element_ty, x).UnwrapUnsafe();
      }
    };
    auto fptosi = b.create<ma::FPToSIOp>(dst_ty, value);
    int64_t min = llvm::minIntN(dst_element_ty.getIntOrFloatBitWidth());
    int64_t max = llvm::maxIntN(dst_element_ty.getIntOrFloatBitWidth());

    // value <= static_cast<float>(INT_MIN) ? INT_MIN : ...
    auto clamped = b.create<mlir::arith::SelectOp>(
        b.create<mlir::arith::CmpFOp>(mlir::arith::CmpFPredicate::OLE, value,
                                      cst_float(min)),
        cst_int(min), fptosi);
    // value >= static_cast<float>(INT_MAX) ? INT_MAX : ...
    clamped = b.create<mlir::arith::SelectOp>(
        b.create<mlir::arith::CmpFOp>(mlir::arith::CmpFPredicate::OGE, value,
                                      cst_float(max)),
        cst_int(max), clamped);
    // isnan(value) ? 0 : ...
    return b.create<mlir::arith::SelectOp>(
        b.create<mlir::arith::CmpFOp>(mlir::arith::CmpFPredicate::UNO, value,
                                      value),
        cst_int(0), clamped);
  }

  LOG(FATAL) << "Type conversion not supported: "
             << llvm_ir::DumpToString(src_element_ty) << " -> "
             << llvm_ir::DumpToString(dst_element_ty);
}

Value Subtract(ImplicitLocOpBuilder& b, ValueRange values) {
  if (mlir::isa<mlir::IntegerType>(mlir::getElementTypeOrSelf(values[0]))) {
    return b.create<ma::SubIOp>(values[0], values[1]);
  } else {
    return b.create<ma::SubFOp>(values[0], values[1]);
  }
}

Value Compare(ImplicitLocOpBuilder& b, ValueRange values,
              mlir::mhlo::ComparisonDirection direction) {
  const Type type = mlir::getElementTypeOrSelf(values[0]);
  if (mlir::isa<mlir::IntegerType>(type)) {
    return b.create<ma::CmpIOp>(
        mlir::mhlo::impl::getCmpPredicate<ma::CmpIPredicate>(
            direction,
            /*isSigned=*/!type.isInteger(1))
            .value(),
        values[0], values[1]);
  }
  return b.create<ma::CmpFOp>(
      mlir::mhlo::impl::getCmpPredicate<ma::CmpFPredicate>(direction,
                                                           /*isSigned=*/true)
          .value(),
      values[0], values[1]);
}

Value Maximum(ImplicitLocOpBuilder& b, const se::DeviceDescription& device_info,
              ValueRange values) {
  if (mlir::isa<mlir::FloatType>(mlir::getElementTypeOrSelf(values[0]))) {
    return b.create<ma::MaximumFOp>(values);
  }
  // logic: isNaN(lhs) || (!isNan(rhs) && lhs >= rhs) ? lhs : rhs
  // See also: IEEE Std 754-2008 5.11.
  //
  // This also works, but we wanted to make it similar to minimum.
  // logic: isNaN(lhs) || lhs >= rhs ? lhs : rhs
  Value lhs_is_nan =
      Compare(b, {values[0], values[0]}, mlir::mhlo::ComparisonDirection::NE);
  Value rhs_is_not_nan =
      Compare(b, {values[1], values[1]}, mlir::mhlo::ComparisonDirection::EQ);
  Value lhs_is_ge = Compare(b, values, mlir::mhlo::ComparisonDirection::GE);
  return b.create<ma::SelectOp>(
      b.create<ma::OrIOp>(lhs_is_nan,
                          b.create<ma::AndIOp>(rhs_is_not_nan, lhs_is_ge)),
      values[0], values[1]);
}

Value Minimum(ImplicitLocOpBuilder& b, const se::DeviceDescription& device_info,
              ValueRange values) {
  if (mlir::isa<mlir::FloatType>(mlir::getElementTypeOrSelf(values[0]))) {
    return b.create<ma::MinimumFOp>(values);
  }
  // logic: isNaN(lhs) || (!isNan(rhs) && lhs <= rhs) ? lhs : rhs
  // See also: IEEE Std 754-2008 5.11.
  //
  // This should also work, but the tests show that it doesn't work for
  // minimum(x, NaN):
  // logic: isNaN(lhs) || lhs <= rhs ? lhs : rhs
  Value lhs_is_nan =
      Compare(b, {values[0], values[0]}, mlir::mhlo::ComparisonDirection::NE);
  Value rhs_is_not_nan =
      Compare(b, {values[1], values[1]}, mlir::mhlo::ComparisonDirection::EQ);
  Value lhs_is_le = Compare(b, values, mlir::mhlo::ComparisonDirection::LE);
  return b.create<ma::SelectOp>(
      b.create<ma::OrIOp>(lhs_is_nan,
                          b.create<ma::AndIOp>(rhs_is_not_nan, lhs_is_le)),
      values[0], values[1]);
}

ScalarOrTensor Splat(ImplicitLocOpBuilder& b, ScalarOrTensor value,
                     ArrayRef<int64_t> shape) {
  CHECK(!shape.empty());
  auto type = mlir::RankedTensorType::get(shape, value.Type());
  return ScalarOrTensor(b.create<mt::SplatOp>(type, value.UnwrapUnsafe()));
}

absl::StatusOr<Value> EmitElementwise(ImplicitLocOpBuilder& b,
                                      absl::string_view libdevice_path,
                                      const se::DeviceDescription& device_info,
                                      const HloInstruction& hlo,
                                      ValueRange inputs) {
  if (mlir::getElementTypeOrSelf(inputs[0]).isF32() ||
      mlir::getElementTypeOrSelf(inputs[0]).isF64()) {
    auto dev_fn_id = GetTargetDeviceFunctionID(hlo.opcode());
    if (dev_fn_id.ok()) {
      llvm::Triple triple("nvptx64-unknown-unknown");
      if (std::holds_alternative<se::RocmComputeCapability>(
              device_info.gpu_compute_capability())) {
        triple.setTriple("amdgcn-unknown-unknown");
      }
      return b.create<mt::ExternElementwiseOp>(
          inputs[0].getType(), inputs, "libdevice", libdevice_path,
          ObtainDeviceFunctionName(dev_fn_id.value(),
                                   hlo.shape().element_type(), triple),
          /*pure=*/true);
    }
  }
  const bool is_integer =
      mlir::isa<mlir::IntegerType>(mlir::getElementTypeOrSelf(inputs[0]));

  switch (hlo.opcode()) {
    case HloOpcode::kCopy:
      // Dimension transformations are taken care of separately.
      return inputs[0];
    case HloOpcode::kAbs:
      if (is_integer) {
        return b.create<mm::AbsIOp>(inputs[0]);
      }
      return b.create<mm::AbsFOp>(inputs[0]);
    case HloOpcode::kCeil:
      return b.create<mm::CeilOp>(inputs[0]);
    case HloOpcode::kFloor:
      return b.create<mm::FloorOp>(inputs[0]);
    case HloOpcode::kNot:
      return b.create<ma::XOrIOp>(inputs[0], OnesLike(b, inputs[0]));
    case HloOpcode::kNegate:
      // NegFOp is not supported by Triton.
      return Subtract(b, {ZerosLike(b, inputs[0]), inputs[0]});
    case HloOpcode::kConvert: {
      TF_ASSIGN_OR_RETURN(Type dst_ty,
                          TritonType(b, hlo.shape().element_type()));
      return Cast(b, inputs[0], dst_ty);
    }
    case HloOpcode::kAdd:
      if (is_integer) {
        return b.create<ma::AddIOp>(inputs[0], inputs[1]);
      }
      return b.create<ma::AddFOp>(inputs[0], inputs[1]);
    case HloOpcode::kSubtract:
      return Subtract(b, inputs);
    case HloOpcode::kMultiply:
      if (is_integer) {
        return b.create<ma::MulIOp>(inputs[0], inputs[1]);
      }
      return b.create<ma::MulFOp>(inputs[0], inputs[1]);
    case HloOpcode::kMaximum:
      return Maximum(b, device_info, inputs);
    case HloOpcode::kMinimum:
      return Minimum(b, device_info, inputs);
    case HloOpcode::kClamp:
      return Maximum(
          b, device_info,
          {Minimum(b, device_info, {inputs[1], inputs[2]}), inputs[0]});
    case HloOpcode::kAnd:
      return b.create<ma::AndIOp>(inputs[0], inputs[1]);
    case HloOpcode::kOr:
      return b.create<ma::OrIOp>(inputs[0], inputs[1]);
    case HloOpcode::kXor:
      return b.create<ma::XOrIOp>(inputs[0], inputs[1]);
    case HloOpcode::kDivide:
      if (is_integer) {
        // Unsigned not supported yet.
        return b.create<ma::DivSIOp>(inputs[0], inputs[1]);
      }
      return b.create<ma::DivFOp>(inputs[0], inputs[1]);
    case HloOpcode::kCompare:
      return Compare(
          b, inputs,
          mlir::mhlo::symbolizeComparisonDirection(
              ComparisonDirectionToString(hlo.comparison_direction()))
              .value());
    case HloOpcode::kSelect:
      return b.create<ma::SelectOp>(
          Compare(b, {inputs[0], ZerosLike(b, inputs[0])},
                  mlir::mhlo::ComparisonDirection::NE),
          inputs[1], inputs[2]);
    case HloOpcode::kReducePrecision:
      return mlir::mhlo::reducePrecision<mt::BitcastOp>(
          b.getLoc(), inputs[0], hlo.exponent_bits(), hlo.mantissa_bits(), &b);
    default:
      return absl::InvalidArgumentError(
          absl::StrCat("Unsupported elementwise operation ", hlo.ToString()));
  }
}

absl::StatusOr<ScalarOrTensor> EmitConstant(ImplicitLocOpBuilder& b,
                                            const HloInstruction& constant) {
  TF_ASSIGN_OR_RETURN(Type ty, TritonType(b, constant.shape().element_type()));
  llvm::SmallVector<int64_t> shape{constant.shape().dimensions().begin(),
                                   constant.shape().dimensions().end()};

  if (constant.shape().IsInteger()) {
    if (constant.shape().element_type() == U64) {
      return CreateConst(b, ty, ScalarConstantValue<uint64_t>(constant, U64),
                         shape);
    } else {
      return CreateConst(b, ty, ScalarConstantValue<int64_t>(constant, S64),
                         shape);
    }
  }
  return CreateConst(b, ty, ScalarConstantValue<double>(constant, F64), shape);
}

// Emit sequence of operations for unpacking 2xi4 -> i8.
absl::StatusOr<Value> EmitUnpackInt4(ImplicitLocOpBuilder& b,
                                     const HloInstruction* hlo,
                                     int64_t unpack_dim_idx, Value value) {
  VLOG(6) << "EmitUnpackInt4: " << hlo->ToString();
  auto input_type = mlir::cast<mlir::RankedTensorType>(value.getType());
  if (input_type.getShape().size() != 2) {
    return absl::InvalidArgumentError(
        absl::StrCat("UnpackInt4 works only for 2d inputs: ", hlo->ToString()));
  }
  // We use shifts instead the mask because we need to keep the sign bit.
  Value shift4 =
      Splat(b, CreateConst(b, b.getI8Type(), 4), input_type.getShape())
          .UnwrapUnsafe();
  Value lo = b.create<ma::ShRSIOp>(b.create<ma::ShLIOp>(value, shift4), shift4);
  Value hi = b.create<ma::ShRSIOp>(value, shift4);
  Value result = b.create<mt::JoinOp>(hi, lo);
  if (unpack_dim_idx == 0) {
    result = b.create<mt::TransOp>(result, b.getDenseI32ArrayAttr({0, 2, 1}));
  }
  SmallVector<int64_t> result_shape(input_type.getShape());
  result_shape[unpack_dim_idx] *= 2;
  auto type = mlir::RankedTensorType::get(result_shape, b.getI8Type());
  return b.create<mt::ReshapeOp>(type, result, /*allow_reorder=*/false);
}

}  // namespace xla::gpu::triton
