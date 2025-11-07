//===-- OxCamlFormatters.cpp --------------------------------------------===//
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
///
/// ## Benefits
///
/// - **Uniform Data Flow**: Each formatter knows it receives bytes to format
/// - **Composable**: Each formatter can create appropriate DataExtractors for
///   recursive calls
/// - **LLDB-Idiomatic**: Aligns with how C++ and other language formatters work
///
/// ## Example Flow
///
/// For a pointer to a tuple (int * int):
/// 1. Entry point gets DataExtractor with 8 bytes (the pointer value)
/// 2. FormatPointer reads pointer value, fetches 16 bytes from memory
/// 3. Creates new DataExtractor with those 16 bytes
/// 4. FormatStructure receives the 16-byte DataExtractor
/// 5. Creates two 8-byte sub-DataExtractors (offset 0 and offset 8)
/// 6. FormatBase formats each integer from its respective DataExtractor

#include "OxCamlFormatters.h"
#include "LogChannelOxCaml.h"
#include "OxCamlFormatHelpers.h"
#include "OxCamlHelpers.h"
#include "OxCamlValueFormatters.h"
#include "Plugins/TypeSystem/OxCaml/TypeSystemOxCaml.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Reference.h"
#include "lldb/Utility/Status.h"
#include "lldb/ValueObject/ValueObject.h"
#include "llvm/Support/FormatVariadic.h"
#include <cassert>
#include <cinttypes>
#include <cstring>
#include <string>
#include <vector>

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters;
using namespace lldb_private::formatters::oxcaml;
using namespace lldb_private::formatters::oxcaml::helpers;

// Forward declarations
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

// Helper function to safely extract OxCamlType from CompilerType
static OxCamlType *ExtractOxCamlType(const CompilerType &compiler_type) {
  if (!compiler_type.IsValid()) {
    llvm::report_fatal_error("Invalid CompilerType in ExtractOxCamlType");
  }

  auto *type_ref =
      static_cast<Reference<OxCamlType> *>(compiler_type.GetOpaqueQualType());
  if (!type_ref) {
    llvm::report_fatal_error("Invalid type reference in ExtractOxCamlType");
  }

  return type_ref->get();
}

// CR sspies: There are a lot of helper functions in this file that should be
// factored out. They have to do with the formatting of variants, which can be a
// bit subtle.

static uint64_t ReadDiscriminatorValue(const OxCamlVariantPart &variant_part,
                                       DataExtractor &data) {
  const auto &discriminator = variant_part.GetDiscriminator();
  lldb::offset_t discr_offset = discriminator.data_member_location;

  uint64_t discriminator_byte_size = discriminator.GetType()->GetByteSize();
  uint64_t value;

  if (discriminator_byte_size == 1) {
    value = data.GetU8(&discr_offset);
  } else {
    value = data.GetU64(&discr_offset);
  }

  // Handle bit fields if present
  if (discriminator.IsBitField()) {
    uint64_t bit_offset = discriminator.bit_offset.value();
    uint64_t bit_size = discriminator.bit_size.value();
    uint64_t discr_mask = (1ULL << bit_size) - 1;
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

  const auto &variant_parts = struct_type->GetVariantParts();
  for (const auto &variant_part : variant_parts) {
    uint64_t discr_value = ReadDiscriminatorValue(variant_part, data);

    auto active_variant = variant_part.GetActiveVariant(discr_value);
    if (active_variant.has_value()) {
      active_variants.push_back(*active_variant);
    }
  }

  return active_variants;
}

// Estimate memory allocation size for heap blocks pointed to by OxCaml pointers
//
// NOTE: This function provides an estimate but not the precise allocation size.
//
// FUTURE: This function is a temporary workaround for DWARF information that
// reports only the base structure size. We plan to enhance the OCaml compiler
// to emit variant-specific sizes in DWARF, which will make this estimation
// unnecessary.
//
static uint64_t EstimatePointerAllocationSize(OxCamlType *type,
                                              DataExtractor &data) {
  if (!type) {
    return 0;
  }

  // CR sspies: This is being conservative and probably not necessary.
  while (type->GetKind() == OxCamlType::Typedef) {
    type = static_cast<OxCamlTypedefType *>(type)->GetUnderlyingType();
  }

  if (type->GetKind() != OxCamlType::Structure) {
    return type->GetByteSize();
  }

  auto *struct_type = static_cast<OxCamlStructureType *>(type);
  uint64_t max_end_offset = type->GetByteSize();

  const auto &members = struct_type->GetMembers();
  for (const auto &member : members) {
    // CR sspies: If the size that is returned here is not exact (e.g., as in
    // the case of structures, then our estimate here is off). That's why it is
    // not exact.
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

static bool FormatFallback(Stream &stream, OxCamlType *type,
                           DataExtractor &data, lldb::ProcessSP process_sp) {
  size_t byte_size = data.GetByteSize();
  if (byte_size == 0) {
    constexpr const char *marker = "<empty>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker,
        "Zero-byte value encountered while formatting type '{0}' (DIE 0x{1:x})",
        type->GetDisplayName(), type->GetDieId());
    stream.PutCString(marker);
    return true;
  }

  stream.PutCString("data(");
  for (size_t i = 0; i < byte_size; ++i) {
    if (i > 0)
      stream.PutCString(" ");
    lldb::offset_t offset = i;
    uint8_t byte = data.GetU8(&offset);
    stream.Printf("%02x", byte);
  }
  stream.PutCString(")");
  return true;
}

static bool FormatPlaceholder(Stream &stream,
                              OxCamlPlaceholderType *placeholder_type,
                              DataExtractor &data, lldb::ProcessSP process_sp) {
  constexpr const char *marker = "<error>";
  OXCAML_EXPLAIN_ERROR_MARKER(
      marker, "Cannot format unresolved placeholder type '{0}' (DIE 0x{1:x16})",
      placeholder_type->GetDisplayName(), placeholder_type->GetDieId());

  stream.PutCString(marker);
  return true;
}

static bool FormatUnknown(Stream &stream, OxCamlUnknownType *unknown_type,
                          DataExtractor &data, lldb::ProcessSP process_sp) {
  constexpr char marker[] = "<error>";
  OXCAML_EXPLAIN_ERROR_MARKER(
      marker, "Unsupported DWARF tag 0x{0:x} for type '{1}' (DIE 0x{2:x16})",
      unknown_type->GetDwarfTag(), unknown_type->GetDisplayName(),
      unknown_type->GetDieId());

  stream.PutCString(marker);
  return true;
}

static std::optional<helpers::FloatSize>
ByteSizeToFloatSize(uint64_t byte_size) {
  switch (byte_size) {
  case constants::FLOAT16_SIZE:
    return helpers::FloatSize::Half;
  case constants::FLOAT32_SIZE:
    return helpers::FloatSize::Single;
  case constants::FLOAT64_SIZE:
    return helpers::FloatSize::Double;
  default:
    return std::nullopt;
  }
}

static bool FormatUnboxedBase(Stream &stream,
                              OxCamlUnboxedBaseType *unboxed_type,
                              DataExtractor &data, lldb::ProcessSP process_sp) {
  uint64_t byte_size = unboxed_type->GetByteSize();
  OxCamlUnboxedBaseType::BaseKind kind = unboxed_type->GetBaseKind();

  if (byte_size == 0) {
    constexpr const char *marker = "<0-byte base type>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Unboxed base type '{0}' (DIE 0x{1:x}) reports zero byte size",
        unboxed_type->GetDisplayName(), unboxed_type->GetDieId());
    stream.PutCString(marker);
    return true;
  }

  lldb::offset_t offset = 0;
  switch (kind) {
  case OxCamlUnboxedBaseType::Signed: {
    std::optional<llvm::APInt> apint =
        helpers::ExtractAPInt(data, &offset, byte_size);
    if (!apint) {
      llvm::report_fatal_error("Failed to extract signed integer");
    }
    std::string suffix = suffixes::GetSignedIntegerSuffix(byte_size);
    helpers::FormatAPInt(&stream, *apint, true, "#", suffix);
    return true;
  }
  case OxCamlUnboxedBaseType::Unsigned: {
    std::optional<llvm::APInt> apint =
        helpers::ExtractAPInt(data, &offset, byte_size);
    if (!apint) {
      llvm::report_fatal_error("Failed to extract unsigned integer");
    }
    std::string suffix = suffixes::GetUnsignedIntegerSuffix(byte_size);
    helpers::FormatAPInt(&stream, *apint, false, "#", suffix);
    return true;
  }
  case OxCamlUnboxedBaseType::Float: {
    std::optional<helpers::FloatSize> float_size =
        ByteSizeToFloatSize(byte_size);
    if (!float_size) {
      // Keep the formatted marker alive while logging/printing; taking c_str()
      // on a temporary would leave us with a dangling pointer.
      std::string marker_storage =
          llvm::formatv("<{0}-byte float>", byte_size).str();
      const char *marker = marker_storage.c_str();
      OXCAML_EXPLAIN_ERROR_MARKER(marker,
                                  "Float value width {0} bytes for type '{1}' "
                                  "(DIE 0x{2:x}) is unsupported",
                                  byte_size, unboxed_type->GetDisplayName(),
                                  unboxed_type->GetDieId());
      stream.PutCString(marker);
      return true;
    }
    std::optional<llvm::APFloat> apfloat =
        helpers::ExtractAPFloat(data, &offset, *float_size);
    if (!apfloat) {
      llvm::report_fatal_error("Failed to extract float");
    }
    std::string suffix = (*float_size == helpers::FloatSize::Single)
                             ? suffixes::FLOAT32_SUFFIX
                             : "";
    helpers::FormatAPFloat(&stream, *apfloat, std::nullopt, "#", suffix);
    return true;
  }
  }
}

static bool FormatEnum(Stream &stream, OxCamlEnumType *enum_type,
                       DataExtractor &data, lldb::ProcessSP process_sp) {
  assert(enum_type->GetByteSize() == helpers::constants::WORD_SIZE &&
         "OCaml enum types must be 8 bytes");

  lldb::offset_t offset = 0;
  uint64_t value = data.GetU64(&offset);

  auto name_opt = enum_type->GetEnumeratorName(value);
  if (name_opt.has_value()) {
    stream.PutCString(name_opt.value());
  } else {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log,
             "FormatEnum: Enumerator not found for value 0x{0:x}, using "
             "fallback formatting",
             value);
    return FormatFallback(stream, enum_type, data, process_sp);
  }
  return true;
}

static bool FormatPointer(Stream &stream, OxCamlPointerType *ptr_type,
                          DataExtractor &data, lldb::ProcessSP process_sp,
                          const ExecutionContextRef &exe_ctx_ref) {
  assert(ptr_type->GetByteSize() == helpers::constants::WORD_SIZE &&
         "OCaml pointer types must be 8 bytes");

  lldb::offset_t offset = 0;
  uint64_t ptr_value = data.GetU64(&offset);

  if (helpers::value::IsImmediate(ptr_value)) {
    constexpr const char *pointer_marker = "<pointer>";
    OXCAML_EXPLAIN_ERROR_MARKER(pointer_marker,
                                "Pointer value 0x{0:x} is an OCaml immediate; "
                                "cannot dereference as '{1}' (DIE 0x{2:x})",
                                ptr_value, ptr_type->GetDisplayName(),
                                ptr_type->GetDieId());
    stream.PutCString(pointer_marker);
    return true;
  }

  OxCamlType *pointed_to = ptr_type->GetPointedToType();
  if (!pointed_to || !process_sp) {
    constexpr const char *pointer_marker = "<pointer>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        pointer_marker,
        "Pointer 0x{0:x} lacks a resolved target type or process context while "
        "formatting '{1}' (DIE 0x{2:x})",
        ptr_value, ptr_type->GetDisplayName(), ptr_type->GetDieId());
    stream.PutCString(pointer_marker);
    return true;
  }

  // Special case: Arrays are variable-sized and need the raw pointer value
  // Don't dereference - just pass the data (containing pointer) to FormatArray
  if (pointed_to->GetKind() == OxCamlType::Array) {
    return FormatValue(stream, pointed_to, data, process_sp, exe_ctx_ref);
  }

  uint64_t size = pointed_to->GetByteSize();
  if (size == 0) {
    constexpr const char *pointer_marker = "<pointer>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        pointer_marker,
        "Resolved target type '{0}' (DIE 0x{1:x}) reports byte size 0; cannot "
        "dereference pointer 0x{2:x}",
        pointed_to->GetDisplayName(), pointed_to->GetDieId(), ptr_value);
    stream.PutCString(pointer_marker);
    return true;
  }

  int64_t base_offset = pointed_to->GetPointerAdjustmentOffset();
  uint64_t adjusted_address = ptr_value + base_offset;

  uint64_t actual_size = size;

  // CR sspies: This is being conservative. This is probably not necessary
  // in the case of OxCaml. This can also probably be factored out into a nice
  // recursive structure that avoids the while loop here.
  OxCamlType *resolved_type = pointed_to;
  while (resolved_type->GetKind() == OxCamlType::Typedef) {
    resolved_type =
        static_cast<OxCamlTypedefType *>(resolved_type)->GetUnderlyingType();
  }

  if (resolved_type->GetKind() == OxCamlType::Structure) {
    auto *struct_type = static_cast<OxCamlStructureType *>(resolved_type);
    const auto &variant_parts = struct_type->GetVariantParts();

    if (!variant_parts.empty()) {
      // Two-pass approach for structures with variant parts:
      // 1. Read enough data to analyze all discriminators
      // 2. Calculate precise size based on active variants

      uint64_t min_discriminator_size =
          CalculateMinimumSizeForDiscriminators(struct_type);
      uint64_t temp_read_size = std::max(min_discriminator_size, size);

      Status error;
      std::vector<uint8_t> temp_buffer(temp_read_size);
      size_t bytes_read = process_sp->ReadMemory(
          adjusted_address, temp_buffer.data(), temp_buffer.size(), error);

      if (bytes_read >= min_discriminator_size) {
        DataExtractor heap_data(temp_buffer.data(), bytes_read,
                                data.GetByteOrder(), data.GetAddressByteSize());
        uint64_t estimated_size =
            EstimatePointerAllocationSize(pointed_to, heap_data);
        actual_size = estimated_size;
      } else {
        actual_size = size;
      }
    }
  }

  if (base_offset != 0) {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log,
             "FormatPointer: Applying base offset {0} to pointer 0x{1:x}, "
             "adjusted address: 0x{2:x}, reading {3} bytes",
             base_offset, ptr_value, adjusted_address, actual_size);
  }

  std::vector<uint8_t> buffer(actual_size);
  Status error;
  size_t bytes_read = process_sp->ReadMemory(adjusted_address, buffer.data(),
                                             actual_size, error);

  if (bytes_read != actual_size || !error.Success()) {
    constexpr const char *pointer_marker = "<pointer>";
    OXCAML_EXPLAIN_ERROR_MARKER(pointer_marker,
                                "Failed to read {0} bytes from address 0x{1:x} "
                                "for pointer 0x{2:x} to '{3}' (DIE 0x{4:x})",
                                actual_size, adjusted_address, ptr_value,
                                pointed_to->GetDisplayName(),
                                pointed_to->GetDieId());
    stream.PutCString(pointer_marker);
    return true;
  }

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
// 1. Exception with NO arguments (e.g., "exception Empty"):
//    The exception value IS a pointer to the Object_tag block:
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
  std::string constructor_name; // Exception constructor name
  std::vector<uint64_t>
      arguments; // Exception arguments (empty if no arguments)
};

static std::optional<ExceptionInfo>
ExtractExceptionInfo(lldb::addr_t exception_addr, lldb::ProcessSP process_sp) {
  Status error;
  Log *log = GetLog(OxCamlLog::Formatting);

  uint64_t header = process_sp->ReadUnsignedIntegerFromMemory(
      exception_addr - constants::WORD_SIZE, constants::WORD_SIZE, 0, error);
  if (error.Fail()) {
    LLDB_LOG(log, "Failed to read exception block header at 0x{0:x}: {1}",
             exception_addr, error.AsCString());
    return std::nullopt;
  }

  uint8_t tag = header::ExtractTag(header);
  uint64_t wosize = header::ExtractWosize(header);

  std::vector<uint64_t> arguments;
  lldb::addr_t string_addr;

  switch (tag) {
  case static_cast<uint8_t>(constants::SpecialTag::Object_tag): {
    uint64_t string_ptr = process_sp->ReadUnsignedIntegerFromMemory(
        exception_addr, constants::WORD_SIZE, 0, error);
    if (error.Fail() || (string_ptr & 1) != 0 || string_ptr == 0) {
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
    if (error.Fail() || (obj_tag_ptr & 1) != 0 || obj_tag_ptr == 0) {
      LLDB_LOG(log,
               "Failed to read valid Object_tag pointer from exception block "
               "at 0x{0:x}",
               exception_addr);
      return std::nullopt;
    }

    uint64_t string_ptr = process_sp->ReadUnsignedIntegerFromMemory(
        obj_tag_ptr, constants::WORD_SIZE, 0, error);
    if (error.Fail() || (string_ptr & 1) != 0 || string_ptr == 0) {
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

  if (exception_value == 0 || (exception_value & 1) == 1) {
    constexpr const char *marker = "<exception>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker,
        "Exception value 0x{0:x} is a {1}; cannot display OCaml exception",
        exception_value,
        ((exception_value == 0) ? "null pointer" : "immediate OCaml value"));
    stream.PutCString(marker);
    return false;
  }

  auto info_opt = ExtractExceptionInfo(exception_value, process_sp);

  if (!info_opt) {
    constexpr const char *marker = "<exception>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Failed to extract OCaml exception payload at 0x{0:x}",
        exception_value);
    stream.PutCString(marker);
    return false;
  }

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

static bool FormatMember(Stream &stream, const OxCamlMember &member,
                         DataExtractor &data, lldb::ProcessSP process_sp,
                         const ExecutionContextRef &exe_ctx_ref) {
  if (member.IsBitField()) {
    lldb::offset_t offset = member.data_member_location;

    if (offset + sizeof(uint64_t) > data.GetByteSize()) {
      constexpr const char *marker = "<member>";
      OXCAML_EXPLAIN_ERROR_MARKER(marker,
                                  "Not enough data ({0} bytes) to read "
                                  "bit-field '{1}' starting at offset {2}",
                                  data.GetByteSize(),
                                  member.name.value_or("<unnamed>"),
                                  (unsigned)offset);
      stream.PutCString(marker);
      return false;
    }
    uint64_t full_value = data.GetU64(&offset);

    uint64_t bit_mask = (1ULL << member.bit_size.value()) - 1;
    uint64_t extracted = (full_value >> member.bit_offset.value()) & bit_mask;

    // CR sspies: This assumes the entire bit-field fits within the 8 bytes read
    // above. Bit-fields that span multiple words will require a different
    // extraction path.
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

enum VariantKind {
  SingleEntryVariant, // 1 member, no name
  TupleVariant,       // >1 members, no names
  RecordVariant       // at least 1 named member
};

// Format variant part with generic square bracket format: Name[mem1 = val1;
// mem2 = val2]
static bool FormatVariantPartGeneric(
    Stream &stream, const OxCamlVariantPart &variant_part, DataExtractor &data,
    lldb::ProcessSP process_sp, const ExecutionContextRef &exe_ctx_ref,
    uint64_t discr_value, const std::vector<OxCamlMember> &members) {
  std::string discr_name = "Unknown";
  const auto &discriminator = variant_part.GetDiscriminator();
  if (auto *enum_type =
          dynamic_cast<OxCamlEnumType *>(discriminator.GetType())) {
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
      if (members[i].name.has_value()) {
        stream.Printf("%s = ", members[i].name.value().c_str());
      }
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
  auto *enum_type = dynamic_cast<OxCamlEnumType *>(discriminator.GetType());
  if (!enum_type) {
    constexpr const char *marker = "<variant>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Variant discriminator type '{0}' is not an OCaml enum",
        discriminator.GetType()->GetDisplayName());
    stream.PutCString(marker);
    return false;
  }

  auto name_opt =
      enum_type->GetEnumeratorName(static_cast<int64_t>(discr_value));
  if (!name_opt.has_value()) {
    constexpr const char *marker = "<variant>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker,
        "Variant discriminator value 0x{0:x} has no matching enumerator",
        discr_value);
    stream.PutCString(marker);
    return false;
  }

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
        constexpr const char *marker = "<unavailable>";
        OXCAML_EXPLAIN_ERROR_MARKER(
            marker,
            "Variant member index {0} lacks a DW_AT_name; using placeholder",
            i);
        stream.PutCString(marker);
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
  uint64_t discr_value = ReadDiscriminatorValue(variant_part, data);

  auto active_variant = variant_part.GetActiveVariant(discr_value);
  if (!active_variant.has_value()) {
    stream.Printf("UnknownVariant[]");
    return false;
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

static bool FormatArray(Stream &stream, OxCamlArrayType *array_type,
                        DataExtractor &data, lldb::ProcessSP process_sp,
                        const ExecutionContextRef &exe_ctx_ref) {
  Log *log = GetLog(OxCamlLog::Formatting);

  lldb::offset_t offset = 0;
  uint64_t array_ptr = data.GetU64(&offset);

  if (offset == 0) {
    constexpr const char *marker = "<array>";
    OXCAML_EXPLAIN_ERROR_MARKER(marker,
                                "Failed to read OCaml array pointer from value "
                                "data ({0} bytes available)",
                                data.GetByteSize());
    stream.PutCString(marker);
    return false;
  }

  LLDB_LOG(log, "FormatArray: array pointer = 0x{0:x}", array_ptr);

  // Check for null or immediate values (not actual pointers)
  if (array_ptr == 0 || helpers::value::IsImmediate(array_ptr)) {
    std::string marker_storage =
        llvm::formatv("<array@0x{0:x}>", array_ptr).str();
    const char *marker = marker_storage.c_str();
    const char *reason =
        array_ptr == 0 ? "null pointer" : "immediate OCaml value";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Array pointer 0x{0:x} is a {1}; skipping dereference",
        array_ptr, reason);
    stream.PutCString(marker);
    return true;
  }

  Status error;
  lldb::addr_t header_addr = header::GetHeaderAddress(array_ptr);
  uint64_t header = process_sp->ReadUnsignedIntegerFromMemory(
      header_addr, constants::WORD_SIZE, 0, error);

  if (error.Fail()) {
    LLDB_LOG(log, "FormatArray: Failed to read array header at 0x{0:x}",
             header_addr);
    std::string marker_storage =
        llvm::formatv("<array@0x{0:x}>", array_ptr).str();
    const char *marker = marker_storage.c_str();
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker,
        "Failed to read OCaml array header at 0x{0:x} for pointer 0x{1:x}: {2}",
        header_addr, array_ptr, error.AsCString());
    stream.PutCString(marker);
    return true;
  }

  uint8_t tag;
  uint64_t wosize;
  uint8_t reserved;
  helpers::header::ParseHeader(header, tag, wosize, reserved);

  LLDB_LOG(log, "FormatArray: wosize = {0}, tag = {1}", wosize, tag);

  stream.Printf("[| ");

  OxCamlType *element_type = array_type->GetElementType();
  uint64_t stride = array_type->GetStride();

  // OCaml float arrays (tag 254) store unboxed 8-byte IEEE 754 doubles
  // wosize is the number of doubles (1 word = 1 double on 64-bit platforms)
  if (tag == constants::DOUBLE_ARRAY_TAG) {
    for (uint64_t i = 0; i < wosize; i++) {
      if (i > 0)
        stream.Printf("; ");

      uint64_t element_address = array_ptr + (i * constants::DOUBLE_SIZE);
      uint64_t element_value = process_sp->ReadUnsignedIntegerFromMemory(
          element_address, constants::DOUBLE_SIZE, 0, error);

      if (error.Fail()) {
        constexpr const char *marker = "<float>";
        OXCAML_EXPLAIN_ERROR_MARKER(
            marker, "Failed to read float array element {0} at 0x{1:x}", i,
            element_address);
        stream.PutCString(marker);
        error.Clear();
        continue;
      }

      DataExtractor element_data(&element_value, constants::DOUBLE_SIZE,
                                 data.GetByteOrder(),
                                 data.GetAddressByteSize());
      OxCamlUnboxedBaseType synthetic_float(0, std::nullopt,
                                            constants::DOUBLE_SIZE,
                                            OxCamlUnboxedBaseType::Float);
      FormatUnboxedBase(stream, &synthetic_float, element_data, process_sp);
    }
  } else {
    // Regular arrays: calculate number of elements from wosize and stride
    // wosize is number of words, total bytes = wosize * WORD_SIZE
    uint64_t total_bytes = wosize * helpers::constants::WORD_SIZE;
    uint64_t num_elements = total_bytes / stride;

    for (uint64_t i = 0; i < num_elements; i++) {
      if (i > 0)
        stream.Printf("; ");

      uint64_t element_address = array_ptr + (i * stride);

      std::vector<uint8_t> buffer(stride);
      size_t bytes_read =
          process_sp->ReadMemory(element_address, buffer.data(), stride, error);

      if (bytes_read != stride || error.Fail()) {
        constexpr const char *marker = "<element>";
        OXCAML_EXPLAIN_ERROR_MARKER(marker,
                                    "Failed to read array element {0} at "
                                    "0x{1:x}; requested {2} bytes, read {3}",
                                    i, element_address, stride, bytes_read);
        stream.PutCString(marker);
        error.Clear();
        continue;
      }

      DataExtractor element_data(buffer.data(), stride, data.GetByteOrder(),
                                 data.GetAddressByteSize());
      FormatValue(stream, element_type, element_data, process_sp, exe_ctx_ref);
    }
  }

  stream.Printf(" |]");
  return true;
}

// Special case for OCaml variants:
// - Structures with no direct members and exactly one variant part
//   are formatted without braces (just the variant content directly)
// - This represents the typical OCaml variant structure encoding
static bool FormatStructure(Stream &stream, OxCamlStructureType *struct_type,
                            DataExtractor &data, lldb::ProcessSP process_sp,
                            const ExecutionContextRef &exe_ctx_ref) {
  const auto &members = struct_type->GetMembers();
  const auto &variant_parts = struct_type->GetVariantParts();

  // Special case: OCaml variant (no direct members, exactly one variant part)
  // Format the variant content directly without structure braces using OCaml
  // formatting
  if (members.empty() && variant_parts.size() == 1) {
    return FormatVariantPart(stream, variant_parts[0], data, process_sp,
                             exe_ctx_ref, true);
  }

  // Special case: OCaml exceptions (union-style structure with "exn" and "raw"
  // members) Exceptions have both members at the same offset - format only the
  // "raw" member
  if (members.size() == 2 && variant_parts.empty()) {
    // Check if this is the exception pattern: two members named "exn" and "raw"
    // at offset 0
    if (members[0].name.has_value() && members[1].name.has_value() &&
        members[0].data_member_location == 0 &&
        members[1].data_member_location == 0) {
      const std::string &name0 = members[0].name.value();
      const std::string &name1 = members[1].name.value();
      if ((name0 == "exn" && name1 == "raw") ||
          (name0 == "raw" && name1 == "exn")) {
        return FormatException(stream, data, process_sp, exe_ctx_ref);
      }
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

bool lldb_private::formatters::oxcaml::OxCamlValue_SummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &options) {
  DataExtractor data;
  Status error;
  valobj.GetData(data, error);

  if (!error.Success()) {
    constexpr const char *marker = "<unavailable>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "ValueObject data extraction failed: {0}", error.AsCString());
    stream.PutCString(marker);
    return true;
  }

  CompilerType compiler_type = valobj.GetCompilerType();
  OxCamlType *type = ExtractOxCamlType(compiler_type);

  lldb::ProcessSP process_sp = valobj.GetProcessSP();

  const ExecutionContextRef &exe_ctx_ref = valobj.GetExecutionContextRef();

  return FormatValue(stream, type, data, process_sp, exe_ctx_ref);
}
