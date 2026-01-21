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
                        lldb::ProcessSP process_sp,
                        const ExecutionContextRef &exe_ctx_ref);
static bool FormatUnboxedBase(Stream &stream,
                              OxCamlUnboxedBaseType *unboxed_type,
                              DataExtractor &data, lldb::ProcessSP process_sp);
static bool FormatFallback(Stream &stream, OxCamlType *type,
                           DataExtractor &data, lldb::ProcessSP process_sp);
static bool FormatEnum(Stream &stream, OxCamlEnumType *enum_type,
                       DataExtractor &data, lldb::ProcessSP process_sp);
static bool FormatPointer(Stream &stream, OxCamlPointerType *ptr_type,
                          DataExtractor &data, lldb::ProcessSP process_sp,
                          const ExecutionContextRef &exe_ctx_ref);
static bool FormatTypedef(Stream &stream, OxCamlTypedefType *typedef_type,
                          DataExtractor &data, lldb::ProcessSP process_sp,
                          const ExecutionContextRef &exe_ctx_ref);
static bool FormatStructure(Stream &stream, OxCamlStructureType *struct_type,
                            DataExtractor &data, lldb::ProcessSP process_sp,
                            const ExecutionContextRef &exe_ctx_ref);
static bool FormatPlaceholder(Stream &stream,
                              OxCamlPlaceholderType *placeholder_type,
                              DataExtractor &data, lldb::ProcessSP process_sp);
static bool FormatUnknown(Stream &stream, OxCamlUnknownType *unknown_type,
                          DataExtractor &data, lldb::ProcessSP process_sp);
static bool FormatArray(Stream &stream, OxCamlArrayType *array_type,
                        DataExtractor &data, lldb::ProcessSP process_sp,
                        const ExecutionContextRef &exe_ctx_ref);

// Forward declarations for variant functions called from
// FormatPointer/FormatStructure
static uint64_t
CalculateMinimumSizeForDiscriminators(OxCamlStructureType *struct_type);
static uint64_t EstimatePointerAllocationSize(OxCamlType *type,
                                              DataExtractor &data);
static uint64_t ComputeActualPointerSize(OxCamlType *pointed_to,
                                         lldb::addr_t adjusted_address,
                                         const DataExtractor &data,
                                         lldb::ProcessSP process_sp);
static bool FormatVariantPart(Stream &stream,
                              const OxCamlVariantPart &variant_part,
                              DataExtractor &data, lldb::ProcessSP process_sp,
                              const ExecutionContextRef &exe_ctx_ref,
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

  const ExecutionContextRef &exe_ctx_ref = valobj.GetExecutionContextRef();

  return FormatValue(stream, type, data, process_sp, exe_ctx_ref);
}

// ============================================================================
// Type-Specific Formatters
// ============================================================================

static bool FormatFallback(Stream &stream, OxCamlType *type,
                           DataExtractor &data, lldb::ProcessSP process_sp) {
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
                              DataExtractor &data, lldb::ProcessSP process_sp) {
  OXCAML_EMIT_MARKER(
      stream, "<placeholder>",
      "Cannot format unresolved placeholder type '{0}' (DIE 0x{1:x16})",
      placeholder_type->GetDisplayName(), placeholder_type->GetDieId());
  return true;
}

static bool FormatUnknown(Stream &stream, OxCamlUnknownType *unknown_type,
                          DataExtractor &data, lldb::ProcessSP process_sp) {
  OXCAML_EMIT_MARKER(
      stream, "<unknown>",
      "Unsupported DWARF tag 0x{0:x} for type '{1}' (DIE 0x{2:x16})",
      unknown_type->GetDwarfTag(), unknown_type->GetDisplayName(),
      unknown_type->GetDieId());
  return true;
}

static bool FormatUnboxedBase(Stream &stream,
                              OxCamlUnboxedBaseType *unboxed_type,
                              DataExtractor &data, lldb::ProcessSP process_sp) {
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
                       DataExtractor &data, lldb::ProcessSP process_sp) {
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

static bool FormatPointer(Stream &stream, OxCamlPointerType *ptr_type,
                          DataExtractor &data, lldb::ProcessSP process_sp,
                          const ExecutionContextRef &exe_ctx_ref) {
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

  // Special case: Arrays are variable-sized and need the raw pointer value
  // Don't dereference - just pass the data (containing pointer) to FormatArray
  if (pointed_to->GetKind() == OxCamlType::Array) {
    return FormatArray(stream, static_cast<OxCamlArrayType *>(pointed_to), data,
                       process_sp, exe_ctx_ref);
  }

  uint64_t size = pointed_to->GetByteSize();
  ENSURE(size != 0, stream, "<pointer>",
         "Resolved target type '{0}' (DIE 0x{1:x}) reports byte size 0; "
         "cannot dereference pointer 0x{2:x}",
         pointed_to->GetDisplayName(), pointed_to->GetDieId(), ptr_value);

  // potentially offset the pointer for OCaml blocks
  int64_t base_offset = pointed_to->GetPointerAdjustmentOffset();
  uint64_t adjusted_address = ptr_value + base_offset;

  uint64_t actual_size =
      ComputeActualPointerSize(pointed_to, adjusted_address, data, process_sp);

  if (base_offset != 0) {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log,
             "FormatPointer: Applying base offset {0} to pointer 0x{1:x}, "
             "adjusted address: 0x{2:x}, reading {3} bytes",
             base_offset, ptr_value, adjusted_address, actual_size);
  }

  ENSURE(
      actual_size != 0, stream, "<pointer>",
      "Resolved target type '{0}' (DIE 0x{1:x}) computed dereference size 0; "
      "cannot dereference pointer 0x{2:x}",
      pointed_to->GetDisplayName(), pointed_to->GetDieId(), ptr_value);

  std::vector<uint8_t> buffer(actual_size);
  Status error;
  size_t bytes_read = process_sp->ReadMemory(adjusted_address, buffer.data(),
                                             actual_size, error);

  ENSURE(bytes_read == actual_size && error.Success(), stream, "<pointer>",
         "Failed to read {0} bytes from address 0x{1:x} for pointer 0x{2:x} "
         "to '{3}' (DIE 0x{4:x})",
         actual_size, adjusted_address, ptr_value, pointed_to->GetDisplayName(),
         pointed_to->GetDieId());

  DataExtractor pointed_data(buffer.data(), actual_size, data.GetByteOrder(),
                             data.GetAddressByteSize());

  return FormatValue(stream, pointed_to, pointed_data, process_sp, exe_ctx_ref);
}

static bool FormatTypedef(Stream &stream, OxCamlTypedefType *typedef_type,
                          DataExtractor &data, lldb::ProcessSP process_sp,
                          const ExecutionContextRef &exe_ctx_ref) {
  return FormatValue(stream, typedef_type->GetUnderlyingType(), data,
                     process_sp, exe_ctx_ref);
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
                            lldb::ProcessSP process_sp,
                            const ExecutionContextRef &exe_ctx_ref) {
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

  auto info_opt = ExtractExceptionInfo(exception_value, process_sp);

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
                             process_sp->GetByteOrder(), constants::WORD_SIZE);
      oxcaml::FormatOxCamlValue(stream, arg_data, process_sp, exe_ctx_ref);
    }

    if (use_parens) {
      stream.Printf(")");
    }
  }

  return true;
}

static bool FormatArray(Stream &stream, OxCamlArrayType *array_type,
                        DataExtractor &data, lldb::ProcessSP process_sp,
                        const ExecutionContextRef &exe_ctx_ref) {
  ENSURE(data.GetByteSize() >= helpers::constants::WORD_SIZE, stream, "<array>",
         "Data size {0} < expected word size {1}", data.GetByteSize(),
         helpers::constants::WORD_SIZE);

  lldb::offset_t offset = 0;
  uint64_t array_ptr = data.GetU64(&offset);

  ENSURE(offset != 0, stream, "<array>",
         "Failed to read OCaml array pointer from value data ({0} bytes "
         "available)",
         data.GetByteSize());

  // Check for null or immediate values (not actual pointers)
  ENSURE(array_ptr != 0 && !helpers::value::IsImmediate(array_ptr), stream,
         llvm::formatv("<array@0x{0:x}>", array_ptr).str(),
         "Array pointer 0x{0:x} is a {1}; skipping dereference", array_ptr,
         array_ptr == 0 ? "null pointer" : "immediate OCaml value");

  auto header_opt = helpers::ReadBlockHeader(array_ptr, process_sp);
  ENSURE(header_opt.has_value(), stream,
         llvm::formatv("<array@0x{0:x}>", array_ptr).str(),
         "Failed to read OCaml array header for pointer 0x{0:x}", array_ptr);

  uint64_t header = *header_opt;
  uint8_t tag;
  uint64_t wosize;
  uint8_t reserved;
  helpers::header::ParseHeader(header, tag, wosize, reserved);

  // OCaml float arrays (tag 254) store unboxed 8-byte IEEE 754 doubles
  // wosize is the number of doubles (1 word = 1 double on 64-bit platforms)
  if (tag == constants::DOUBLE_ARRAY_TAG) {
    return oxcaml::FormatOxCamlDoubleArray(stream, array_ptr, wosize,
                                           process_sp);
  }

  // Regular arrays: calculate number of elements from wosize and stride
  // wosize is number of words, total bytes = wosize * WORD_SIZE
  OxCamlType *element_type = array_type->GetElementType();
  uint64_t stride = array_type->GetStride();
  ENSURE(stride != 0, stream, llvm::formatv("<array@0x{0:x}>", array_ptr).str(),
         "Array type '{0}' (DIE 0x{1:x}) has stride 0; cannot format array at "
         "0x{2:x}",
         array_type->GetDisplayName(), array_type->GetDieId(), array_ptr);

  uint64_t total_bytes = wosize * helpers::constants::WORD_SIZE;
  uint64_t num_elements = total_bytes / stride;
  Status error;

  stream.Printf("[| ");

  for (uint64_t i = 0; i < num_elements; i++) {
    if (i > 0)
      stream.Printf("; ");

    uint64_t element_address = array_ptr + (i * stride);

    std::vector<uint8_t> buffer(stride);
    size_t bytes_read =
        process_sp->ReadMemory(element_address, buffer.data(), stride, error);

    if (bytes_read != stride || error.Fail()) {
      OXCAML_EMIT_MARKER(stream, "<element>",
                         "Failed to read array element {0} at 0x{1:x}; "
                         "requested {2} bytes, read {3}",
                         i, element_address, stride, bytes_read);
      error.Clear();
      continue;
    }

    DataExtractor element_data(buffer.data(), stride, data.GetByteOrder(),
                               data.GetAddressByteSize());
    FormatValue(stream, element_type, element_data, process_sp, exe_ctx_ref);
  }

  stream.Printf(" |]");
  return true;
}

static bool FormatMember(Stream &stream, const OxCamlMember &member,
                         DataExtractor &data, lldb::ProcessSP process_sp,
                         const ExecutionContextRef &exe_ctx_ref) {
  if (member.IsBitField()) {
    uint64_t bit_offset = member.bit_offset.value();
    uint64_t bit_size = member.bit_size.value();
    lldb::offset_t byte_offset = member.data_member_location;

    // Check if bit-field spans multiple words (not currently supported)
    // Multi-word bit-fields would require reading multiple 64-bit words and
    // combining bits across word boundaries
    ENSURE(bit_offset + bit_size <= 64, stream, "<bit-field>",
           "Bit-field '{0}' spans multiple words (offset={1}, size={2}); "
           "multi-word bit-fields not yet supported",
           member.name.value_or("<unnamed>"), bit_offset, bit_size);

    ENSURE(byte_offset + sizeof(uint64_t) <= data.GetByteSize(), stream,
           "<member>",
           "Not enough data ({0} bytes) to read bit-field '{1}' starting at "
           "offset {2}",
           data.GetByteSize(), member.name.value_or("<unnamed>"),
           static_cast<unsigned>(byte_offset));

    uint64_t full_value = data.GetU64(&byte_offset);
    uint64_t bit_mask = MakeLowBitMask(bit_size);
    uint64_t extracted = (full_value >> bit_offset) & bit_mask;

    DataExtractor bit_data(&extracted, sizeof(extracted), data.GetByteOrder(),
                           data.GetAddressByteSize());

    return FormatValue(stream, member.GetType(), bit_data, process_sp,
                       exe_ctx_ref);
  }

  DataExtractor member_data(data, member.data_member_location,
                            member.GetType()->GetByteSize());
  return FormatValue(stream, member.GetType(), member_data, process_sp,
                     exe_ctx_ref);
}

static bool FormatStructure(Stream &stream, OxCamlStructureType *struct_type,
                            DataExtractor &data, lldb::ProcessSP process_sp,
                            const ExecutionContextRef &exe_ctx_ref) {
  const auto &members = struct_type->GetMembers();
  const auto &variant_parts = struct_type->GetVariantParts();

  // Special case for OCaml variants: Structures with no direct members and
  // exactly one variant part are formatted without braces (just the variant
  // content directly). This represents the typical OCaml variant structure
  // encoding.
  if (members.empty() && variant_parts.size() == 1) {
    return FormatVariantPart(stream, variant_parts[0], data, process_sp,
                             exe_ctx_ref, true);
  }

  // Special case for OCaml exceptions: Union-style structure with "exn" and
  // "raw" members. Exceptions have both members at the same offset. We format
  // only the "raw" member. This hardcodes the current format of the OxCaml
  // compiler.
  if (members.size() == 2 && variant_parts.empty() &&
      members[0].name.has_value() && members[1].name.has_value() &&
      members[0].data_member_location == 0 &&
      members[1].data_member_location == 0) {
    const std::string &name0 = members[0].name.value();
    const std::string &name1 = members[1].name.value();
    if ((name0 == "exn" && name1 == "raw") ||
        (name0 == "raw" && name1 == "exn")) {
      return FormatException(stream, data, process_sp, exe_ctx_ref);
    }
  }

  // Regular case: structure with members and/or multiple variant parts
  // Determine formatting based on type
  bool is_tuple = struct_type->IsTuple();
  const char *open_delim = is_tuple ? "(" : "{ ";
  const char *close_delim = is_tuple ? ")" : " }";
  const char *separator = is_tuple ? ", " : "; ";

  stream.Printf("%s", open_delim);

  bool has_content = false;

  // Format regular members first
  for (size_t i = 0; i < members.size(); ++i) {
    if (has_content)
      stream.Printf("%s", separator);

    if (members[i].name.has_value()) {
      stream.Printf("%s = ", members[i].name.value().c_str());
    }

    FormatMember(stream, members[i], data, process_sp, exe_ctx_ref);
    has_content = true;
  }

  // Format variant parts
  for (size_t i = 0; i < variant_parts.size(); ++i) {
    if (has_content)
      stream.Printf("%s", separator);

    FormatVariantPart(stream, variant_parts[i], data, process_sp, exe_ctx_ref,
                      false);
    has_content = true;
  }

  stream.Printf("%s", close_delim);

  return true;
}

static bool FormatValue(Stream &stream, OxCamlType *type, DataExtractor &data,
                        lldb::ProcessSP process_sp,
                        const ExecutionContextRef &exe_ctx_ref) {
  if (!type) {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(
        log,
        "FormatValue: No type information available, using fallback formatter");
    return FormatFallback(stream, type, data, process_sp);
  }

  switch (type->GetKind()) {
  case OxCamlType::Value:
    return oxcaml::FormatOxCamlValue(stream, data, process_sp, exe_ctx_ref);
  case OxCamlType::UnboxedBase:
    return FormatUnboxedBase(stream, static_cast<OxCamlUnboxedBaseType *>(type),
                             data, process_sp);
  case OxCamlType::Enum:
    return FormatEnum(stream, static_cast<OxCamlEnumType *>(type), data,
                      process_sp);
  case OxCamlType::Pointer:
    return FormatPointer(stream, static_cast<OxCamlPointerType *>(type), data,
                         process_sp, exe_ctx_ref);
  case OxCamlType::Typedef:
    return FormatTypedef(stream, static_cast<OxCamlTypedefType *>(type), data,
                         process_sp, exe_ctx_ref);
  case OxCamlType::Structure:
    return FormatStructure(stream, static_cast<OxCamlStructureType *>(type),
                           data, process_sp, exe_ctx_ref);
  case OxCamlType::Array:
    return FormatArray(stream, static_cast<OxCamlArrayType *>(type), data,
                       process_sp, exe_ctx_ref);
  case OxCamlType::Placeholder:
    return FormatPlaceholder(stream, static_cast<OxCamlPlaceholderType *>(type),
                             data, process_sp);
  case OxCamlType::Unknown:
    return FormatUnknown(stream, static_cast<OxCamlUnknownType *>(type), data,
                         process_sp);
  }
  return false;
}

// ============================================================================
// Variant Formatting
// ============================================================================
//
// ## Problem Overview
//
// OCaml variants present a formatting challenge: the actual memory size of a
// variant value depends on which constructor is active at runtime. In the
// current implementation of the OxCaml compiler and the LLDB plugin, the DWARF
// information only provides static size information (the base structure size),
// but each variant constructor may have a different payload size.
//
// ## Current Approach: Approximate Two-Pass Strategy
//
// Since the DWARF doesn't encode dynamic size calculations (which would require
// DWARF expressions), we use an estimation approach:
//
// 1. **First Pass - Read Discriminators**:
//    - Calculate the minimum memory needed to read all discriminators
//    - Read enough memory to analyze which variant constructors are active
//
// 2. **Second Pass - Estimate Total Size**:
//    - Based on active variants, calculate the maximum offset required
//    - This includes: base structure members + active variant member payloads
//    - Read the estimated total memory needed
//
// ## Key Functions:
//
// - `ReadDiscriminatorValue()` - Extracts discriminator value from data
// - `CalculateMinimumSizeForDiscriminators()` - Computes memory needed for
//   discriminator analysis
// - `FindActiveVariantsInStructure()` - Determines which variants are active
//   based on discriminator values
// - `EstimatePointerAllocationSize()` - Estimates total allocation size by
//   examining all active variant payloads
// - `FormatVariantPart()` - Main dispatcher for variant formatting
// - `FormatVariantPartOxCaml()` - OCaml-style variant formatting
// - `FormatVariantPartGeneric()` - Generic variant formatting
//
// ## Limitations
//
// This approach provides an estimate rather than exact sizes:
// - We calculate the maximum offset across all members in the active variant
// - Member type sizes may themselves be estimates (e.g., nested structures)
// - The actual memory layout might be more compact than our calculation
// suggests
//
// A more precise solution would require the OCaml compiler to emit DWARF
// expressions that dynamically calculate exact sizes based on runtime values.
//
// ============================================================================

static std::optional<uint64_t>
ReadDiscriminatorValue(const OxCamlVariantPart &variant_part,
                       DataExtractor &data) {
  const auto &discriminator = variant_part.GetDiscriminator();
  lldb::offset_t discr_offset = discriminator.data_member_location;

  uint64_t discriminator_byte_size = discriminator.GetType()->GetByteSize();

  std::optional<llvm::APInt> apint =
      helpers::ExtractAPInt(data, &discr_offset, discriminator_byte_size);
  if (!apint.has_value() || apint->getBitWidth() > 64) {
    return std::nullopt;
  }

  uint64_t value = apint->getZExtValue();

  if (discriminator.IsBitField()) {
    uint64_t bit_offset = discriminator.bit_offset.value();
    uint64_t bit_size = discriminator.bit_size.value();
    if (bit_offset + bit_size > 64)
      return std::nullopt;
    uint64_t discr_mask = MakeLowBitMask(bit_size);
    value = (value >> bit_offset) & discr_mask;
  }

  return value;
}

static uint64_t
CalculateMinimumSizeForDiscriminators(OxCamlStructureType *struct_type) {
  uint64_t max_discriminator_end = 0;

  const auto &variant_parts = struct_type->GetVariantParts();
  for (const auto &variant_part : variant_parts) {
    const auto &discr = variant_part.GetDiscriminator();
    uint64_t discr_end =
        discr.data_member_location + discr.GetType()->GetByteSize();
    max_discriminator_end = std::max(max_discriminator_end, discr_end);
  }

  return max_discriminator_end;
}

static std::vector<const OxCamlVariantPart::Variant *>
FindActiveVariantsInStructure(OxCamlStructureType *struct_type,
                              DataExtractor &data) {
  std::vector<const OxCamlVariantPart::Variant *> active_variants;
  Log *log = GetLog(OxCamlLog::UserVisibleErrors);

  const auto &variant_parts = struct_type->GetVariantParts();
  for (const auto &variant_part : variant_parts) {
    auto discr_value_opt = ReadDiscriminatorValue(variant_part, data);
    if (!discr_value_opt.has_value()) {
      LLDB_LOG(log,
               "Failed to read discriminator value for variant part with "
               "discriminator '{0}'",
               variant_part.GetDiscriminator().name);
      continue; // Skip variant part if discriminator cannot be read
    }

    auto active_variant = variant_part.GetActiveVariant(*discr_value_opt);
    if (active_variant.has_value()) {
      active_variants.push_back(*active_variant);
    }
  }

  return active_variants;
}

static uint64_t EstimatePointerAllocationSize(OxCamlType *type,
                                              DataExtractor &data) {
  OX_ASSERT(type != nullptr,
            "EstimatePointerAllocationSize called with null type (ptr={0:P})",
            type);

  while (type->GetKind() == OxCamlType::Typedef) {
    type = static_cast<OxCamlTypedefType *>(type)->GetUnderlyingType();
  }

  // We assume all types except for structures return accurate sizes. Only
  // structures can have variant parts.
  if (type->GetKind() != OxCamlType::Structure) {
    return type->GetByteSize();
  }

  auto *struct_type = static_cast<OxCamlStructureType *>(type);
  uint64_t max_end_offset = type->GetByteSize();

  const auto &members = struct_type->GetMembers();
  for (const auto &member : members) {
    // If the size that is returned here is not exact (e.g., as in the case of
    // structures, then our estimate here is off). That's why it is not exact.
    uint64_t member_end =
        member.data_member_location + member.GetType()->GetByteSize();
    max_end_offset = std::max(max_end_offset, member_end);
  }

  auto active_variants = FindActiveVariantsInStructure(struct_type, data);
  for (const auto *variant : active_variants) {
    for (const auto &member : variant->members) {
      uint64_t member_end =
          member.data_member_location + member.GetType()->GetByteSize();
      max_end_offset = std::max(max_end_offset, member_end);
    }
  }

  return max_end_offset;
}

static uint64_t ComputeActualPointerSize(OxCamlType *pointed_to,
                                         lldb::addr_t adjusted_address,
                                         const DataExtractor &data,
                                         lldb::ProcessSP process_sp) {
  uint64_t type_size = pointed_to->GetByteSize();

  if (pointed_to->GetKind() != OxCamlType::Structure) {
    return type_size;
  }

  auto *struct_type = static_cast<OxCamlStructureType *>(pointed_to);
  const auto &variant_parts = struct_type->GetVariantParts();

  if (variant_parts.empty()) {
    return type_size;
  }

  uint64_t min_discriminator_size =
      CalculateMinimumSizeForDiscriminators(struct_type);
  uint64_t temp_read_size = std::max(min_discriminator_size, type_size);

  // Two-pass approach for structures with variant parts:
  // 1. Read enough data to analyze all discriminators
  // 2. Calculate precise size based on active variants
  Status error;
  std::vector<uint8_t> temp_buffer(temp_read_size);
  size_t bytes_read = process_sp->ReadMemory(
      adjusted_address, temp_buffer.data(), temp_buffer.size(), error);

  if (bytes_read >= min_discriminator_size) {
    DataExtractor heap_data(temp_buffer.data(), bytes_read, data.GetByteOrder(),
                            data.GetAddressByteSize());
    uint64_t estimated_size =
        EstimatePointerAllocationSize(pointed_to, heap_data);
    return estimated_size;
  }

  return type_size;
}

enum VariantKind {
  SingleEntryVariant, // 1 member, no name
  TupleVariant,       // >1 members, no names
  RecordVariant       // at least 1 named member
};

// Format variant part with generic square bracket format:
// Name[mem1 = val1; ...; memN = valN]
static bool FormatVariantPartGeneric(
    Stream &stream, const OxCamlVariantPart &variant_part, DataExtractor &data,
    lldb::ProcessSP process_sp, const ExecutionContextRef &exe_ctx_ref,
    uint64_t discr_value, const std::vector<OxCamlMember> &members) {
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
      FormatMember(stream, members[i], data, process_sp, exe_ctx_ref);
    }
    stream.Printf("]");
  }

  return true;
}

static bool FormatVariantPartOxCaml(
    Stream &stream, const OxCamlVariantPart &variant_part, DataExtractor &data,
    lldb::ProcessSP process_sp, const ExecutionContextRef &exe_ctx_ref,
    uint64_t discr_value, const std::vector<OxCamlMember> &members) {

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

    FormatMember(stream, members[i], data, process_sp, exe_ctx_ref);
  }

  stream.Printf("%s", close_delim);

  return true;
}

// Main variant formatting dispatcher
//
// Special handling for artificial discriminators:
// - Artificial discriminators (e.g., Pointer/Immediate) with exactly one member
//   in the active variant are displayed transparently (member content only)
// - All other cases dispatch to either OCaml or generic formatting
static bool FormatVariantPart(Stream &stream,
                              const OxCamlVariantPart &variant_part,
                              DataExtractor &data, lldb::ProcessSP process_sp,
                              const ExecutionContextRef &exe_ctx_ref,
                              bool is_ocaml_variant = false) {
  auto discr_value_opt = ReadDiscriminatorValue(variant_part, data);
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

  // Special case: artificial discriminator with exactly one member
  // Display the member content directly without discriminator name/brackets
  if (variant_part.HasArtificialDiscriminator() && members.size() == 1) {
    FormatMember(stream, members[0], data, process_sp, exe_ctx_ref);
    return true;
  }

  if (is_ocaml_variant) {
    return FormatVariantPartOxCaml(stream, variant_part, data, process_sp,
                                   exe_ctx_ref, discr_value, members);
  } else {
    return FormatVariantPartGeneric(stream, variant_part, data, process_sp,
                                    exe_ctx_ref, discr_value, members);
  }
}
