//===-- OxCamlValueFormatters.cpp --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// \file
/// This file implements formatting functions for OCaml boxed values.
///
/// Currently provides placeholder formatting. Will be expanded in the future
/// to include full OCaml runtime structure decoding using the helper functions
/// from OxCamlFormatHelpers.

#include "OxCamlValueFormatters.h"
#include "OxCamlFormatHelpers.h"
#include <cassert>

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters::oxcaml;

bool lldb_private::formatters::oxcaml::FormatOxCamlValue(Stream &stream, 
                                                         OxCamlValueType* value_type, 
                                                         DataExtractor& data, 
                                                         lldb::ProcessSP process_sp) {
  assert(value_type->GetByteSize() == 8 && "OCaml value types must be 8 bytes");
  
  // For now, just display "<value>" as requested
  // This will be expanded later to decode actual OCaml runtime structures
  // including immediate integers, heap pointers, strings, floats, variants, etc.
  stream.Printf("<value>");
  return true;
}