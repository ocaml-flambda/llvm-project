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
#include "lldb/ValueObject/ValueObject.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/Status.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Utility/Log.h"
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
static bool FormatValue(Stream &stream, OxCamlType* type, DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatBase(Stream &stream, OxCamlBaseType* base_type, DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatFallback(Stream &stream, OxCamlType* type, DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatEnum(Stream &stream, OxCamlEnumType* enum_type, DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatPointer(Stream &stream, OxCamlPointerType* ptr_type, DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatTypedef(Stream &stream, OxCamlTypedefType* typedef_type, DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatStructure(Stream &stream, OxCamlStructureType* struct_type, DataExtractor& data, lldb::ProcessSP process_sp);


// CR sspies: There are a lot of helper functions in this file that should be factored out.
// They have to do with the formatting of variants, which can be a bit subtle.


// Helper to read discriminator value from data
static uint64_t ReadDiscriminatorValue(const OxCamlVariantPart& variant_part, DataExtractor& data) {
  // Read discriminator from its specified location
  lldb::offset_t discr_offset = variant_part.GetDiscriminator().data_member_location;

  // Read discriminator value based on enum type byte size
  uint64_t discriminator_byte_size = variant_part.GetDiscriminator().enum_type->GetByteSize();
  uint64_t value;

  if (discriminator_byte_size == 1) {
    value = data.GetU8(&discr_offset);
  } else {
    value = data.GetU64(&discr_offset);
  }

  // Extract discriminator bits based on bit offset and size
  uint64_t bit_offset = variant_part.GetDiscriminator().bit_offset;
  uint64_t bit_size = variant_part.GetDiscriminator().bit_size;
  uint64_t discr_mask = (1ULL << bit_size) - 1;
  uint64_t discr_value = (value >> bit_offset) & discr_mask;

  return discr_value;
}

// Calculate minimum size needed to read all discriminators in a structure
static uint64_t CalculateMinimumSizeForDiscriminators(OxCamlStructureType* struct_type) {
  uint64_t max_discriminator_end = 0;

  const auto& variant_parts = struct_type->GetVariantParts();
  for (const auto& variant_part : variant_parts) {
    const auto& discr = variant_part.GetDiscriminator();
    uint64_t discr_end = discr.data_member_location + discr.enum_type->GetByteSize();
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
    uint64_t member_end = member.data_member_location + member.type->GetByteSize();
    max_end_offset = std::max(max_end_offset, member_end);
  }

  // Calculate based on only active variants (requires data)
  auto active_variants = FindActiveVariantsInStructure(struct_type, data);
  for (const auto* variant : active_variants) {
    for (const auto& member : variant->members) {
      uint64_t member_end = member.data_member_location + member.type->GetByteSize();
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

// Format base OCaml values
static bool FormatBase(Stream &stream, OxCamlBaseType* base_type, DataExtractor& data, lldb::ProcessSP process_sp) {
  assert(base_type->GetByteSize() == 8 && "OCaml base types must be 8 bytes");

  lldb::offset_t offset = 0;
  uint64_t value = data.GetU64(&offset);

  if (value & 1) {
    // Immediate integer - shift and print
    int64_t int_val = ((int64_t)value) >> 1;
    stream.Printf("%" PRId64, int_val);
  } else {
    // Pointer value
    stream.Printf("0x%" PRIx64, value);
  }
  return true;
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
                         DataExtractor& data, lldb::ProcessSP process_sp) {
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
  return FormatValue(stream, pointed_to, pointed_data, process_sp);
}

// Format typedef by looking through to underlying type
static bool FormatTypedef(Stream &stream, OxCamlTypedefType* typedef_type,
                         DataExtractor& data, lldb::ProcessSP process_sp) {
  return FormatValue(stream, typedef_type->GetUnderlyingType(), data, process_sp);
}

// Format a single member (regular or bit field)
static bool FormatMember(Stream &stream, const OxCamlMember& member, DataExtractor& data, lldb::ProcessSP process_sp) {
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

    return FormatValue(stream, member.type, bit_data, process_sp);
  }

  // Regular member - read from data_member_location
  DataExtractor member_data(data, member.data_member_location, member.type->GetByteSize());
  return FormatValue(stream, member.type, member_data, process_sp);
}

// Format a variant part showing the active variant
static bool FormatVariantPart(Stream &stream, const OxCamlVariantPart& variant_part, DataExtractor& data, lldb::ProcessSP process_sp) {
  // Read discriminator value using helper function
  uint64_t discr_value = ReadDiscriminatorValue(variant_part, data);

  // Find the active variant
  auto active_variant = variant_part.GetActiveVariant(discr_value);
  if (!active_variant.has_value()) {
    stream.Printf("UnknownVariant[]");
    return false;
  }

  // Get discriminator name from enum
  std::string discr_name = variant_part.GetDiscriminatorName(discr_value).value_or("Unknown");

  // Format as DiscriminatorName[member1; member2; ...]
  stream.Printf("%s[", discr_name.c_str());

  const auto& members = (*active_variant)->members;
  for (size_t i = 0; i < members.size(); ++i) {
    if (i > 0) stream.Printf("; ");

    if (members[i].name.has_value()) {
      stream.Printf("%s = ", members[i].name.value().c_str());
    }

    FormatMember(stream, members[i], data, process_sp);
  }

  stream.Printf("]");
  return true;
}

// Format structure values using DataExtractor for members
static bool FormatStructure(Stream &stream, OxCamlStructureType* struct_type,
                           DataExtractor& data, lldb::ProcessSP process_sp) {
  const auto& members = struct_type->GetMembers();
  const auto& variant_parts = struct_type->GetVariantParts();

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

    FormatMember(stream, members[i], data, process_sp);
    has_content = true;
  }

  // Format variant parts
  for (size_t i = 0; i < variant_parts.size(); ++i) {
    if (has_content) stream.Printf("%s", separator);

    FormatVariantPart(stream, variant_parts[i], data, process_sp);
    has_content = true;
  }

  stream.Printf("%s", close_delim);

  return true;
}

// Main dispatcher function
static bool FormatValue(Stream &stream, OxCamlType* type, DataExtractor& data, lldb::ProcessSP process_sp) {
  if (!type) {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log, "FormatValue: No type information available, using fallback formatter");
    return FormatFallback(stream, type, data, process_sp);
  }

  switch (type->GetKind()) {
    case OxCamlType::Base:
      return FormatBase(stream, static_cast<OxCamlBaseType*>(type), data, process_sp);
    case OxCamlType::Enum:
      return FormatEnum(stream, static_cast<OxCamlEnumType*>(type), data, process_sp);
    case OxCamlType::Pointer:
      return FormatPointer(stream, static_cast<OxCamlPointerType*>(type), data, process_sp);
    case OxCamlType::Typedef:
      return FormatTypedef(stream, static_cast<OxCamlTypedefType*>(type), data, process_sp);
    case OxCamlType::Structure:
      return FormatStructure(stream, static_cast<OxCamlStructureType*>(type), data, process_sp);
  }
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
  OxCamlType* type = nullptr;
  if (compiler_type.IsValid()) {
    type = static_cast<OxCamlType*>(compiler_type.GetOpaqueQualType());
  }

  lldb::ProcessSP process_sp = valobj.GetProcessSP();
  return FormatValue(stream, type, data, process_sp);
}
