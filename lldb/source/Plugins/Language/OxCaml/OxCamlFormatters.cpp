//===-- OxCamlFormatters.cpp ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// \file
/// This file implements formatters for OCaml values using a uniform
/// DataExtractor-based approach.
///
/// ## Architecture Overview
///
/// All formatters in this file use DataExtractor as the universal abstraction
/// for "bytes to format". This eliminates the confusion between values
/// (immediates in registers) and addresses (pointers to memory).
///
/// ## DataExtractor Approach
///
/// DataExtractor is an abstraction for a contiguous buffer of bytes that
/// can be read with proper byte-order handling. In our formatter architecture:
///
/// 1. **Entry Point**: The summary provider receives a DataExtractor from the
///    ValueObject containing the initial 8 bytes (either an immediate value or
///    a pointer).
///
/// 2. **Uniform Interface**: Every formatter function takes a DataExtractor as
///    input, not raw uint64_t values. This creates a consistent interface
///    throughout the formatting pipeline.
///
/// 3. **Type-Specific Handling**:
///    - **Base/Enum types**: Read the value directly from the DataExtractor
///    - **Pointers**: Read the pointer value, fetch pointed-to memory into a
///      new DataExtractor, then recursively format
///    - **Structures**: Create sub-DataExtractors for each member at their
///      respective offsets
///    - **Typedefs**: Pass the DataExtractor through unchanged
///
/// 4. **Memory Reading**: When a pointer needs to be dereferenced (e.g., for
///    structures), FormatPointer reads the entire pointed-to object into a new
///    DataExtractor. This new DataExtractor is then passed to the appropriate
///    formatter.
///
/// 5. **Structure Members**: FormatStructure creates a sub-DataExtractor for
///    each member using the DataExtractor(parent, offset, size) constructor.
///    This provides each member formatter with exactly the bytes it needs.

#include "OxCamlFormatters.h"
#include "LogChannelOxCaml.h"
#include "OxCamlAssert.h"
#include "OxCamlFormatHelpers.h"
#include "OxCamlHelpers.h"
#include "OxCamlValueFormatters.h"
#include "Plugins/TypeSystem/OxCaml/TypeSystemOxCaml.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Target/Process.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Reference.h"
#include "lldb/Utility/Status.h"
#include "lldb/ValueObject/ValueObject.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"
#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#define ENSURE(CONDITION, STREAM, MARKER_EXPR, ERROR_FMT, ...)                 \
  OXCAML_CONDITIONALLY_EMIT_MARKER_AND_RETURN(                                 \
      STREAM, CONDITION, true, MARKER_EXPR, ERROR_FMT, __VA_ARGS__)

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters;
using namespace lldb_private::formatters::oxcaml;
using namespace lldb_private::formatters::oxcaml::helpers;

static uint64_t MakeLowBitMask(uint64_t bit_size) {
  if (bit_size >= 64)
    return std::numeric_limits<uint64_t>::max();
  return (1ULL << bit_size) - 1ULL;
}

static bool FormatValue(Stream &stream, OxCamlType *type, DataExtractor &data,
                        const OxCamlFormatContext &context,
                        std::optional<lldb::addr_t> object_address);
static bool FormatUnboxedBase(Stream &stream,
                              OxCamlUnboxedBaseType *unboxed_type,
                              DataExtractor &data);
static bool FormatFallback(Stream &stream, OxCamlType *type,
                           DataExtractor &data);
static bool FormatEnum(Stream &stream, OxCamlEnumType *enum_type,
                       DataExtractor &data);
static bool FormatPointer(Stream &stream, OxCamlPointerType *ptr_type,
                          DataExtractor &data,
                          const OxCamlFormatContext &context);
static bool FormatTypedef(Stream &stream, OxCamlTypedefType *typedef_type,
                          DataExtractor &data,
                          const OxCamlFormatContext &context,
                          std::optional<lldb::addr_t> object_address);
static bool FormatStructure(Stream &stream, OxCamlStructureType *struct_type,
                            DataExtractor &data,
                            const OxCamlFormatContext &context,
                            std::optional<lldb::addr_t> object_address);
static bool FormatPlaceholder(Stream &stream,
                              OxCamlPlaceholderType *placeholder_type,
                              DataExtractor &data);
static bool FormatUnknown(Stream &stream, OxCamlUnknownType *unknown_type,
                          DataExtractor &data);
static bool FormatArray(Stream &stream, OxCamlArrayType *array_type,
                        DataExtractor &data, const OxCamlFormatContext &context,
                        std::optional<lldb::addr_t> object_address);
static bool TryFormatRuntimeFloatArray(Stream &stream, lldb::addr_t array_ptr,
                                       lldb::ProcessSP process_sp);

static bool FormatVariantPart(Stream &stream,
                              const OxCamlVariantPart &variant_part,
                              DataExtractor &data,
                              const OxCamlFormatContext &context,
                              std::optional<lldb::addr_t> object_address,
                              bool is_ocaml_variant);

// ============================================================================
// Entry Point: OxCamlValue_SummaryProvider
// ============================================================================
//
// This is the main entry point registered with LLDB for formatting OCaml
// values. It extracts the data from the ValueObject, retrieves the OxCamlType
// information, and dispatches to FormatValue() which handles the type-specific
// formatting.
//
// ============================================================================

bool lldb_private::formatters::oxcaml::OxCamlValue_SummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &options) {
  DataExtractor data;
  Status error;
  valobj.GetData(data, error);

  ENSURE(error.Success(), stream, "<unavailable>",
         "ValueObject data extraction failed: {0}", error.AsCString());

  CompilerType compiler_type = valobj.GetCompilerType();
  void *opaque_type = compiler_type.GetOpaqueQualType();

  ENSURE(compiler_type.IsValid(), stream, "<unavailable>",
         "OxCamlValue_SummaryProvider called with invalid CompilerType "
         "(opaque={0:P})",
         opaque_type);

  auto *type_ref = static_cast<Reference<OxCamlType> *>(opaque_type);
  ENSURE(type_ref != nullptr, stream, "<unavailable>",
         "OxCamlValue_SummaryProvider called with null type reference "
         "(opaque={0:P})",
         opaque_type);

  OxCamlType *type = type_ref->get();

  lldb::ProcessSP process_sp = valobj.GetProcessSP();
  ENSURE(process_sp, stream, "<unavailable>",
         "OxCamlValue_SummaryProvider called without process context "
         "(ptr={0:P})",
         process_sp.get());

  OxCamlFormatContext context{process_sp, valobj.GetExecutionContextRef(),
                              data.GetByteOrder(), data.GetAddressByteSize()};

  // No object address at entry: [data] holds the OCaml value itself
  // (typically a register-resident tagged pointer). FormatPointer sets the
  // object address for any pointed-to objects it dereferences.
  return FormatValue(stream, type, data, context,
                     /*object_address=*/std::nullopt);
}

// ============================================================================
// Type-Specific Formatters
// ============================================================================

static bool FormatFallback(Stream &stream, OxCamlType *type,
                           DataExtractor &data) {
  size_t byte_size = data.GetByteSize();
  if (byte_size == 0) {
    if (type) {
      OXCAML_EMIT_MARKER(stream, "<empty>",
                         "Zero-byte value encountered while formatting type "
                         "'{0}' (DIE 0x{1:x})",
                         type->GetDisplayName(), type->GetDieId());
    } else {
      OXCAML_EMIT_MARKER(stream, "<empty>", "{0}",
                         "Zero-byte value encountered while formatting value "
                         "with no type information");
    }
    return true;
  }

  stream.PutCString("data(");
  for (size_t i = 0; i < byte_size; ++i) {
    if (i > 0)
      stream.PutCString(" ");
    lldb::offset_t offset = i;
    uint8_t byte = data.GetU8(&offset);
    OX_ASSERT(offset == i + 1,
              "GetU8 failed to advance offset in FormatFallback "
              "(expected={0}, actual={1})",
              i + 1, offset);
    stream.Printf("%02x", byte);
  }
  stream.PutCString(")");
  return true;
}

static bool FormatPlaceholder(Stream &stream,
                              OxCamlPlaceholderType *placeholder_type,
                              DataExtractor &data) {
  OXCAML_EMIT_MARKER(
      stream, "<placeholder>",
      "Cannot format unresolved placeholder type '{0}' (DIE 0x{1:x16})",
      placeholder_type->GetDisplayName(), placeholder_type->GetDieId());
  return true;
}

static bool FormatUnknown(Stream &stream, OxCamlUnknownType *unknown_type,
                          DataExtractor &data) {
  OXCAML_EMIT_MARKER(
      stream, "<unknown>",
      "Unsupported DWARF tag 0x{0:x} for type '{1}' (DIE 0x{2:x16})",
      unknown_type->GetDwarfTag(), unknown_type->GetDisplayName(),
      unknown_type->GetDieId());
  return true;
}

static bool FormatUnboxedBase(Stream &stream,
                              OxCamlUnboxedBaseType *unboxed_type,
                              DataExtractor &data) {
  uint64_t byte_size = unboxed_type->GetByteSize();
  ENSURE(byte_size != 0, stream, "<0-byte base type>",
         "Unboxed base type '{0}' (DIE 0x{1:x}) reports zero byte size",
         unboxed_type->GetDisplayName(), unboxed_type->GetDieId());

  OxCamlUnboxedBaseType::BaseKind kind = unboxed_type->GetBaseKind();
  lldb::offset_t offset = 0;
  switch (kind) {
  case OxCamlUnboxedBaseType::Signed: {
    auto apint = helpers::ExtractAPInt(data, &offset, byte_size);
    ENSURE(apint.has_value(), stream, "<signed>",
           "Signed base type '{0}' ({1}B) unreadable",
           unboxed_type->GetDisplayName(), byte_size);

    std::string suffix = suffixes::GetSignedIntegerSuffix(byte_size);
    helpers::FormatAPInt(&stream, *apint, true, "#", suffix);
    return true;
  }
  case OxCamlUnboxedBaseType::Unsigned: {
    auto apint = helpers::ExtractAPInt(data, &offset, byte_size);
    ENSURE(apint.has_value(), stream, "<unsigned>",
           "Unsigned base type '{0}' ({1}B) unreadable",
           unboxed_type->GetDisplayName(), byte_size);

    std::string suffix = suffixes::GetUnsignedIntegerSuffix(byte_size);
    helpers::FormatAPInt(&stream, *apint, false, "#", suffix);
    return true;
  }
  case OxCamlUnboxedBaseType::Float: {
    auto float_size = helpers::ByteSizeToFloatSize(byte_size);
    ENSURE(float_size.has_value(), stream,
           llvm::formatv("<{0}-byte float>", byte_size).str(),
           "Float value width {0} bytes for type '{1}' (DIE 0x{2:x}) is "
           "unsupported",
           byte_size, unboxed_type->GetDisplayName(), unboxed_type->GetDieId());

    auto apfloat = helpers::ExtractAPFloat(data, &offset, *float_size);
    ENSURE(apfloat.has_value(), stream, "<float>",
           "Float base type '{0}' ({1}B) unreadable",
           unboxed_type->GetDisplayName(), byte_size);

    std::string suffix;
    switch (*float_size) {
    case helpers::FloatSize::Half:
      suffix = suffixes::FLOAT16_SUFFIX;
      break;
    case helpers::FloatSize::Single:
      suffix = suffixes::FLOAT32_SUFFIX;
      break;
    case helpers::FloatSize::Double:
      suffix = suffixes::FLOAT64_SUFFIX;
      break;
    }
    helpers::FormatAPFloat(&stream, *apfloat, std::nullopt, "#", suffix);
    return true;
  }
  }
  llvm_unreachable("unknown OxCamlUnboxedBaseType::BaseKind");
}

static bool FormatEnum(Stream &stream, OxCamlEnumType *enum_type,
                       DataExtractor &data) {
  uint64_t byte_size = enum_type->GetByteSize();
  OX_ASSERT(byte_size > 0 && byte_size <= helpers::constants::WORD_SIZE,
            "OCaml enum type '{0}' (DIE 0x{1:x}) has size {2} bytes, must be "
            "1-{3} bytes",
            enum_type->GetDisplayName(), enum_type->GetDieId(), byte_size,
            helpers::constants::WORD_SIZE);

  lldb::offset_t offset = 0;
  uint64_t value = data.GetMaxU64(&offset, static_cast<uint32_t>(byte_size));

  ENSURE(offset == byte_size, stream, "<enum>",
         "Failed to read {0} bytes for enum type '{1}' (DIE 0x{2:x}); "
         "read {3} bytes",
         byte_size, enum_type->GetDisplayName(), enum_type->GetDieId(), offset);

  auto name_opt = enum_type->GetEnumeratorName(value);
  ENSURE(name_opt.has_value(), stream, "<enum>",
         "Enumerator not found for value 0x{0:x} in enum type '{1}' "
         "(DIE 0x{2:x})",
         value, enum_type->GetDisplayName(), enum_type->GetDieId());

  stream.PutCString(name_opt.value());
  return true;
}

// Resolve the byte size of [type] at format time. For structures, evaluates
// the dynamic DW_AT_byte_size/DW_AT_bit_size against [object_address] (which
// must be the type's own object address). For arrays, compiler-emitted OxCaml
// DWARF wraps the DW_TAG_array_type in a reference type and equips the array
// with DW_AT_count and DW_AT_byte_stride. The count expression reads the OCaml
// block header wosize; generic runtime array discovery belongs to the
// ocaml_value formatter, not this typed-array path. For everything else,
// returns the static byte size.
static llvm::Expected<uint64_t>
ResolveTypeByteSize(OxCamlType *type,
                    std::optional<lldb::addr_t> object_address,
                    const OxCamlFormatContext &context) {
  OxCamlType *underlying = type;
  while (underlying && underlying->GetKind() == OxCamlType::Typedef)
    underlying =
        static_cast<OxCamlTypedefType *>(underlying)->GetUnderlyingType();
  if (!underlying)
    return type->GetByteSize();

  if (underlying->GetKind() == OxCamlType::Structure) {
    auto *struct_type = static_cast<OxCamlStructureType *>(underlying);
    auto result =
        struct_type->GetDynamicSize().Evaluate(context, object_address);
    if (!result)
      return result.takeError();
    OX_ASSERT(
        result->GetKind() == OxCamlLayoutValueKind::ScalarBytes ||
            result->GetKind() == OxCamlLayoutValueKind::ScalarBits,
        "Dynamic size for structure type '{0}' (DIE 0x{1:x}) evaluated to "
        "layout kind {2}; expected ScalarBytes or ScalarBits",
        struct_type->GetDisplayName(), struct_type->GetDieId(),
        static_cast<int>(result->GetKind()));
    if (result->GetKind() == OxCamlLayoutValueKind::ScalarBits)
      return (result->GetScalar() + 7) / 8;
    return result->GetScalar();
  }

  if (underlying->GetKind() == OxCamlType::Array) {
    auto *array_type = static_cast<OxCamlArrayType *>(underlying);
    auto stride_result =
        array_type->GetStride().Evaluate(context, object_address);
    if (!stride_result)
      return stride_result.takeError();
    OX_ASSERT(stride_result->GetKind() == OxCamlLayoutValueKind::ScalarBytes,
              "DW_AT_byte_stride for array '{0}' (DIE 0x{1:x}) evaluated to "
              "layout kind {2}; expected ScalarBytes",
              array_type->GetDisplayName(), array_type->GetDieId(),
              static_cast<int>(stride_result->GetKind()));
    uint64_t stride = stride_result->GetScalar();

    const auto &count = array_type->GetCount();
    if (!count.has_value())
      return llvm::createStringError(
          "Array type '%s' (DIE 0x%" PRIx64
          ") has no DW_AT_count; compiler-emitted OxCaml arrays must include "
          "a subrange count expression",
          array_type->GetDisplayName().c_str(), array_type->GetDieId());
    auto count_result = count->Evaluate(context, object_address);
    if (!count_result)
      return count_result.takeError();
    OX_ASSERT(count_result->GetKind() == OxCamlLayoutValueKind::ScalarElements,
              "DW_AT_count for array '{0}' (DIE 0x{1:x}) evaluated to layout "
              "kind {2}; expected ScalarElements",
              array_type->GetDisplayName(), array_type->GetDieId(),
              static_cast<int>(count_result->GetKind()));
    return count_result->GetScalar() * stride;
  }

  return type->GetByteSize();
}

static bool FormatPointer(Stream &stream, OxCamlPointerType *ptr_type,
                          DataExtractor &data,
                          const OxCamlFormatContext &context) {
  uint64_t ptr_size = ptr_type->GetByteSize();
  ENSURE(ptr_size == helpers::constants::WORD_SIZE, stream, "<pointer>",
         "OCaml pointer type '{0}' (DIE 0x{1:x}) has size {2} bytes, "
         "expected {3} bytes",
         ptr_type->GetDisplayName(), ptr_type->GetDieId(), ptr_size,
         helpers::constants::WORD_SIZE);

  lldb::offset_t offset = 0;
  uint64_t ptr_value = data.GetU64(&offset);

  ENSURE(offset == helpers::constants::WORD_SIZE, stream, "<pointer>",
         "Failed to read pointer value for type '{0}' (DIE 0x{1:x}); "
         "read {2} bytes, expected {3}",
         ptr_type->GetDisplayName(), ptr_type->GetDieId(), offset,
         helpers::constants::WORD_SIZE);

  ENSURE(!helpers::value::IsImmediate(ptr_value), stream, "<pointer>",
         "Pointer value 0x{0:x} is an OCaml immediate; cannot dereference as "
         "'{1}' (DIE 0x{2:x})",
         ptr_value, ptr_type->GetDisplayName(), ptr_type->GetDieId());

  OxCamlType *pointed_to = ptr_type->GetPointedToType();
  ENSURE(pointed_to, stream, "<pointer>",
         "Pointer 0x{0:x} to '{1}' (DIE 0x{2:x}) lacks a resolved target type",
         ptr_value, ptr_type->GetDisplayName(), ptr_type->GetDieId());

  lldb::addr_t object_address = ptr_value;

  llvm::Expected<uint64_t> actual_size_expected =
      ResolveTypeByteSize(pointed_to, object_address, context);
  if (!actual_size_expected) {
    OXCAML_EMIT_MARKER(
        stream, "<pointer>",
        "Failed to evaluate dynamic byte size for pointer 0x{0:x} to "
        "'{1}' (DIE 0x{2:x}): {3}",
        ptr_value, pointed_to->GetDisplayName(), pointed_to->GetDieId(),
        llvm::toString(actual_size_expected.takeError()));
    return true;
  }
  uint64_t actual_size = *actual_size_expected;
  OxCamlType *underlying_pointed_to = pointed_to;
  while (underlying_pointed_to &&
         underlying_pointed_to->GetKind() == OxCamlType::Typedef)
    underlying_pointed_to =
        static_cast<OxCamlTypedefType *>(underlying_pointed_to)
            ->GetUnderlyingType();
  bool is_array_target = underlying_pointed_to &&
                         underlying_pointed_to->GetKind() == OxCamlType::Array;

  // Empty OCaml arrays are real heap blocks with zero-byte payloads. Keep the
  // zero-size guard for other pointer targets, but let arrays flow through to
  // FormatArray with an empty DataExtractor and the object address.
  ENSURE(
      actual_size != 0 || is_array_target, stream, "<pointer>",
      "Resolved target type '{0}' (DIE 0x{1:x}) computed dereference size 0; "
      "cannot dereference pointer 0x{2:x}",
      pointed_to->GetDisplayName(), pointed_to->GetDieId(), ptr_value);

  std::vector<uint8_t> buffer(actual_size);
  DataExtractor pointed_data;
  pointed_data.SetByteOrder(data.GetByteOrder());
  pointed_data.SetAddressByteSize(data.GetAddressByteSize());
  if (actual_size > 0) {
    Status error;
    size_t bytes_read = context.process_sp->ReadMemory(
        object_address, buffer.data(), actual_size, error);

    ENSURE(bytes_read == actual_size && error.Success(), stream, "<pointer>",
           "Failed to read {0} bytes from address 0x{1:x} for pointer 0x{2:x} "
           "to '{3}' (DIE 0x{4:x})",
           actual_size, object_address, ptr_value, pointed_to->GetDisplayName(),
           pointed_to->GetDieId());

    pointed_data.SetData(buffer.data(), actual_size, data.GetByteOrder());
  }

  return FormatValue(stream, pointed_to, pointed_data, context, object_address);
}

static bool FormatTypedef(Stream &stream, OxCamlTypedefType *typedef_type,
                          DataExtractor &data,
                          const OxCamlFormatContext &context,
                          std::optional<lldb::addr_t> object_address) {
  return FormatValue(stream, typedef_type->GetUnderlyingType(), data, context,
                     object_address);
}

// Format an OCaml exception value
//
// OCaml Exception Representation:
// ================================
// Every exception constructor gets a unique value, which is a block with
// Object_tag (0xf8). This Object_tag block stores the exception's constructor
// name as a string in its first field.
//
// Exception Value Encoding (two cases):
//
// 1. Exception WITHOUT arguments (e.g., "exception Empty"):
//    The exception value is a pointer to the Object_tag block:
//      value -> [Object_tag block]
//                 field 0: pointer to string "Empty"
//
// 2. Exception WITH arguments (e.g., "exception Found of int"):
//    The exception value is a pointer to a separate block (tag 0x00) that
//    stores:
//      value -> [Exception block, tag 0x00]
//                 field 0: pointer to Object_tag block (the exception value)
//                 field 1+: exception arguments
//
//    Where the Object_tag block contains:
//      [Object_tag block, tag 0xf8]
//        field 0: pointer to string "Found"

// Information extracted from an OCaml exception value
struct ExceptionInfo {
  std::string constructor_name;    // Exception constructor name
  std::vector<uint64_t> arguments; // Exception arguments, possibly empty
};

static std::optional<ExceptionInfo>
ExtractExceptionInfo(lldb::addr_t exception_addr, lldb::ProcessSP process_sp) {
  Status error;
  Log *log = GetLog(OxCamlLog::Formatting);

  auto header_opt = helpers::ReadBlockHeader(exception_addr, process_sp);
  if (!header_opt.has_value()) {
    LLDB_LOG(log, "Failed to read exception block header for pointer 0x{0:x}",
             exception_addr);
    return std::nullopt;
  }

  uint64_t header = *header_opt;
  uint8_t tag = header::ExtractTag(header);
  uint64_t wosize = header::ExtractWosize(header);

  std::vector<uint64_t> arguments;
  lldb::addr_t string_addr;

  switch (tag) {
  case static_cast<uint8_t>(constants::SpecialTag::Object_tag): {
    uint64_t string_ptr = process_sp->ReadUnsignedIntegerFromMemory(
        exception_addr, constants::WORD_SIZE, 0, error);
    if (error.Fail() || helpers::value::IsImmediate(string_ptr) ||
        string_ptr == 0) {
      LLDB_LOG(log,
               "Failed to read valid string pointer from Object_tag block at "
               "0x{0:x}",
               exception_addr);
      return std::nullopt;
    }
    string_addr = string_ptr;
    break;
  }

  case constants::EXCEPTION_BLOCK_TAG: {
    uint64_t obj_tag_ptr = process_sp->ReadUnsignedIntegerFromMemory(
        exception_addr, constants::WORD_SIZE, 0, error);
    if (error.Fail() || helpers::value::IsImmediate(obj_tag_ptr) ||
        obj_tag_ptr == 0) {
      LLDB_LOG(log,
               "Failed to read valid Object_tag pointer from exception block "
               "at 0x{0:x}",
               exception_addr);
      return std::nullopt;
    }

    uint64_t string_ptr = process_sp->ReadUnsignedIntegerFromMemory(
        obj_tag_ptr, constants::WORD_SIZE, 0, error);
    if (error.Fail() || helpers::value::IsImmediate(string_ptr) ||
        string_ptr == 0) {
      LLDB_LOG(log,
               "Failed to read valid string pointer from Object_tag at 0x{0:x}",
               obj_tag_ptr);
      return std::nullopt;
    }
    string_addr = string_ptr;

    for (uint64_t i = 1; i < wosize; i++) {
      uint64_t arg_value = process_sp->ReadUnsignedIntegerFromMemory(
          exception_addr + i * constants::WORD_SIZE, constants::WORD_SIZE, 0,
          error);
      if (error.Fail()) {
        LLDB_LOG(log,
                 "Failed to read exception arguments from block at 0x{0:x}",
                 exception_addr);
        return std::nullopt;
      }
      arguments.push_back(arg_value);
    }
    break;
  }

  default:
    LLDB_LOG(log, "Unknown exception block tag {0} at 0x{1:x}", tag,
             exception_addr);
    return std::nullopt;
  }

  uint64_t string_header = process_sp->ReadUnsignedIntegerFromMemory(
      string_addr - constants::WORD_SIZE, constants::WORD_SIZE, 0, error);
  if (error.Fail() ||
      header::ExtractTag(string_header) !=
          static_cast<uint8_t>(constants::SpecialTag::String_tag)) {
    LLDB_LOG(log, "Failed to read valid string header at 0x{0:x}", string_addr);
    return std::nullopt;
  }

  uint64_t wosize_string = header::ExtractWosize(string_header);

  auto string_opt =
      helpers::ReadOCamlStringData(string_addr, wosize_string, process_sp);
  if (!string_opt) {
    LLDB_LOG(log, "Failed to read string data at 0x{0:x}", string_addr);
    return std::nullopt;
  }

  ExceptionInfo info;
  info.constructor_name = std::move(*string_opt);
  info.arguments = std::move(arguments);

  return info;
}

// Format an OCaml exception value
static bool FormatException(Stream &stream, DataExtractor &data,
                            const OxCamlFormatContext &context) {
  lldb::offset_t offset = 0;
  uint64_t exception_value = data.GetU64(&offset);

  ENSURE(offset == helpers::constants::WORD_SIZE, stream, "<exception>",
         "Failed to read exception value; read {0} bytes, expected {1}", offset,
         helpers::constants::WORD_SIZE);

  ENSURE(exception_value != 0 && helpers::value::IsPointer(exception_value),
         stream, "<exception>",
         "Exception value 0x{0:x} is a {1}; cannot display OCaml exception",
         exception_value,
         (exception_value == 0) ? "null pointer" : "immediate OCaml value");

  auto info_opt = ExtractExceptionInfo(exception_value, context.process_sp);

  ENSURE(info_opt.has_value(), stream, "<exception>",
         "Failed to extract OCaml exception payload at 0x{0:x}",
         exception_value);

  stream.Printf("%s", info_opt->constructor_name.c_str());

  if (!info_opt->arguments.empty()) {
    bool use_parens = (info_opt->arguments.size() > 1);
    stream.Printf(use_parens ? " (" : " ");

    for (size_t i = 0; i < info_opt->arguments.size(); i++) {
      if (i > 0) {
        stream.Printf(", ");
      }

      DataExtractor arg_data(&info_opt->arguments[i], constants::WORD_SIZE,
                             context.byte_order, constants::WORD_SIZE);
      oxcaml::FormatOxCamlValue(stream, arg_data, context.process_sp,
                                context.exe_ctx_ref);
    }

    if (use_parens) {
      stream.Printf(")");
    }
  }

  return true;
}

// Polymorphic array values such as ['a array] may have an ocaml_value element
// type in DWARF but actually carry unboxed doubles at runtime. The OCaml block
// tag (Double_array_tag) is authoritative, so check it here before formatting
// elements from DWARF. Returns true (with output emitted) if the runtime tag
// proves this is a float array; returns false without emitting anything
// otherwise.
static bool TryFormatRuntimeFloatArray(Stream &stream, lldb::addr_t array_ptr,
                                       lldb::ProcessSP process_sp) {
  auto header_opt = helpers::ReadBlockHeader(array_ptr, process_sp);
  if (!header_opt.has_value())
    return false;

  uint8_t tag;
  uint64_t wosize;
  uint8_t reserved;
  helpers::header::ParseHeader(*header_opt, tag, wosize, reserved);
  if (tag != helpers::constants::DOUBLE_ARRAY_TAG)
    return false;

  return oxcaml::FormatOxCamlDoubleArray(stream, array_ptr, wosize, process_sp);
}

static bool FormatArray(Stream &stream, OxCamlArrayType *array_type,
                        DataExtractor &data, const OxCamlFormatContext &context,
                        std::optional<lldb::addr_t> object_address) {
  if (object_address.has_value() &&
      TryFormatRuntimeFloatArray(stream, *object_address, context.process_sp))
    return true;

  llvm::Expected<OxCamlLayoutResult> stride_result =
      array_type->GetStride().Evaluate(context, object_address);
  if (!stride_result) {
    OXCAML_EMIT_MARKER(
        stream, "<array>",
        "Failed to evaluate byte_stride for array '{0}' (DIE 0x{1:x}): {2}",
        array_type->GetDisplayName(), array_type->GetDieId(),
        llvm::toString(stride_result.takeError()));
    return true;
  }
  uint64_t stride = stride_result->GetScalar();
  ENSURE(stride != 0, stream, "<array>",
         "Array type '{0}' (DIE 0x{1:x}) has stride 0; cannot format elements",
         array_type->GetDisplayName(), array_type->GetDieId());

  // OxCaml compiler-emitted array DIEs always carry DW_AT_count on their
  // subrange. Values without typed-array DWARF are handled by generic
  // ocaml_value formatting instead.
  ENSURE(array_type->GetCount().has_value(), stream, "<array>",
         "Array type '{0}' (DIE 0x{1:x}) has no DW_AT_count; "
         "compiler-emitted OxCaml arrays must include a subrange count "
         "expression",
         array_type->GetDisplayName(), array_type->GetDieId());
  llvm::Expected<OxCamlLayoutResult> count_result =
      array_type->GetCount()->Evaluate(context, object_address);
  if (!count_result) {
    OXCAML_EMIT_MARKER(
        stream, "<array>",
        "Failed to evaluate DW_AT_count for array '{0}' (DIE 0x{1:x}): {2}",
        array_type->GetDisplayName(), array_type->GetDieId(),
        llvm::toString(count_result.takeError()));
    return true;
  }
  uint64_t num_elements = count_result->GetScalar();

  uint64_t payload_size = num_elements * stride;
  ENSURE(data.GetByteSize() >= payload_size, stream, "<array>",
         "Array '{0}' (DIE 0x{1:x}) payload buffer holds {2} bytes, needs "
         "{3} ({4} elements x {5}-byte stride)",
         array_type->GetDisplayName(), array_type->GetDieId(),
         data.GetByteSize(), payload_size, num_elements, stride);

  OxCamlType *element_type = array_type->GetElementType();
  stream.Printf("[| ");
  for (uint64_t i = 0; i < num_elements; i++) {
    if (i > 0)
      stream.Printf("; ");

    DataExtractor element_data(data, i * stride, stride);
    std::optional<lldb::addr_t> element_address;
    if (object_address.has_value())
      element_address = *object_address + i * stride;
    FormatValue(stream, element_type, element_data, context, element_address);
  }
  stream.Printf(" |]");
  return true;
}

// A member's location resolved against the parent's object address and the
// parent's in-buffer data: either an offset within [data] (for constant
// object-relative locations) or a process load address (for exprloc results).
// The member object address is the OCaml object address of the member itself,
// suitable for evaluating expressions on the member's type.
struct ResolvedMemberLocation {
  enum class Source { BufferOffset, LoadAddress };
  Source source;
  uint64_t buffer_offset = 0;
  lldb::addr_t load_address = LLDB_INVALID_ADDRESS;
  std::optional<lldb::addr_t> member_object_address;
};

static std::optional<ResolvedMemberLocation>
ResolveMemberLocation(Stream &stream, const OxCamlMember &member,
                      DataExtractor &data, const OxCamlFormatContext &context,
                      std::optional<lldb::addr_t> parent_object_address) {
  (void)data;
  llvm::Expected<OxCamlLayoutResult> result =
      member.location.Evaluate(context, parent_object_address);
  if (!result) {
    OXCAML_EMIT_MARKER(
        stream, "<member>", "Failed to evaluate location for member '{0}': {1}",
        member.name.value_or("<unnamed>"), llvm::toString(result.takeError()));
    return std::nullopt;
  }

  ResolvedMemberLocation resolved;
  if (result->GetLocationStorage() ==
      OxCamlLayoutResult::LocationStorage::ObjectRelativeByteOffset) {
    resolved.source = ResolvedMemberLocation::Source::BufferOffset;
    resolved.buffer_offset = result->GetObjectRelativeByteOffset();
    if (parent_object_address.has_value())
      resolved.member_object_address =
          *parent_object_address + resolved.buffer_offset;
  } else {
    resolved.source = ResolvedMemberLocation::Source::LoadAddress;
    resolved.load_address = result->GetLoadAddress();
    resolved.member_object_address = resolved.load_address;
  }
  return resolved;
}

// Read [byte_size] bytes for a member or discriminator into [out_buffer]
// according to a resolved location.
static bool
ReadResolvedMemberBytes(Stream &stream, const OxCamlMember &member,
                        uint64_t byte_size,
                        const ResolvedMemberLocation &resolved,
                        DataExtractor &data, const OxCamlFormatContext &context,
                        llvm::SmallVectorImpl<uint8_t> &out_buffer) {
  out_buffer.resize(byte_size);
  if (resolved.source == ResolvedMemberLocation::Source::BufferOffset) {
    uint64_t offset = resolved.buffer_offset;
    if (offset + byte_size > data.GetByteSize()) {
      OXCAML_EMIT_MARKER(
          stream, "<member>",
          "Member '{0}' at object-relative offset {1} (+{2} bytes) extends "
          "past available data ({3} bytes)",
          member.name.value_or("<unnamed>"), offset, byte_size,
          data.GetByteSize());
      return false;
    }
    if (data.CopyData(offset, byte_size, out_buffer.data()) != byte_size) {
      OXCAML_EMIT_MARKER(
          stream, "<member>",
          "Member '{0}': failed to copy {1} bytes from offset {2}",
          member.name.value_or("<unnamed>"), byte_size, offset);
      return false;
    }
    return true;
  }

  Status error;
  size_t bytes_read = context.process_sp->ReadMemory(
      resolved.load_address, out_buffer.data(), byte_size, error);
  if (bytes_read != byte_size || error.Fail()) {
    OXCAML_EMIT_MARKER(
        stream, "<member>",
        "Member '{0}': failed to read {1} bytes from 0x{2:x}: {3}",
        member.name.value_or("<unnamed>"), byte_size, resolved.load_address,
        error.AsCString());
    return false;
  }
  return true;
}

static bool FormatMember(Stream &stream, const OxCamlMember &member,
                         DataExtractor &data,
                         const OxCamlFormatContext &context,
                         std::optional<lldb::addr_t> object_address) {
  auto resolved =
      ResolveMemberLocation(stream, member, data, context, object_address);
  if (!resolved.has_value())
    return true;

  if (member.IsBitField()) {
    llvm::Expected<OxCamlLayoutResult> bit_offset_result =
        member.bit_offset->Evaluate(context, object_address);
    if (!bit_offset_result) {
      OXCAML_EMIT_MARKER(stream, "<bit-field>",
                         "Failed to evaluate bit offset for member '{0}': {1}",
                         member.name.value_or("<unnamed>"),
                         llvm::toString(bit_offset_result.takeError()));
      return true;
    }
    llvm::Expected<OxCamlLayoutResult> bit_size_result =
        member.bit_size->Evaluate(context, object_address);
    if (!bit_size_result) {
      OXCAML_EMIT_MARKER(stream, "<bit-field>",
                         "Failed to evaluate bit size for member '{0}': {1}",
                         member.name.value_or("<unnamed>"),
                         llvm::toString(bit_size_result.takeError()));
      return true;
    }
    uint64_t bit_offset = bit_offset_result->GetScalar();
    uint64_t bit_size = bit_size_result->GetScalar();

    ENSURE(bit_offset + bit_size <= 64, stream, "<bit-field>",
           "Bit-field '{0}' spans multiple words (offset={1}, size={2}); "
           "multi-word bit-fields not yet supported",
           member.name.value_or("<unnamed>"), bit_offset, bit_size);

    llvm::SmallVector<uint8_t, 8> word;
    if (!ReadResolvedMemberBytes(stream, member, sizeof(uint64_t), *resolved,
                                 data, context, word))
      return true;

    DataExtractor word_data(word.data(), word.size(), data.GetByteOrder(),
                            data.GetAddressByteSize());
    lldb::offset_t off = 0;
    uint64_t full_value = word_data.GetU64(&off);
    uint64_t bit_mask = MakeLowBitMask(bit_size);
    uint64_t extracted = (full_value >> bit_offset) & bit_mask;

    // The extracted bit-field value is a fresh scalar with no header; nested
    // expressions cannot reference an object address through it.
    DataExtractor bit_data(&extracted, sizeof(extracted), data.GetByteOrder(),
                           data.GetAddressByteSize());
    return FormatValue(stream, member.GetType(), bit_data, context,
                       /*object_address=*/std::nullopt);
  }

  // Resolve the member type's dynamic byte size using the member's own
  // object address, so nested structures with exprloc-valued sizes read the
  // right amount of memory.
  llvm::Expected<uint64_t> member_byte_size_expected = ResolveTypeByteSize(
      member.GetType(), resolved->member_object_address, context);
  if (!member_byte_size_expected) {
    OXCAML_EMIT_MARKER(
        stream, "<member>",
        "Failed to evaluate byte size for member '{0}' (type '{1}'): {2}",
        member.name.value_or("<unnamed>"), member.GetType()->GetDisplayName(),
        llvm::toString(member_byte_size_expected.takeError()));
    return true;
  }
  uint64_t member_byte_size = *member_byte_size_expected;

  llvm::SmallVector<uint8_t, 32> buffer;
  if (!ReadResolvedMemberBytes(stream, member, member_byte_size, *resolved,
                               data, context, buffer))
    return true;

  DataExtractor member_data(buffer.data(), member_byte_size,
                            data.GetByteOrder(), data.GetAddressByteSize());
  return FormatValue(stream, member.GetType(), member_data, context,
                     resolved->member_object_address);
}

// Returns true if both members have constant data_member_location 0.
static bool AreBothMembersAtConstantOffsetZero(const OxCamlMember &a,
                                               const OxCamlMember &b) {
  return a.location.IsConstant() && a.location.GetConstant() == 0 &&
         b.location.IsConstant() && b.location.GetConstant() == 0;
}

static bool FormatStructure(Stream &stream, OxCamlStructureType *struct_type,
                            DataExtractor &data,
                            const OxCamlFormatContext &context,
                            std::optional<lldb::addr_t> object_address) {
  const auto &members = struct_type->GetMembers();
  const auto &variant_parts = struct_type->GetVariantParts();

  // OCaml variant: a structure with no direct members and exactly one
  // variant part is rendered as the variant content alone (no braces).
  if (members.empty() && variant_parts.size() == 1) {
    return FormatVariantPart(stream, variant_parts[0], data, context,
                             object_address, true);
  }

  // OCaml exception: a union-style struct with "exn" and "raw" members both
  // at offset 0. Format only via FormatException.
  if (members.size() == 2 && variant_parts.empty() &&
      members[0].name.has_value() && members[1].name.has_value() &&
      AreBothMembersAtConstantOffsetZero(members[0], members[1])) {
    const std::string &name0 = members[0].name.value();
    const std::string &name1 = members[1].name.value();
    if ((name0 == "exn" && name1 == "raw") ||
        (name0 == "raw" && name1 == "exn")) {
      return FormatException(stream, data, context);
    }
  }

  bool is_tuple = struct_type->IsTuple();
  const char *open_delim = is_tuple ? "(" : "{ ";
  const char *close_delim = is_tuple ? ")" : " }";
  const char *separator = is_tuple ? ", " : "; ";

  stream.Printf("%s", open_delim);

  bool has_content = false;

  for (size_t i = 0; i < members.size(); ++i) {
    if (has_content)
      stream.Printf("%s", separator);

    if (members[i].name.has_value()) {
      stream.Printf("%s = ", members[i].name.value().c_str());
    }

    FormatMember(stream, members[i], data, context, object_address);
    has_content = true;
  }

  for (size_t i = 0; i < variant_parts.size(); ++i) {
    if (has_content)
      stream.Printf("%s", separator);

    FormatVariantPart(stream, variant_parts[i], data, context, object_address,
                      false);
    has_content = true;
  }

  stream.Printf("%s", close_delim);

  return true;
}

static bool FormatValue(Stream &stream, OxCamlType *type, DataExtractor &data,
                        const OxCamlFormatContext &context,
                        std::optional<lldb::addr_t> object_address) {
  if (!type) {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(
        log,
        "FormatValue: No type information available, using fallback formatter");
    return FormatFallback(stream, type, data);
  }

  switch (type->GetKind()) {
  case OxCamlType::Value:
    return oxcaml::FormatOxCamlValue(stream, data, context.process_sp,
                                     context.exe_ctx_ref);
  case OxCamlType::UnboxedBase:
    return FormatUnboxedBase(stream, static_cast<OxCamlUnboxedBaseType *>(type),
                             data);
  case OxCamlType::Enum:
    return FormatEnum(stream, static_cast<OxCamlEnumType *>(type), data);
  case OxCamlType::Pointer:
    return FormatPointer(stream, static_cast<OxCamlPointerType *>(type), data,
                         context);
  case OxCamlType::Typedef:
    return FormatTypedef(stream, static_cast<OxCamlTypedefType *>(type), data,
                         context, object_address);
  case OxCamlType::Structure:
    return FormatStructure(stream, static_cast<OxCamlStructureType *>(type),
                           data, context, object_address);
  case OxCamlType::Array:
    return FormatArray(stream, static_cast<OxCamlArrayType *>(type), data,
                       context, object_address);
  case OxCamlType::Placeholder:
    return FormatPlaceholder(stream, static_cast<OxCamlPlaceholderType *>(type),
                             data);
  case OxCamlType::Unknown:
    return FormatUnknown(stream, static_cast<OxCamlUnknownType *>(type), data);
  }
  return false;
}

static std::optional<uint64_t>
ReadDiscriminatorValue(Stream &stream, const OxCamlVariantPart &variant_part,
                       DataExtractor &data, const OxCamlFormatContext &context,
                       std::optional<lldb::addr_t> object_address) {
  const auto &discriminator = variant_part.GetDiscriminator();
  uint64_t discriminator_byte_size = discriminator.GetType()->GetByteSize();
  // The discriminator value is later returned as a uint64_t, so wider
  // discriminators cannot fit and we refuse to read them here.
  if (discriminator_byte_size == 0 ||
      discriminator_byte_size > sizeof(uint64_t))
    return std::nullopt;

  auto resolved = ResolveMemberLocation(stream, discriminator, data, context,
                                        object_address);
  if (!resolved.has_value())
    return std::nullopt;

  llvm::SmallVector<uint8_t, 8> buffer;
  if (!ReadResolvedMemberBytes(stream, discriminator, discriminator_byte_size,
                               *resolved, data, context, buffer))
    return std::nullopt;

  DataExtractor discr_data(buffer.data(), buffer.size(), data.GetByteOrder(),
                           data.GetAddressByteSize());
  lldb::offset_t cursor = 0;
  std::optional<llvm::APInt> apint =
      helpers::ExtractAPInt(discr_data, &cursor, discriminator_byte_size);
  if (!apint.has_value() || apint->getBitWidth() > 64)
    return std::nullopt;

  uint64_t value = apint->getZExtValue();

  if (discriminator.IsBitField()) {
    Log *log = GetLog(OxCamlLog::Formatting);
    auto bit_offset_result =
        discriminator.bit_offset->Evaluate(context, object_address);
    if (!bit_offset_result) {
      LLDB_LOG_ERROR(log, bit_offset_result.takeError(),
                     "ReadDiscriminatorValue: bit_offset evaluation failed for "
                     "discriminator '{1}': {0}",
                     discriminator.name);
      return std::nullopt;
    }
    auto bit_size_result =
        discriminator.bit_size->Evaluate(context, object_address);
    if (!bit_size_result) {
      LLDB_LOG_ERROR(log, bit_size_result.takeError(),
                     "ReadDiscriminatorValue: bit_size evaluation failed for "
                     "discriminator '{1}': {0}",
                     discriminator.name);
      return std::nullopt;
    }
    uint64_t bit_offset = bit_offset_result->GetScalar();
    uint64_t bit_size = bit_size_result->GetScalar();
    if (bit_offset + bit_size > 64)
      return std::nullopt;
    uint64_t discr_mask = MakeLowBitMask(bit_size);
    value = (value >> bit_offset) & discr_mask;
  }

  return value;
}

enum VariantKind {
  SingleEntryVariant, // 1 member, no name
  TupleVariant,       // >1 members, no names
  RecordVariant       // at least 1 named member
};

// Format variant part with generic square bracket format:
// Name[mem1 = val1; ...; memN = valN]
static bool FormatVariantPartGeneric(Stream &stream,
                                     const OxCamlVariantPart &variant_part,
                                     DataExtractor &data,
                                     const OxCamlFormatContext &context,
                                     std::optional<lldb::addr_t> object_address,
                                     uint64_t discr_value,
                                     const std::vector<OxCamlMember> &members) {
  std::string discr_name = "Unknown";
  const auto &discriminator = variant_part.GetDiscriminator();
  if (discriminator.GetType()->GetKind() == OxCamlType::Enum) {
    auto *enum_type = static_cast<OxCamlEnumType *>(discriminator.GetType());
    auto name_opt =
        enum_type->GetEnumeratorName(static_cast<int64_t>(discr_value));
    if (name_opt.has_value()) {
      discr_name = name_opt.value();
    }
  }

  stream.Printf("%s", discr_name.c_str());

  if (!members.empty()) {
    stream.Printf("[");
    for (size_t i = 0; i < members.size(); ++i) {
      if (i > 0)
        stream.Printf("; ");
      if (members[i].name.has_value())
        stream.Printf("%s = ", members[i].name.value().c_str());
      FormatMember(stream, members[i], data, context, object_address);
    }
    stream.Printf("]");
  }

  return true;
}

static bool FormatVariantPartOxCaml(Stream &stream,
                                    const OxCamlVariantPart &variant_part,
                                    DataExtractor &data,
                                    const OxCamlFormatContext &context,
                                    std::optional<lldb::addr_t> object_address,
                                    uint64_t discr_value,
                                    const std::vector<OxCamlMember> &members) {

  const auto &discriminator = variant_part.GetDiscriminator();
  ENSURE(discriminator.GetType()->GetKind() == OxCamlType::Enum, stream,
         "<variant>", "Variant discriminator type '{0}' is not an OCaml enum",
         discriminator.GetType()->GetDisplayName());
  auto *enum_type = static_cast<OxCamlEnumType *>(discriminator.GetType());

  auto name_opt =
      enum_type->GetEnumeratorName(static_cast<int64_t>(discr_value));
  ENSURE(name_opt.has_value(), stream, "<variant>",
         "Variant discriminator value 0x{0:x} has no matching enumerator",
         discr_value);

  stream.Printf("%s", name_opt.value().c_str());

  if (members.empty()) {
    return true;
  }

  stream.Printf(" ");

  // Determine variant kind by checking if all members are unnamed
  bool all_unnamed = true;
  for (const auto &member : members) {
    if (member.name.has_value()) {
      all_unnamed = false;
      break;
    }
  }

  VariantKind kind;
  if (members.size() == 1 && all_unnamed) {
    kind = SingleEntryVariant;
  } else if (members.size() > 1 && all_unnamed) {
    kind = TupleVariant;
  } else {
    kind = RecordVariant;
  }

  const char *open_delim = "";
  const char *close_delim = "";
  const char *separator = "";

  switch (kind) {
  case SingleEntryVariant:
    break;
  case TupleVariant:
    open_delim = "(";
    close_delim = ")";
    separator = ", ";
    break;
  case RecordVariant:
    open_delim = "{ ";
    close_delim = " }";
    separator = "; ";
    break;
  }

  stream.Printf("%s", open_delim);

  for (size_t i = 0; i < members.size(); ++i) {
    if (i > 0)
      stream.Printf("%s", separator);

    if (kind == RecordVariant) {
      if (members[i].name.has_value()) {
        stream.Printf("%s = ", members[i].name.value().c_str());
      } else {
        OXCAML_EMIT_MARKER(
            stream, "<unavailable>",
            "Variant member index {0} lacks a DW_AT_name; using placeholder",
            i);
        stream.PutCString(" = ");
      }
    }

    FormatMember(stream, members[i], data, context, object_address);
  }

  stream.Printf("%s", close_delim);

  return true;
}

// An artificial discriminator (e.g. Pointer/Immediate) with exactly one
// member in the active variant is displayed transparently (member content
// only, no discriminator name or brackets).
static bool FormatVariantPart(Stream &stream,
                              const OxCamlVariantPart &variant_part,
                              DataExtractor &data,
                              const OxCamlFormatContext &context,
                              std::optional<lldb::addr_t> object_address,
                              bool is_ocaml_variant) {
  auto discr_value_opt = ReadDiscriminatorValue(stream, variant_part, data,
                                                context, object_address);
  ENSURE(discr_value_opt.has_value(), stream, "<variant>",
         "Discriminator value unreadable (variant={0})",
         variant_part.GetDiscriminator().name);
  uint64_t discr_value = *discr_value_opt;

  auto active_variant = variant_part.GetActiveVariant(discr_value);
  if (!active_variant.has_value()) {
    stream.Printf("UnknownVariant[]");
    return true;
  }

  const auto &members = (*active_variant)->members;

  if (variant_part.HasArtificialDiscriminator() && members.size() == 1) {
    FormatMember(stream, members[0], data, context, object_address);
    return true;
  }

  if (is_ocaml_variant) {
    return FormatVariantPartOxCaml(stream, variant_part, data, context,
                                   object_address, discr_value, members);
  } else {
    return FormatVariantPartGeneric(stream, variant_part, data, context,
                                    object_address, discr_value, members);
  }
}
