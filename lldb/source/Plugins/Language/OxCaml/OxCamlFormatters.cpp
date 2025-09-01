//===-- OxCamlFormatters.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OxCamlFormatters.h"
#include "LogChannelOxCaml.h"
#include "lldb/ValueObject/ValueObject.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/Status.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Utility/Log.h"
#include "Plugins/TypeSystem/OxCaml/TypeSystemOxCaml.h"
#include <cinttypes>

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters;
using namespace lldb_private::formatters::oxcaml;

// Forward declarations
static bool FormatValue(Stream &stream, OxCamlType* type, uint64_t value, lldb::ProcessSP process_sp);
static bool FormatBase(Stream &stream, uint64_t value);
static bool FormatEnum(Stream &stream, OxCamlEnumType* enum_type, uint64_t value);
static bool FormatPointer(Stream &stream, OxCamlPointerType* ptr_type, uint64_t value, lldb::ProcessSP process_sp);
static bool FormatTypedef(Stream &stream, OxCamlTypedefType* typedef_type, uint64_t value, lldb::ProcessSP process_sp);
static bool FormatStructure(Stream &stream, OxCamlStructureType* struct_type, uint64_t value, lldb::ProcessSP process_sp);

// Format base/immediate values
static bool FormatBase(Stream &stream, uint64_t value) {
  if (value & 1) {
    // Immediate integer - shift and print
    int64_t int_val = ((int64_t)value) >> 1;
    stream.Printf("%" PRId64, int_val);
  } else if (value == 0) {
    // Unit value
    stream.Printf("()");
  } else {
    // Pointer (shouldn't happen for base, but be safe)
    stream.Printf("<0x%" PRIx64 ">", value);
  }
  return true;
}

// Format enum values
static bool FormatEnum(Stream &stream, OxCamlEnumType* enum_type, uint64_t value) {
  auto name_opt = enum_type->GetEnumeratorName(value);
  if (name_opt.has_value()) {
    stream.PutCString(name_opt.value());
  } else {
    // Fallback to base formatting
    FormatBase(stream, value);
  }
  return true;
}

// Format pointer values by dereferencing
static bool FormatPointer(Stream &stream, OxCamlPointerType* ptr_type, 
                         uint64_t value, lldb::ProcessSP process_sp) {
  if (value == 0) {
    stream.Printf("()");  // unit
    return true;
  }
  
  if (value & 1) {
    stream.Printf("<invalid pointer: 0x%" PRIx64 ">", value);
    return true;
  }
  
  OxCamlType* pointed_to = ptr_type->GetPointedToType();
  if (!pointed_to || !process_sp) {
    stream.Printf("<0x%" PRIx64 ">", value);
    return true;
  }
  
  // Read the pointed-to value
  uint64_t deref_value;
  Status error;
  size_t bytes_read = process_sp->ReadMemory(value, &deref_value, 8, error);
  
  if (bytes_read == 8 && error.Success()) {
    // Recursively format the dereferenced value
    return FormatValue(stream, pointed_to, deref_value, process_sp);
  }
  
  stream.Printf("<0x%" PRIx64 ">", value);
  return true;
}

// Format typedef by looking through to underlying type
static bool FormatTypedef(Stream &stream, OxCamlTypedefType* typedef_type,
                         uint64_t value, lldb::ProcessSP process_sp) {
  // Simply look through to underlying type
  return FormatValue(stream, typedef_type->GetUnderlyingType(), value, process_sp);
}

// Format structure values (simplified - placeholders for now)
static bool FormatStructure(Stream &stream, OxCamlStructureType* struct_type,
                           uint64_t value, lldb::ProcessSP process_sp) {
  const auto& members = struct_type->GetMembers();
  
  if (struct_type->IsTuple()) {
    // Print tuple: (_, ..., _)
    stream.Printf("(");
    for (size_t i = 0; i < members.size(); ++i) {
      if (i > 0) stream.Printf(", ");
      stream.Printf("_");
    }
    stream.Printf(")");
  } else {
    // Print record: {field1: _; ...; fieldN: _}
    stream.Printf("{");
    for (size_t i = 0; i < members.size(); ++i) {
      if (i > 0) stream.Printf("; ");
      if (members[i].name.has_value()) {
        stream.Printf("%s: _", members[i].name.value().c_str());
      } else {
        stream.Printf("_: _");  // Shouldn't happen for records
      }
    }
    stream.Printf("}");
  }
  return true;
}

// Main dispatcher function
static bool FormatValue(Stream &stream, OxCamlType* type, uint64_t value, lldb::ProcessSP process_sp) {
  if (!type) {
    return FormatBase(stream, value);  // Fallback to base formatting
  }
  
  switch (type->GetKind()) {
    case OxCamlType::Base:
      return FormatBase(stream, value);
    case OxCamlType::Enum:
      return FormatEnum(stream, static_cast<OxCamlEnumType*>(type), value);
    case OxCamlType::Pointer:
      return FormatPointer(stream, static_cast<OxCamlPointerType*>(type), value, process_sp);
    case OxCamlType::Typedef:
      return FormatTypedef(stream, static_cast<OxCamlTypedefType*>(type), value, process_sp);
    case OxCamlType::Structure:
      return FormatStructure(stream, static_cast<OxCamlStructureType*>(type), value, process_sp);
    default:
      return FormatBase(stream, value);  // Fallback
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
  
  // Read the value
  lldb::offset_t offset = 0;
  uint64_t value = data.GetU64(&offset);
  
  // Get type and format using the new dispatcher
  CompilerType compiler_type = valobj.GetCompilerType();
  OxCamlType* type = nullptr;
  if (compiler_type.IsValid()) {
    type = static_cast<OxCamlType*>(compiler_type.GetOpaqueQualType());
  }
  
  lldb::ProcessSP process_sp = valobj.GetProcessSP();
  return FormatValue(stream, type, value, process_sp);
}