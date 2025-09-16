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
#include "OxCamlFormatHelpers.h"
#include "OxCamlValueFormatters.h"
#include "LogChannelOxCaml.h"
#include "lldb/ValueObject/ValueObject.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/Status.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Reference.h"
#include "Plugins/TypeSystem/OxCaml/TypeSystemOxCaml.h"
#include <cassert>
#include <cinttypes>
#include <cstring>
#include <vector>

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters;
using namespace lldb_private::formatters::oxcaml;

// Forward declarations
static bool FormatValue(Stream &stream, OxCamlType* type, DataExtractor& data, lldb::ProcessSP process_sp, const ExecutionContextRef &exe_ctx_ref);
static bool FormatUnboxedBase(Stream &stream, OxCamlUnboxedBaseType* unboxed_type, DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatFallback(Stream &stream, OxCamlType* type, DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatEnum(Stream &stream, OxCamlEnumType* enum_type, DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatPointer(Stream &stream, OxCamlPointerType* ptr_type, DataExtractor& data, lldb::ProcessSP process_sp, const ExecutionContextRef &exe_ctx_ref);
static bool FormatTypedef(Stream &stream, OxCamlTypedefType* typedef_type, DataExtractor& data, lldb::ProcessSP process_sp, const ExecutionContextRef &exe_ctx_ref);
static bool FormatStructure(Stream &stream, OxCamlStructureType* struct_type, DataExtractor& data, lldb::ProcessSP process_sp, const ExecutionContextRef &exe_ctx_ref);
static bool FormatPlaceholder(Stream &stream, OxCamlPlaceholderType* placeholder_type, DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatUnknown(Stream &stream, OxCamlUnknownType* unknown_type, DataExtractor& data, lldb::ProcessSP process_sp);

// Helper function to safely extract OxCamlType from CompilerType
static OxCamlType* ExtractOxCamlType(const CompilerType& compiler_type) {
  if (!compiler_type.IsValid()) {
    llvm::report_fatal_error("Invalid CompilerType in ExtractOxCamlType");
  }

  auto* type_ref = static_cast<Reference<OxCamlType>*>(compiler_type.GetOpaqueQualType());
  if (!type_ref) {
    llvm::report_fatal_error("Invalid type reference in ExtractOxCamlType");
  }

  return type_ref->get();
}

// CR sspies: There are a lot of helper functions in this file that should be factored out.
// They have to do with the formatting of variants, which can be a bit subtle.


// Helper to read discriminator value from data
static uint64_t ReadDiscriminatorValue(const OxCamlVariantPart& variant_part, DataExtractor& data) {
  const auto& discriminator = variant_part.GetDiscriminator();
  lldb::offset_t discr_offset = discriminator.data_member_location;

  // Get byte size from the discriminator's type
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

// Calculate minimum size needed to read all discriminators in a structure
static uint64_t CalculateMinimumSizeForDiscriminators(OxCamlStructureType* struct_type) {
  uint64_t max_discriminator_end = 0;

  const auto& variant_parts = struct_type->GetVariantParts();
  for (const auto& variant_part : variant_parts) {
    const auto& discr = variant_part.GetDiscriminator();
    uint64_t discr_end = discr.data_member_location + discr.GetType()->GetByteSize();
    max_discriminator_end = std::max(max_discriminator_end, discr_end);
  }

  return max_discriminator_end;
}

// Helper to find all active variants in a structure based on discriminator values
static std::vector<const OxCamlVariantPart::Variant*>
FindActiveVariantsInStructure(OxCamlStructureType* struct_type, DataExtractor& data) {
  std::vector<const OxCamlVariantPart::Variant*> active_variants;

  const auto& variant_parts = struct_type->GetVariantParts();
  for (const auto& variant_part : variant_parts) {
    uint64_t discr_value = ReadDiscriminatorValue(variant_part, data);

    // Find the active variant
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
// @param type The OxCaml type to analyze
// @param data The data extractor containing the heap block data (required)
// @return Estimated allocation size in bytes
static uint64_t EstimatePointerAllocationSize(OxCamlType* type, DataExtractor& data) {
  if (!type) {
    // Error: This function requires a valid type
    return 0;
  }

  // Follow typedefs to the actual structure
  // CR sspies: This is being conservative and probably not necessary.
  while (type->GetKind() == OxCamlType::Typedef) {
    type = static_cast<OxCamlTypedefType*>(type)->GetUnderlyingType();
  }

  if (type->GetKind() != OxCamlType::Structure) {
    return type->GetByteSize(); // Use the type's reported size
  }

  auto* struct_type = static_cast<OxCamlStructureType*>(type);
  uint64_t max_end_offset = type->GetByteSize(); // Start with base type size

  // Check all regular members
  const auto& members = struct_type->GetMembers();
  for (const auto& member : members) {
    // CR sspies: If the size that is returned here is not exact (e.g., as in
    // the case of structures, then our estimate here is off). That's why it is
    // not exact.
    uint64_t member_end = member.data_member_location + member.GetType()->GetByteSize();
    max_end_offset = std::max(max_end_offset, member_end);
  }

  // Calculate based on only active variants (requires data)
  auto active_variants = FindActiveVariantsInStructure(struct_type, data);
  for (const auto* variant : active_variants) {
    for (const auto& member : variant->members) {
      uint64_t member_end = member.data_member_location + member.GetType()->GetByteSize();
      max_end_offset = std::max(max_end_offset, member_end);
    }
  }

  return max_end_offset;
}

// Format fallback - print raw bytes as hex
static bool FormatFallback(Stream &stream, OxCamlType* type, DataExtractor& data, lldb::ProcessSP process_sp) {
  size_t byte_size = data.GetByteSize();
  if (byte_size == 0) {
    stream.Printf("<no data>");
    return true;
  }

  // Print raw bytes in hex
  stream.Printf("<");
  for (size_t i = 0; i < byte_size; ++i) {
    if (i > 0) stream.Printf(" ");
    lldb::offset_t offset = i;
    uint8_t byte = data.GetU8(&offset);
    stream.Printf("%02x", byte);
  }
  stream.Printf(">");
  return true;
}

// Format placeholder types during recursive parsing
static bool FormatPlaceholder(Stream &stream, OxCamlPlaceholderType* placeholder_type, DataExtractor& data, lldb::ProcessSP process_sp) {
  Log *log = GetLog(OxCamlLog::Formatting);
  LLDB_LOG(log, "WARNING: FormatPlaceholder called for DIE 0x{0:x16} - placeholder not resolved during parsing!",
           placeholder_type->GetDieId());

  // Display a clear indication that this type is still being parsed
  stream.Printf("<resolving>");
  return true;
}

// Format unknown DWARF types
static bool FormatUnknown(Stream &stream, OxCamlUnknownType* unknown_type, DataExtractor& data, lldb::ProcessSP process_sp) {
  Log *log = GetLog(OxCamlLog::Formatting);
  LLDB_LOG(log, "FormatUnknown called for DIE 0x{0:x16}, DWARF tag 0x{1:x}",
           unknown_type->GetDieId(), unknown_type->GetDwarfTag());

  // Display the DWARF tag information to help with debugging
  stream.Printf("<unknown DWARF tag 0x%x>", unknown_type->GetDwarfTag());
  return true;
}


// Helper to convert byte size to FloatSize enum
static std::optional<oxcaml::helpers::FloatSize> ByteSizeToFloatSize(uint64_t byte_size) {
  switch (byte_size) {
    case 2: return oxcaml::helpers::FloatSize::Half;    // float16#
    case 4: return oxcaml::helpers::FloatSize::Single;  // float32#
    case 8: return oxcaml::helpers::FloatSize::Double;  // float#
    default: return std::nullopt;
  }
}

// Helper to get suffix for signed integers
static std::string GetSignedIntegerSuffix(uint64_t byte_size) {
  switch (byte_size) {
    case 1: return "";      // int8# (no standard suffix)
    case 2: return "";      // int16# (no standard suffix)
    case 4: return "l";     // int32# -> #42l
    case 8: return "L";     // int64# -> #42L
    default: return "";     // fallback
  }
}

// Helper to get suffix for unsigned integers
// Note: Unsigned suffixes (ul, UL) don't actually exist in OCaml,
// but we use them here for clarity in the debugger
static std::string GetUnsignedIntegerSuffix(uint64_t byte_size) {
  switch (byte_size) {
    case 1: return "";      // uint8# (no standard suffix)
    case 2: return "";      // uint16# (no standard suffix)
    case 4: return "ul";    // uint32# -> #42ul
    case 8: return "UL";    // uint64# -> #42UL
    default: return "";     // fallback
  }
}


// Format unboxed base types (float#, int32#, etc.)
static bool FormatUnboxedBase(Stream &stream, OxCamlUnboxedBaseType* unboxed_type, DataExtractor& data, lldb::ProcessSP process_sp) {
  uint64_t byte_size = unboxed_type->GetByteSize();
  OxCamlUnboxedBaseType::BaseKind kind = unboxed_type->GetBaseKind();

  // Early exit for invalid byte sizes
  if (byte_size == 0) {
    stream.PutCString("<0 byte base type>");
    return true;
  }

  // Dispatch to specific formatters
  lldb::offset_t offset = 0;
  switch (kind) {
    case OxCamlUnboxedBaseType::Signed: {
      std::optional<llvm::APInt> apint = oxcaml::helpers::ExtractAPInt(data, &offset, byte_size);
      if (!apint) { llvm::report_fatal_error("Failed to extract signed integer"); }
      std::string suffix = GetSignedIntegerSuffix(byte_size);
      oxcaml::helpers::FormatAPInt(&stream, *apint, true, "#", suffix);
      return true;
    }
    case OxCamlUnboxedBaseType::Unsigned: {
      std::optional<llvm::APInt> apint = oxcaml::helpers::ExtractAPInt(data, &offset, byte_size);
      if (!apint) { llvm::report_fatal_error("Failed to extract unsigned integer"); }
      std::string suffix = GetUnsignedIntegerSuffix(byte_size);
      oxcaml::helpers::FormatAPInt(&stream, *apint, false, "#", suffix);
      return true;
    }
    case OxCamlUnboxedBaseType::Float: {
      std::optional<oxcaml::helpers::FloatSize> float_size = ByteSizeToFloatSize(byte_size);
      if (!float_size) {
        stream.Printf("<%" PRIu64 "-byte float>", byte_size);
        return true;
      }
      std::optional<llvm::APFloat> apfloat = oxcaml::helpers::ExtractAPFloat(data, &offset, *float_size);
      if (!apfloat) { llvm::report_fatal_error("Failed to extract float"); }
      std::string suffix = (*float_size == oxcaml::helpers::FloatSize::Single) ? "s" : "";
      oxcaml::helpers::FormatAPFloat(&stream, *apfloat, std::nullopt, "#", suffix);
      return true;
    }
  }

  llvm_unreachable("Unknown unboxed base type kind");
}

// Format enum values
static bool FormatEnum(Stream &stream, OxCamlEnumType* enum_type, DataExtractor& data, lldb::ProcessSP process_sp) {
  assert(enum_type->GetByteSize() == 8 && "OCaml enum types must be 8 bytes");

  lldb::offset_t offset = 0;
  uint64_t value = data.GetU64(&offset);

  auto name_opt = enum_type->GetEnumeratorName(value);
  if (name_opt.has_value()) {
    stream.PutCString(name_opt.value());
  } else {
    // Fallback: log and use fallback formatter
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log, "FormatEnum: Enumerator not found for value 0x{0:x}, using fallback formatting", value);
    return FormatFallback(stream, enum_type, data, process_sp);
  }
  return true;
}

// Format pointer values by dereferencing
static bool FormatPointer(Stream &stream, OxCamlPointerType* ptr_type,
                         DataExtractor& data, lldb::ProcessSP process_sp,
                         const ExecutionContextRef &exe_ctx_ref) {
  assert(ptr_type->GetByteSize() == 8 && "OCaml pointer types must be 8 bytes");

  lldb::offset_t offset = 0;
  uint64_t ptr_value = data.GetU64(&offset);

  if (ptr_value & 1) {
    stream.Printf("<invalid pointer: 0x%" PRIx64 ">", ptr_value);
    return true;
  }

  OxCamlType* pointed_to = ptr_type->GetPointedToType();
  if (!pointed_to || !process_sp) {
    stream.Printf("<0x%" PRIx64 ">", ptr_value);
    return true;
  }

  // Read the pointed-to memory into a new DataExtractor
  uint64_t size = pointed_to->GetByteSize();
  if (size == 0) {
    stream.Printf("<0x%" PRIx64 ">", ptr_value);
    return true;
  }

  // Apply base offset from the pointed-to type
  int64_t base_offset = pointed_to->GetPointerAdjustmentOffset();
  uint64_t adjusted_address = ptr_value + base_offset;

  // Check if we need two-pass approach for structures with variant parts
  uint64_t actual_size = size;

  // Follow typedefs to check if this is a structure with variant parts
  // CR sspies: This is being conservative. This is probably not necessary
  // in the case of OxCaml. This can also probably be factored out into a nice
  // recursive structure that avoids the while loop here.
  OxCamlType* resolved_type = pointed_to;
  while (resolved_type->GetKind() == OxCamlType::Typedef) {
    resolved_type = static_cast<OxCamlTypedefType*>(resolved_type)->GetUnderlyingType();
  }

  if (resolved_type->GetKind() == OxCamlType::Structure) {
    auto* struct_type = static_cast<OxCamlStructureType*>(resolved_type);
    const auto& variant_parts = struct_type->GetVariantParts();

    if (!variant_parts.empty()) {
      // Two-pass approach for structures with variant parts:
      // 1. Read enough data to analyze all discriminators
      // 2. Calculate precise size based on active variants

      // First pass: Calculate minimum size to read all discriminators
      uint64_t min_discriminator_size = CalculateMinimumSizeForDiscriminators(struct_type);
      uint64_t temp_read_size = std::max(min_discriminator_size, size);

      Status error;
      std::vector<uint8_t> temp_buffer(temp_read_size);
      size_t bytes_read = process_sp->ReadMemory(adjusted_address, temp_buffer.data(), temp_buffer.size(), error);

      if (bytes_read >= min_discriminator_size) {
        // Create DataExtractor from the read data for variant analysis
        DataExtractor heap_data(temp_buffer.data(), bytes_read, data.GetByteOrder(), data.GetAddressByteSize());

        // Second pass: Calculate precise size based on active variants
        uint64_t estimated_size = EstimatePointerAllocationSize(pointed_to, heap_data);
        actual_size = estimated_size;
      } else {
        // Fallback to base structure size if we can't read enough for discriminators
        actual_size = size;
      }
    }
  }

  // Log when applying non-zero offset
  if (base_offset != 0) {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log, "FormatPointer: Applying base offset {0} to pointer 0x{1:x}, adjusted address: 0x{2:x}, reading {3} bytes",
             base_offset, ptr_value, adjusted_address, actual_size);
  }

  // Create buffer and read memory from adjusted address
  std::vector<uint8_t> buffer(actual_size);
  Status error;
  size_t bytes_read = process_sp->ReadMemory(adjusted_address, buffer.data(), actual_size, error);

  if (bytes_read != actual_size || !error.Success()) {
    stream.Printf("<0x%" PRIx64 ">", ptr_value);
    return true;
  }

  // Create DataExtractor with the dereferenced data
  DataExtractor pointed_data(buffer.data(), actual_size,
                             data.GetByteOrder(),
                             data.GetAddressByteSize());

  // Recursively format with the new DataExtractor
  return FormatValue(stream, pointed_to, pointed_data, process_sp, exe_ctx_ref);
}

// Format typedef by looking through to underlying type
static bool FormatTypedef(Stream &stream, OxCamlTypedefType* typedef_type,
                         DataExtractor& data, lldb::ProcessSP process_sp,
                         const ExecutionContextRef &exe_ctx_ref) {
  return FormatValue(stream, typedef_type->GetUnderlyingType(), data, process_sp, exe_ctx_ref);
}

// Format a single member (regular or bit field)
static bool FormatMember(Stream &stream, const OxCamlMember& member, DataExtractor& data, lldb::ProcessSP process_sp, const ExecutionContextRef &exe_ctx_ref) {
  // Handle bit fields by extracting specific bits
  if (member.IsBitField()) {
    lldb::offset_t offset = member.data_member_location;
    uint64_t full_value = data.GetU64(&offset);

    if (full_value == UINT64_MAX) {
      // Failed to read value
      stream.Printf("<invalid>");
      return false;
    }

    // Extract the specific bits
    uint64_t bit_mask = (1ULL << member.bit_size.value()) - 1;
    uint64_t extracted = (full_value >> member.bit_offset.value()) & bit_mask;

    // Create DataExtractor with extracted value
    DataExtractor bit_data(&extracted, sizeof(extracted),
                           data.GetByteOrder(),
                           data.GetAddressByteSize());

    return FormatValue(stream, member.GetType(), bit_data, process_sp, exe_ctx_ref);
  }

  // Regular member - read from data_member_location
  DataExtractor member_data(data, member.data_member_location, member.GetType()->GetByteSize());
  return FormatValue(stream, member.GetType(), member_data, process_sp, exe_ctx_ref);
}

// Enum to classify variant argument types for OCaml formatting
enum VariantKind {
  SingleEntryVariant,  // 1 member, no name
  TupleVariant,        // >1 members, no names
  RecordVariant        // at least 1 named member
};

// Format variant part with generic square bracket format: Name[mem1 = val1; mem2 = val2]
static bool FormatVariantPartGeneric(Stream &stream, const OxCamlVariantPart& variant_part,
                                     DataExtractor& data, lldb::ProcessSP process_sp,
                                     const ExecutionContextRef &exe_ctx_ref,
                                     uint64_t discr_value, const std::vector<OxCamlMember>& members) {
  // Try to get discriminator name if it's an enum
  std::string discr_name = "Unknown";
  const auto& discriminator = variant_part.GetDiscriminator();
  if (auto* enum_type = dynamic_cast<OxCamlEnumType*>(discriminator.GetType())) {
    auto name_opt = enum_type->GetEnumeratorName(static_cast<int64_t>(discr_value));
    if (name_opt.has_value()) {
      discr_name = name_opt.value();
    }
  }

  // Format as DiscriminatorName[member1; member2; ...]
  stream.Printf("%s", discr_name.c_str());

  if (!members.empty()) {
    stream.Printf("[");
    for (size_t i = 0; i < members.size(); ++i) {
      if (i > 0) stream.Printf("; ");
      if (members[i].name.has_value()) {
        stream.Printf("%s = ", members[i].name.value().c_str());
      }
      FormatMember(stream, members[i], data, process_sp, exe_ctx_ref);
    }
    stream.Printf("]");
  }

  return true;
}

// Format variant part with OCaml-style formatting
static bool FormatVariantPartOxCaml(Stream &stream, const OxCamlVariantPart& variant_part,
                                    DataExtractor& data, lldb::ProcessSP process_sp,
                                    const ExecutionContextRef &exe_ctx_ref,
                                    uint64_t discr_value, const std::vector<OxCamlMember>& members) {
  // Step 1: Get constructor name from enum
  const auto& discriminator = variant_part.GetDiscriminator();
  auto* enum_type = dynamic_cast<OxCamlEnumType*>(discriminator.GetType());
  if (!enum_type) {
    stream.Printf("<Invalid Variant>");
    return false;
  }

  auto name_opt = enum_type->GetEnumeratorName(static_cast<int64_t>(discr_value));
  if (!name_opt.has_value()) {
    stream.Printf("<Invalid Variant>");
    return false;
  }

  // Print constructor name
  stream.Printf("%s", name_opt.value().c_str());

  // Step 2: Early return if no members
  if (members.empty()) {
    return true;
  }

  // Step 3: Add space before arguments
  stream.Printf(" ");

  // Step 4: Determine variant kind
  // Check if all members are unnamed
  bool all_unnamed = true;
  for (const auto& member : members) {
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
    kind = RecordVariant;  // at least one member has a name
  }

  // Step 5: Set delimiters based on kind
  const char* open_delim = "";
  const char* close_delim = "";
  const char* separator = "";

  switch (kind) {
    case SingleEntryVariant:
      // No delimiters
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

  // Print opening delimiter
  stream.Printf("%s", open_delim);

  // Step 6: Single loop to format all members
  for (size_t i = 0; i < members.size(); ++i) {
    if (i > 0) stream.Printf("%s", separator);

    // For record variants, print field name or <unavailable>
    if (kind == RecordVariant) {
      if (members[i].name.has_value()) {
        stream.Printf("%s = ", members[i].name.value().c_str());
      } else {
        stream.Printf("<unavailable> = ");
      }
    }

    FormatMember(stream, members[i], data, process_sp, exe_ctx_ref);
  }

  // Print closing delimiter
  stream.Printf("%s", close_delim);

  return true;
}

// Main variant formatting dispatcher
//
// Special handling for artificial discriminators:
// - Artificial discriminators (e.g., Pointer/Immediate) with exactly one member
//   in the active variant are displayed transparently (member content only)
// - All other cases dispatch to either OCaml or generic formatting
static bool FormatVariantPart(Stream &stream, const OxCamlVariantPart& variant_part, DataExtractor& data, lldb::ProcessSP process_sp, const ExecutionContextRef &exe_ctx_ref, bool is_ocaml_variant = false) {
  // Read discriminator value using helper function
  uint64_t discr_value = ReadDiscriminatorValue(variant_part, data);

  // Find the active variant
  auto active_variant = variant_part.GetActiveVariant(discr_value);
  if (!active_variant.has_value()) {
    stream.Printf("UnknownVariant[]");
    return false;
  }

  const auto& members = (*active_variant)->members;

  // Special case: artificial discriminator with exactly one member
  // Display the member content directly without discriminator name/brackets
  if (variant_part.HasArtificialDiscriminator() && members.size() == 1) {
    FormatMember(stream, members[0], data, process_sp, exe_ctx_ref);
    return true;
  }

  // Dispatch to appropriate formatter
  if (is_ocaml_variant) {
    return FormatVariantPartOxCaml(stream, variant_part, data, process_sp, exe_ctx_ref, discr_value, members);
  } else {
    return FormatVariantPartGeneric(stream, variant_part, data, process_sp, exe_ctx_ref, discr_value, members);
  }
}

// Format structure values using DataExtractor for members
//
// Special case for OCaml variants:
// - Structures with no direct members and exactly one variant part
//   are formatted without braces (just the variant content directly)
// - This represents the typical OCaml variant structure encoding
static bool FormatStructure(Stream &stream, OxCamlStructureType* struct_type,
                           DataExtractor& data, lldb::ProcessSP process_sp,
                           const ExecutionContextRef &exe_ctx_ref) {
  const auto& members = struct_type->GetMembers();
  const auto& variant_parts = struct_type->GetVariantParts();

  // Special case: OCaml variant (no direct members, exactly one variant part)
  // Format the variant content directly without structure braces using OCaml formatting
  if (members.empty() && variant_parts.size() == 1) {
    return FormatVariantPart(stream, variant_parts[0], data, process_sp, exe_ctx_ref, true);
  }

  // Regular case: structure with members and/or multiple variant parts
  // Determine formatting based on type
  bool is_tuple = struct_type->IsTuple();
  const char* open_delim = is_tuple ? "(" : "{ ";
  const char* close_delim = is_tuple ? ")" : " }";
  const char* separator = is_tuple ? ", " : "; ";

  stream.Printf("%s", open_delim);

  bool has_content = false;

  // Format regular members first
  for (size_t i = 0; i < members.size(); ++i) {
    if (has_content) stream.Printf("%s", separator);

    if (members[i].name.has_value()) {
      stream.Printf("%s = ", members[i].name.value().c_str());
    }

    FormatMember(stream, members[i], data, process_sp, exe_ctx_ref);
    has_content = true;
  }

  // Format variant parts
  for (size_t i = 0; i < variant_parts.size(); ++i) {
    if (has_content) stream.Printf("%s", separator);

    FormatVariantPart(stream, variant_parts[i], data, process_sp, exe_ctx_ref, false);
    has_content = true;
  }

  stream.Printf("%s", close_delim);

  return true;
}

// Main dispatcher function
static bool FormatValue(Stream &stream, OxCamlType* type, DataExtractor& data, lldb::ProcessSP process_sp, const ExecutionContextRef &exe_ctx_ref) {
  if (!type) {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log, "FormatValue: No type information available, using fallback formatter");
    return FormatFallback(stream, type, data, process_sp);
  }

  switch (type->GetKind()) {
    case OxCamlType::Value:
      return oxcaml::FormatOxCamlValue(stream, static_cast<OxCamlValueType*>(type), data, process_sp, exe_ctx_ref);
    case OxCamlType::UnboxedBase:
      return FormatUnboxedBase(stream, static_cast<OxCamlUnboxedBaseType*>(type), data, process_sp);
    case OxCamlType::Enum:
      return FormatEnum(stream, static_cast<OxCamlEnumType*>(type), data, process_sp);
    case OxCamlType::Pointer:
      return FormatPointer(stream, static_cast<OxCamlPointerType*>(type), data, process_sp, exe_ctx_ref);
    case OxCamlType::Typedef:
      return FormatTypedef(stream, static_cast<OxCamlTypedefType*>(type), data, process_sp, exe_ctx_ref);
    case OxCamlType::Structure:
      return FormatStructure(stream, static_cast<OxCamlStructureType*>(type), data, process_sp, exe_ctx_ref);
    case OxCamlType::Placeholder:
      return FormatPlaceholder(stream, static_cast<OxCamlPlaceholderType*>(type), data, process_sp);
    case OxCamlType::Unknown:
      return FormatUnknown(stream, static_cast<OxCamlUnknownType*>(type), data, process_sp);
  }
  return false;  // Add default return for completeness
}

bool lldb_private::formatters::oxcaml::OxCamlValue_SummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &options) {
  // Get raw data
  DataExtractor data;
  Status error;
  valobj.GetData(data, error);

  if (!error.Success()) {
    stream.Printf("<unavailable>");
    return true;
  }

  // Get type and format using DataExtractor directly
  CompilerType compiler_type = valobj.GetCompilerType();
  OxCamlType* type = ExtractOxCamlType(compiler_type);

  lldb::ProcessSP process_sp = valobj.GetProcessSP();
  
  // Get ExecutionContext for address resolution
  const ExecutionContextRef &exe_ctx_ref = valobj.GetExecutionContextRef();
  
  return FormatValue(stream, type, data, process_sp, exe_ctx_ref);
}
