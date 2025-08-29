//===-- OxCamlFormatters.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OxCamlFormatters.h"
#include "lldb/ValueObject/ValueObject.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/Status.h"
#include "lldb/Symbol/CompilerType.h"
#include "Plugins/TypeSystem/OxCaml/TypeSystemOxCaml.h"
#include <cinttypes>

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters;
using namespace lldb_private::formatters::oxcaml;

bool lldb_private::formatters::oxcaml::OxCamlValue_SummaryProvider(
    ValueObject &valobj, Stream &stream, const TypeSummaryOptions &options) {
  // Get the raw data directly
  DataExtractor data;
  Status error;
  uint64_t data_size = valobj.GetData(data, error);
  
  if (!error.Success() || data_size < 8) {
    stream.Printf("<unavailable>");
    return true;
  }
  
  // Read the value
  lldb::offset_t offset = 0;
  uint64_t value = data.GetU64(&offset);
  
  // Get the type information
  CompilerType compiler_type = valobj.GetCompilerType();
  if (compiler_type.IsValid()) {
    // Extract the OxCamlType from the CompilerType
    auto* oxcaml_type = static_cast<OxCamlType*>(compiler_type.GetOpaqueQualType());
    if (oxcaml_type) {
      // Resolve through typedefs to get the actual type
      while (oxcaml_type->GetKind() == OxCamlType::Typedef) {
        auto* typedef_type = static_cast<OxCamlTypedefType*>(oxcaml_type);
        oxcaml_type = typedef_type->GetUnderlyingType();
      }
      
      // Handle enum types
      if (oxcaml_type->GetKind() == OxCamlType::Enum) {
        auto* enum_type = static_cast<OxCamlEnumType*>(oxcaml_type);
        
        // Look up the enumerator name
        int64_t enum_value = static_cast<int64_t>(value);
        auto name_opt = enum_type->GetEnumeratorName(enum_value);
        if (name_opt.has_value()) {
          // Found the enumerator - show its name
          stream.PutCString(name_opt.value());
          return true;
        }
        // Fall through to show as integer if enumerator not found
      }
    }
  }
  
  // Default OCaml value interpretation
  if (value & 1) {
    // Immediate value - show as integer
    int64_t int_val = ((int64_t)value) >> 1;
    stream.Printf("%" PRId64, int_val);
  } else {
    // Pointer value
    if (value == 0) {
      stream.Printf("()"); // unit value
    } else {
      stream.Printf("<pointer: 0x%" PRIx64 ">", value);
    }
  }
  
  return true;
}