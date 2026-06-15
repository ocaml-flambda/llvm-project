//===-- DWARFExpression.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_EXPRESSION_DWARFEXPRESSION_H
#define LLDB_EXPRESSION_DWARFEXPRESSION_H

#include "lldb/Core/Address.h"
#include "lldb/Core/Disassembler.h"
#include "lldb/Core/dwarf.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/Scalar.h"
#include "lldb/Utility/Status.h"
#include "lldb/lldb-private.h"
#include "llvm/DebugInfo/DWARF/DWARFLocationExpression.h"
#include "llvm/Support/Error.h"
#include <functional>
#include <optional>
#include <utility>

namespace lldb_private {

class CallEdge;

/// Context for resolving a DW_OP_entry_value that refers to the entry of a
/// function which is not represented by a live stack frame.
///
/// Normally DW_OP_entry_value is resolved by walking the live call stack from
/// the frame being evaluated up to its caller, then finding the call site that
/// produced the activation (see Evaluate_DW_OP_entry_value). That walk is
/// impossible while the stack frame list is still being built, which is when
/// tail-call frames are synthesized: the function whose entry value is
/// referenced was reached through the static call graph and has no frame of
/// its own to walk back from, and re-entering the frame list would deadlock.
///
/// This structure supplies that information explicitly instead. \ref edge is
/// the call that produced the activation whose entry is referenced; its call
/// site parameters map a callee-side location (matched against the
/// DW_OP_entry_value sub-expression) to a caller-side location. That
/// caller-side location is then evaluated either in \ref caller_frame, when
/// the caller is a live frame, or — when the caller is itself a frameless link
/// in a tail-call chain — by recursively resolving any DW_OP_entry_value it
/// contains against \ref outer, chaining back through the tail-call chain until
/// a live frame is reached.
///
/// A frameless caller can only contribute values that are themselves further
/// entry values or constants. If its caller-side location needs state that a
/// vanished activation cannot provide (a register, frame base, or stack slot),
/// evaluation fails rather than producing an unreliable value.
struct EntryValueResolutionContext {
  /// The call edge whose call site parameters describe the entry value. Never
  /// null in a well-formed context.
  const CallEdge *edge = nullptr;

  /// The live frame of the caller (the function containing \ref edge's call
  /// site), or null when that caller is itself a frameless link in a tail-call
  /// chain.
  StackFrame *caller_frame = nullptr;

  /// When \ref caller_frame is null, how to resolve a DW_OP_entry_value that
  /// appears within the caller-side location expression. A null \ref outer
  /// together with a null \ref caller_frame means the chain cannot be resolved.
  const EntryValueResolutionContext *outer = nullptr;
};

/// \class DWARFExpressionDelegate DWARFExpression.h
/// "lldb/Expression/DWARFExpression.h" Interface through which the DWARF
/// expression evaluator accesses the DWARF unit that contains the expression
/// being evaluated. Implemented by the DWARF symbol file plugin's DWARFUnit.
///
/// This is a top-level class (rather than nested in DWARFExpression) so that
/// it can be forward declared by code that may not include Expression
/// headers, such as lldb_private::Value.
class DWARFExpressionDelegate {
public:
  DWARFExpressionDelegate() = default;
  virtual ~DWARFExpressionDelegate() = default;

  virtual uint16_t GetVersion() const = 0;
  virtual dw_addr_t GetBaseAddress() const = 0;
  virtual uint8_t GetAddressByteSize() const = 0;

  /// Return the size in bytes of an offset in the DWARF format of this
  /// unit: 4 bytes for the 32-bit DWARF format, 8 bytes for the 64-bit
  /// DWARF format. This is the size of the operand of DW_OP_call_ref and
  /// of the DIE reference operand of DW_OP_implicit_pointer.
  virtual uint8_t GetDWARFOffsetByteSize() const = 0;

  virtual llvm::Expected<std::pair<uint64_t, bool>>
  GetDIEBitSizeAndSign(uint64_t relative_die_offset) const = 0;

  /// Return the DW_AT_location expression of the DIE referenced by a
  /// DW_OP_call2, DW_OP_call4, DW_OP_call_ref or DW_OP_implicit_pointer
  /// operation, together with the delegate of the unit that contains the
  /// expression (which may not be this unit when the offset is not unit
  /// relative).
  ///
  /// \param[in] die_offset
  ///     The offset of the referenced DIE. If \a unit_relative is true,
  ///     the offset is relative to the start of this unit
  ///     (DW_OP_call2/DW_OP_call4), otherwise it is an offset in the
  ///     .debug_info section (DW_OP_call_ref, DW_OP_implicit_pointer).
  ///
  /// \param[in] pc_function_offset
  ///     The offset of the program counter from the entry point of the
  ///     function the frame is executing in, or std::nullopt when there is
  ///     no frame or function. When the referenced DIE's DW_AT_location is
  ///     a location list rather than a single expression, this selects the
  ///     list entry that applies. A function-relative offset is used
  ///     because location list ranges are expressed in the address space
  ///     of the DWARF that defines them, which is not necessarily the
  ///     address space the program runs in (for example on Darwin without
  ///     a dSYM, where the DWARF lives in the .o files); the offset is the
  ///     same in both spaces and is re-anchored at the DWARF-side
  ///     function's entry point.
  ///
  /// \return
  ///     The location expression data and the delegate to evaluate it
  ///     with. An empty (zero length) DataExtractor is returned when the
  ///     referenced DIE exists but has no location description that
  ///     applies: either it has no DW_AT_location attribute at all, or the
  ///     attribute is a location list none of whose entries cover the
  ///     program counter. The caller decides what that means (for the
  ///     DW_OP_call operations the call has no effect, which
  ///     compiler-emitted guard expressions rely on to detect unavailable
  ///     values; for DW_OP_implicit_pointer the pointed-at value is
  ///     unavailable). An error is returned if the DIE cannot be resolved,
  ///     or if its DW_AT_location is a location list and
  ///     \a pc_function_offset is unknown (the applicable entry then
  ///     cannot be determined at all).
  virtual llvm::Expected<
      std::pair<DataExtractor, const DWARFExpressionDelegate *>>
  GetDIELocationExpression(uint64_t die_offset, bool unit_relative,
                           std::optional<uint64_t> pc_function_offset)
      const = 0;

  /// Return the value of the DW_AT_const_value attribute of the DIE at
  /// \a die_offset in the .debug_info section. This is used when
  /// dereferencing a DW_OP_implicit_pointer whose referenced DIE describes
  /// the pointed-at value with a constant rather than a location. Block and
  /// string forms are returned as host byte buffers, integer forms as
  /// scalars. Returns std::nullopt when the DIE exists but has no
  /// DW_AT_const_value attribute, and an error when the DIE cannot be
  /// resolved.
  virtual llvm::Expected<std::optional<Value>>
  GetDIEConstValue(uint64_t die_offset) const = 0;

  virtual dw_addr_t ReadAddressFromDebugAddrSection(uint32_t index) const = 0;
  virtual lldb::offset_t
  GetVendorDWARFOpcodeSize(const DataExtractor &data,
                           const lldb::offset_t data_offset,
                           const uint8_t op) const = 0;
  virtual bool ParseVendorDWARFOpcode(uint8_t op, const DataExtractor &opcodes,
                                      lldb::offset_t &offset,
                                      std::vector<Value> &stack) const = 0;

  DWARFExpressionDelegate(const DWARFExpressionDelegate &) = delete;
  DWARFExpressionDelegate &operator=(const DWARFExpressionDelegate &) = delete;
};

/// \class DWARFExpression DWARFExpression.h
/// "lldb/Expression/DWARFExpression.h" Encapsulates a DWARF location
/// expression and interprets it.
///
/// DWARF location expressions are used in two ways by LLDB.  The first
/// use is to find entities specified in the debug information, since their
/// locations are specified in precisely this language.  The second is to
/// interpret expressions without having to run the target in cases where the
/// overhead from copying JIT-compiled code into the target is too high or
/// where the target cannot be run.  This class encapsulates a single DWARF
/// location expression or a location list and interprets it.
class DWARFExpression {
public:
  using Stack = std::vector<Value>;

  /// Compatibility alias; the delegate predates its hoisting to a top-level
  /// class and is widely referred to as DWARFExpression::Delegate.
  using Delegate = DWARFExpressionDelegate;

  DWARFExpression();

  /// Constructor
  ///
  /// \param[in] data
  ///     A data extractor configured to read the DWARF location expression's
  ///     bytecode.
  DWARFExpression(const DataExtractor &data);

  /// Destructor
  ~DWARFExpression();

  /// Return true if the location expression contains data
  bool IsValid() const;

  /// Return the address specified by the first
  /// DW_OP_{addr, addrx, GNU_addr_index} in the operation stream.
  ///
  /// \param[in] dwarf_cu
  ///     The dwarf unit this expression belongs to. Only required to resolve
  ///     DW_OP{addrx, GNU_addr_index}.
  ///
  /// \return
  ///     The address specified by the operation, if the operation exists, or
  ///     an llvm::Error otherwise.
  llvm::Expected<lldb::addr_t>
  GetLocation_DW_OP_addr(const Delegate *dwarf_cu) const;

  bool Update_DW_OP_addr(const Delegate *dwarf_cu, lldb::addr_t file_addr);

  void UpdateValue(uint64_t const_value, lldb::offset_t const_value_byte_size,
                   uint8_t addr_byte_size);

  bool ContainsThreadLocalStorage(const Delegate *dwarf_cu) const;

  bool LinkThreadLocalStorage(
      const Delegate *dwarf_cu,
      std::function<lldb::addr_t(lldb::addr_t file_addr)> const
          &link_address_callback);

  /// Return the call-frame-info style register kind
  lldb::RegisterKind GetRegisterKind() const;

  /// Set the call-frame-info style register kind
  ///
  /// \param[in] reg_kind
  ///     The register kind.
  void SetRegisterKind(lldb::RegisterKind reg_kind);

  /// Evaluate a DWARF location expression in a particular context
  ///
  /// \param[in] exe_ctx
  ///     The execution context in which to evaluate the location
  ///     expression.  The location expression may access the target's
  ///     memory, especially if it comes from the expression parser.
  ///
  /// \param[in] opcode_ctx
  ///     The module which defined the expression.
  ///
  /// \param[in] opcodes
  ///     This is a static method so the opcodes need to be provided
  ///     explicitly.
  ///
  ///  \param[in] reg_ctx
  ///     An optional parameter which provides a RegisterContext for use
  ///     when evaluating the expression (i.e. for fetching register values).
  ///     Normally this will come from the ExecutionContext's StackFrame but
  ///     in the case where an expression needs to be evaluated while building
  ///     the stack frame list, this short-cut is available.
  ///
  /// \param[in] reg_set
  ///     The call-frame-info style register kind.
  ///
  /// \param[in] initial_value_ptr
  ///     A value to put on top of the interpreter stack before evaluating
  ///     the expression, if the expression is parametrized.  Can be NULL.
  ///
  /// \param[in] result
  ///     A value into which the result of evaluating the expression is
  ///     to be placed.
  ///
  /// \param[in] error_ptr
  ///     If non-NULL, used to report errors in expression evaluation.
  ///
  /// \return
  ///     True on success; false otherwise.  If error_ptr is non-NULL,
  ///     details of the failure are provided through it.
  /// \param[in] entry_value_ctx
  ///     Optional explicit context for resolving a DW_OP_entry_value whose
  ///     referenced function has no live stack frame (used while synthesizing
  ///     tail-call frames). When null, DW_OP_entry_value is resolved by walking
  ///     the live call stack instead. \see EntryValueResolutionContext.
  static llvm::Expected<Value>
  Evaluate(ExecutionContext *exe_ctx, RegisterContext *reg_ctx,
           lldb::ModuleSP module_sp, const DataExtractor &opcodes,
           const Delegate *dwarf_cu, const lldb::RegisterKind reg_set,
           const Value *initial_value_ptr, const Value *object_address_ptr,
           const EntryValueResolutionContext *entry_value_ctx = nullptr);

  /// Materialize the value an implicit pointer points at.
  ///
  /// \a implicit_pointer must be a Value of type
  /// Value::ValueType::ImplicitPointer, i.e. the result of evaluating a
  /// DWARF expression that ends in DW_OP_implicit_pointer: a pointer that
  /// does not exist in the process being debugged, but whose pointed-at
  /// value is described by another DIE. This function evaluates the
  /// DW_AT_location of that DIE in the given context and applies the
  /// implicit pointer's byte offset, yielding the pointed-at value. The
  /// result can be any kind of Value: a load address (the pointee does
  /// exist in the process's memory), a host address or scalar (the pointee
  /// only exists in the debugger), or another implicit pointer (the pointee
  /// is itself an eliminated pointer); in the latter cases the value lives
  /// in LLDB's own address space, never the process's.
  ///
  /// Note that failure is always reported as an error: a pointee that is
  /// unavailable (e.g. the referenced DIE has no DW_AT_location) is never
  /// conflated with a pointee that evaluates to the value zero.
  static llvm::Expected<Value>
  DereferenceImplicitPointer(const Value &implicit_pointer,
                             ExecutionContext *exe_ctx,
                             RegisterContext *reg_ctx,
                             lldb::ModuleSP module_sp);

  bool GetExpressionData(DataExtractor &data) const {
    data = m_data;
    return data.GetByteSize() > 0;
  }

  void DumpLocation(Stream *s, lldb::DescriptionLevel level, ABI *abi) const;

  bool MatchesOperand(StackFrame &frame, const Instruction::Operand &op) const;

private:
  /// A data extractor capable of reading opcode bytes
  DataExtractor m_data;

  /// One of the defines that starts with LLDB_REGKIND_
  lldb::RegisterKind m_reg_kind = lldb::eRegisterKindDWARF;
};

} // namespace lldb_private

#endif // LLDB_EXPRESSION_DWARFEXPRESSION_H
