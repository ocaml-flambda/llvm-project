//===-- OxCamlDynamicLayoutValue.cpp ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OxCamlDynamicLayoutValue.h"

#include "../../Language/OxCaml/LogChannelOxCaml.h"
#include "../../Language/OxCaml/OxCamlAssert.h"
#include "lldb/Core/Value.h"
#include "lldb/Expression/DWARFExpression.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Scalar.h"
#include "llvm/Support/Error.h"

using namespace lldb;
using namespace lldb_private;

// ============================================================================
// OxCamlDynamicLayoutValue
// ============================================================================

OxCamlDynamicLayoutValue
OxCamlDynamicLayoutValue::MakeConstant(uint64_t value,
                                       OxCamlLayoutValueKind kind) {
  return OxCamlDynamicLayoutValue(kind, InitialStackLayout::Empty, value,
                                  std::nullopt);
}

OxCamlDynamicLayoutValue
OxCamlDynamicLayoutValue::MakeExpression(DWARFExpressionList expression,
                                         OxCamlLayoutValueKind kind,
                                         InitialStackLayout initial_stack) {
  return OxCamlDynamicLayoutValue(kind, initial_stack, 0,
                                  std::move(expression));
}

static Value MakeObjectAddressValue(lldb::addr_t address) {
  Value v;
  v.GetScalar() = address;
  v.SetValueType(Value::ValueType::Scalar);
  return v;
}

// Wrap [error] with an OxCaml-channel log entry and return it. All evaluation
// failures (DWARF errors, missing object address, kind mismatch) flow through
// here so every call site gets consistent logging on the user-visible channel.
// The caller is responsible for emitting a user-visible formatter marker
// (OXCAML_EMIT_MARKER) when the failure surfaces in formatter output.
static llvm::Error LogAndReturnError(llvm::Error error) {
  std::string message = llvm::toString(std::move(error));
  if (Log *log = GetLog(OxCamlLog::UserVisibleErrors))
    LLDB_LOG(log, "OxCamlDynamicLayoutValue evaluation failed: {0}", message);
  return llvm::createStringError(llvm::inconvertibleErrorCode(), message);
}

llvm::Expected<OxCamlLayoutResult> OxCamlDynamicLayoutValue::Evaluate(
    const OxCamlFormatContext &context,
    std::optional<lldb::addr_t> object_address) const {
  if (IsConstant()) {
    if (m_kind == OxCamlLayoutValueKind::Location)
      return OxCamlLayoutResult::MakeObjectRelativeByteOffset(m_constant);
    return OxCamlLayoutResult::MakeScalar(m_kind, m_constant);
  }

  ExecutionContext exe_ctx(context.exe_ctx_ref);

  Value initial_value;
  const Value *initial_value_ptr = nullptr;
  if (m_initial_stack == InitialStackLayout::ObjectAddress) {
    if (!object_address.has_value())
      return LogAndReturnError(llvm::createStringError(
          "OxCaml dynamic layout expression needs the OCaml object address "
          "but none is available"));
    initial_value = MakeObjectAddressValue(*object_address);
    initial_value_ptr = &initial_value;
  }

  Value object_address_value;
  const Value *object_address_ptr = nullptr;
  if (object_address.has_value()) {
    object_address_value = MakeObjectAddressValue(*object_address);
    object_address_ptr = &object_address_value;
  }

  // Pass the current frame's RegisterContext so DWARF expressions that mention
  // registers (DW_OP_breg*, DW_OP_regx, etc.) can be evaluated correctly.
  RegisterContext *reg_ctx = nullptr;
  if (StackFrame *frame = exe_ctx.GetFramePtr())
    reg_ctx = frame->GetRegisterContext().get();

  llvm::Expected<Value> result =
      m_expression->Evaluate(&exe_ctx, reg_ctx,
                             /*func_load_addr=*/LLDB_INVALID_ADDRESS,
                             initial_value_ptr, object_address_ptr);
  if (!result)
    return LogAndReturnError(result.takeError());

  if (m_kind == OxCamlLayoutValueKind::Location) {
    if (result->GetValueType() != Value::ValueType::LoadAddress)
      return LogAndReturnError(llvm::createStringError(
          "OxCaml location-valued expression did not produce a load address "
          "(value-type %d); a trailing DW_OP_stack_value would make a "
          "location-valued attribute scalar by mistake",
          static_cast<int>(result->GetValueType())));
    return OxCamlLayoutResult::MakeLoadAddress(
        result->GetScalar().ULongLong(LLDB_INVALID_ADDRESS));
  }

  if (result->GetValueType() != Value::ValueType::Scalar)
    return LogAndReturnError(llvm::createStringError(
        "OxCaml scalar-valued expression did not produce an implicit scalar "
        "(value-type %d); did the expression omit DW_OP_stack_value?",
        static_cast<int>(result->GetValueType())));
  return OxCamlLayoutResult::MakeScalar(m_kind,
                                        result->GetScalar().ULongLong());
}

// ============================================================================
// OxCamlLayoutResult
// ============================================================================

OxCamlLayoutResult OxCamlLayoutResult::MakeScalar(OxCamlLayoutValueKind kind,
                                                  uint64_t scalar) {
  OX_ASSERT(kind != OxCamlLayoutValueKind::Location,
            "MakeScalar requires a scalar kind (got {0})",
            static_cast<int>(kind));
  return OxCamlLayoutResult(kind, LocationStorage::ObjectRelativeByteOffset,
                            scalar, LLDB_INVALID_ADDRESS, 0);
}

OxCamlLayoutResult
OxCamlLayoutResult::MakeLoadAddress(lldb::addr_t load_address) {
  return OxCamlLayoutResult(OxCamlLayoutValueKind::Location,
                            LocationStorage::LoadAddress, 0, load_address, 0);
}

OxCamlLayoutResult
OxCamlLayoutResult::MakeObjectRelativeByteOffset(uint64_t byte_offset) {
  return OxCamlLayoutResult(OxCamlLayoutValueKind::Location,
                            LocationStorage::ObjectRelativeByteOffset, 0,
                            LLDB_INVALID_ADDRESS, byte_offset);
}

uint64_t OxCamlLayoutResult::GetScalar() const {
  OX_ASSERT(m_kind != OxCamlLayoutValueKind::Location,
            "GetScalar called on a location-valued result (kind={0})",
            static_cast<int>(m_kind));
  return m_scalar;
}

OxCamlLayoutResult::LocationStorage
OxCamlLayoutResult::GetLocationStorage() const {
  OX_ASSERT(m_kind == OxCamlLayoutValueKind::Location,
            "GetLocationStorage called on a non-location result (kind={0})",
            static_cast<int>(m_kind));
  return m_location_storage;
}

lldb::addr_t OxCamlLayoutResult::GetLoadAddress() const {
  OX_ASSERT(m_kind == OxCamlLayoutValueKind::Location &&
                m_location_storage == LocationStorage::LoadAddress,
            "GetLoadAddress called on a non-load-address result (kind={0}, "
            "storage={1})",
            static_cast<int>(m_kind), static_cast<int>(m_location_storage));
  return m_load_address;
}

uint64_t OxCamlLayoutResult::GetObjectRelativeByteOffset() const {
  OX_ASSERT(m_kind == OxCamlLayoutValueKind::Location &&
                m_location_storage == LocationStorage::ObjectRelativeByteOffset,
            "GetObjectRelativeByteOffset called on a non-object-relative "
            "result (kind={0}, storage={1})",
            static_cast<int>(m_kind), static_cast<int>(m_location_storage));
  return m_object_relative_byte_offset;
}
