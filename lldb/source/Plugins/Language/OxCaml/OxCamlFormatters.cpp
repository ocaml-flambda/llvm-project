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
  
  if (error.Success() && data_size >= 8) {
    lldb::offset_t offset = 0;
    uint64_t value = data.GetU64(&offset);
    
    // OCaml value interpretation
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
  } else {
    stream.Printf("<unavailable>");
  }
  return true;
}