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
#include <cinttypes>
#include <cstring>
#include <vector>

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters;
using namespace lldb_private::formatters::oxcaml;

// Forward declarations
static bool FormatValue(Stream &stream, OxCamlType* type, DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatBase(Stream &stream, DataExtractor& data);
static bool FormatFallback(Stream &stream, DataExtractor& data);
static bool FormatEnum(Stream &stream, OxCamlEnumType* enum_type, DataExtractor& data);
static bool FormatPointer(Stream &stream, OxCamlPointerType* ptr_type, DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatTypedef(Stream &stream, OxCamlTypedefType* typedef_type, DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatStructure(Stream &stream, OxCamlStructureType* struct_type, DataExtractor& data, lldb::ProcessSP process_sp);

// Format fallback - just print hex value (indicates error/unknown)
static bool FormatFallback(Stream &stream, DataExtractor& data) {
  lldb::offset_t offset = 0;
  if (data.GetByteSize() >= 8) {
    uint64_t value = data.GetU64(&offset);
    stream.Printf("0x%" PRIx64, value);
  } else {
    stream.Printf("<insufficient data>");
  }
  return true;
}

// Format base OCaml values
static bool FormatBase(Stream &stream, DataExtractor& data) {
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
static bool FormatEnum(Stream &stream, OxCamlEnumType* enum_type, DataExtractor& data) {
  lldb::offset_t offset = 0;
  uint64_t value = data.GetU64(&offset);

  auto name_opt = enum_type->GetEnumeratorName(value);
  if (name_opt.has_value()) {
    stream.PutCString(name_opt.value());
  } else {
    // Fallback: log and use fallback formatter
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log, "FormatEnum: Enumerator not found for value 0x{0:x}, using fallback formatting", value);
    return FormatFallback(stream, data);
  }
  return true;
}

// Format pointer values by dereferencing
static bool FormatPointer(Stream &stream, OxCamlPointerType* ptr_type,
                         DataExtractor& data, lldb::ProcessSP process_sp) {
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

  // Create buffer and read memory
  std::vector<uint8_t> buffer(size);
  Status error;
  size_t bytes_read = process_sp->ReadMemory(ptr_value, buffer.data(), size, error);

  if (bytes_read != size || !error.Success()) {
    stream.Printf("<0x%" PRIx64 ">", ptr_value);
    return true;
  }

  // Create DataExtractor with the dereferenced data
  DataExtractor pointed_data(buffer.data(), size,
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

// Format structure values using DataExtractor for members
static bool FormatStructure(Stream &stream, OxCamlStructureType* struct_type,
                           DataExtractor& data, lldb::ProcessSP process_sp) {
  const auto& members = struct_type->GetMembers();

  if (struct_type->IsTuple()) {
    stream.Printf("(");
    for (size_t i = 0; i < members.size(); ++i) {
      if (i > 0) stream.Printf(", ");

      DataExtractor member_data(data, members[i].offset, members[i].type->GetByteSize());
      FormatValue(stream, members[i].type, member_data, process_sp);
    }
    stream.Printf(")");
  } else {
    stream.Printf("{");
    for (size_t i = 0; i < members.size(); ++i) {
      if (i > 0) stream.Printf("; ");

      if (members[i].name.has_value()) {
        stream.Printf("%s = ", members[i].name.value().c_str());
      } else {
        stream.Printf("field = ");  // Shouldn't happen for records
      }

      DataExtractor member_data(data, members[i].offset, members[i].type->GetByteSize());
      FormatValue(stream, members[i].type, member_data, process_sp);
    }
    stream.Printf("}");
  }

  return true;
}

// Main dispatcher function
static bool FormatValue(Stream &stream, OxCamlType* type, DataExtractor& data, lldb::ProcessSP process_sp) {
  if (!type) {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log, "FormatValue: No type information available, using fallback formatter");
    return FormatFallback(stream, data);
  }

  switch (type->GetKind()) {
    case OxCamlType::Base:
      return FormatBase(stream, data);
    case OxCamlType::Enum:
      return FormatEnum(stream, static_cast<OxCamlEnumType*>(type), data);
    case OxCamlType::Pointer:
      return FormatPointer(stream, static_cast<OxCamlPointerType*>(type), data, process_sp);
    case OxCamlType::Typedef:
      return FormatTypedef(stream, static_cast<OxCamlTypedefType*>(type), data, process_sp);
    case OxCamlType::Structure:
      return FormatStructure(stream, static_cast<OxCamlStructureType*>(type), data, process_sp);
    default:
      {
        Log *log = GetLog(OxCamlLog::Formatting);
        LLDB_LOG(log, "FormatValue: Unknown type kind, using fallback formatter");
        return FormatFallback(stream, data);
      }
  }
}

bool lldb_private::formatters::oxcaml::OxCamlValue_SummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &options) {
  // Get raw data
  DataExtractor data;
  Status error;
  size_t data_size = valobj.GetData(data, error);

  if (!error.Success() || data_size < 8) {
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
