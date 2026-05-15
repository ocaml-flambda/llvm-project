//===-- OxCamlDynamicLayoutValue.h ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_TYPESYSTEM_OXCAML_OXCAMLDYNAMICLAYOUTVALUE_H
#define LLDB_SOURCE_PLUGINS_TYPESYSTEM_OXCAML_OXCAMLDYNAMICLAYOUTVALUE_H

#include "../../Language/OxCaml/OxCamlAssert.h"
#include "lldb/Expression/DWARFExpressionList.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/lldb-private.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <optional>

namespace lldb_private {

class OxCamlLayoutResult;

/// Describes the unit of a dynamic-layout value's result.
///
/// - ScalarBits/ScalarBytes/ScalarElements: the value is a scalar quantity
///   whose unit is fixed by the attribute it came from (e.g. a byte size, a
///   bit offset, an element count). Scalar exprlocs end with
///   DW_OP_stack_value.
/// - Location: the value identifies a memory location. Constants are
///   object-relative byte offsets; exprlocs evaluate to a load address.
enum class OxCamlLayoutValueKind {
  ScalarBits,
  ScalarBytes,
  ScalarElements,
  Location,
};

/// Session-invariant context for the formatter: process, frame, byte order,
/// pointer size. Set once at the entry point and threaded by const reference
/// through recursive calls. Per-recursion state (e.g. the OCaml object
/// address inherited from an enclosing FormatPointer) is passed as a
/// separate parameter.
struct OxCamlFormatContext {
  lldb::ProcessSP process_sp;
  ExecutionContextRef exe_ctx_ref;
  lldb::ByteOrder byte_order;
  uint32_t address_byte_size;
};

/// A layout fact that may be a constant or a DWARF expression.
///
/// The wrapper is generic over the result's value-kind and the expression's
/// initial-stack convention. Parsing helpers fix the (kind, initial_stack)
/// pair per DWARF attribute, since DWARF carries no signal that varies per
/// occurrence.
class OxCamlDynamicLayoutValue {
public:
  /// What the DWARF evaluator should pre-load on the expression stack before
  /// running the expression.
  enum class InitialStackLayout { Empty, ObjectAddress };

  /// Build a constant layout value. For Location kind, the constant is an
  /// object-relative byte offset.
  static OxCamlDynamicLayoutValue MakeConstant(uint64_t value,
                                               OxCamlLayoutValueKind kind);

  static OxCamlDynamicLayoutValue
  MakeExpression(DWARFExpressionList expression, OxCamlLayoutValueKind kind,
                 InitialStackLayout initial_stack);

  bool IsConstant() const { return !m_expression.has_value(); }
  OxCamlLayoutValueKind GetKind() const { return m_kind; }
  InitialStackLayout GetInitialStack() const { return m_initial_stack; }

  /// Returns the constant value. Asserts when IsConstant() is false, because
  /// reading the constant out of an expression-valued layout would indicate a
  /// bug in the caller.
  uint64_t GetConstant() const {
    OX_ASSERT(IsConstant(),
              "GetConstant() called on a non-constant OxCamlDynamicLayoutValue "
              "(kind={0})",
              static_cast<int>(m_kind));
    return m_constant;
  }

  /// Evaluate the layout value. Constants wrap the stored constant.
  /// Expressions invoke the LLDB DWARF evaluator with [object_address] as
  /// the raw OCaml object pointer; evaluation fails cleanly when the
  /// expression needs an object address (via the initial-stack convention
  /// or DW_OP_push_object_address) but none is supplied.
  llvm::Expected<OxCamlLayoutResult>
  Evaluate(const OxCamlFormatContext &context,
           std::optional<lldb::addr_t> object_address) const;

private:
  OxCamlDynamicLayoutValue(OxCamlLayoutValueKind kind,
                           InitialStackLayout initial_stack, uint64_t constant,
                           std::optional<DWARFExpressionList> expression)
      : m_kind(kind), m_initial_stack(initial_stack), m_constant(constant),
        m_expression(std::move(expression)) {}

  OxCamlLayoutValueKind m_kind;
  InitialStackLayout m_initial_stack;
  uint64_t m_constant = 0;
  std::optional<DWARFExpressionList> m_expression;
};

/// Tagged result of evaluating an OxCamlDynamicLayoutValue.
///
/// The kind drives which accessor is valid:
/// - ScalarBits/ScalarBytes/ScalarElements: GetScalar() returns the value in
///   the indicated unit.
/// - Location: GetLocationStorage() indicates whether the result is an
///   absolute LoadAddress or an ObjectRelativeByteOffset still to be combined
///   with the OCaml object pointer.
///
/// Misuse is a debug assertion, not a runtime fallback.
class OxCamlLayoutResult {
public:
  enum class LocationStorage { LoadAddress, ObjectRelativeByteOffset };

  static OxCamlLayoutResult MakeScalar(OxCamlLayoutValueKind kind,
                                       uint64_t scalar);
  static OxCamlLayoutResult MakeLoadAddress(lldb::addr_t load_address);
  static OxCamlLayoutResult MakeObjectRelativeByteOffset(uint64_t byte_offset);

  OxCamlLayoutValueKind GetKind() const { return m_kind; }

  uint64_t GetScalar() const;

  LocationStorage GetLocationStorage() const;
  lldb::addr_t GetLoadAddress() const;
  uint64_t GetObjectRelativeByteOffset() const;

private:
  OxCamlLayoutResult(OxCamlLayoutValueKind kind,
                     LocationStorage location_storage, uint64_t scalar,
                     lldb::addr_t load_address,
                     uint64_t object_relative_byte_offset)
      : m_kind(kind), m_location_storage(location_storage), m_scalar(scalar),
        m_load_address(load_address),
        m_object_relative_byte_offset(object_relative_byte_offset) {}

  OxCamlLayoutValueKind m_kind;
  LocationStorage m_location_storage;
  uint64_t m_scalar;
  lldb::addr_t m_load_address;
  uint64_t m_object_relative_byte_offset;
};

} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_TYPESYSTEM_OXCAML_OXCAMLDYNAMICLAYOUTVALUE_H
