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
#include "LogChannelOxCaml.h"
#include "lldb/Target/Process.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Status.h"
#include "llvm/Support/ErrorHandling.h"
#include <cassert>

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters::oxcaml;

/// OCaml special block tags from the runtime system.
/// These are the predefined tags for special OCaml heap blocks.
enum class OxCamlSpecialTag : uint8_t {
  Lazy_tag = 246,           ///< Lazy values
  Closure_tag = 247,        ///< Function closures
  Object_tag = 248,         ///< Object instances
  Infix_tag = 249,          ///< Infix closures
  Forward_tag = 250,        ///< Forwarding pointers (GC)
  Abstract_tag = 251,       ///< Abstract values
  String_tag = 252,         ///< String values
  Double_tag = 253,         ///< Boxed float values
  Double_array_tag = 254,   ///< Float arrays
  Custom_tag = 255          ///< Custom blocks
};

// Forward declarations of helper functions
static bool FormatOxCamlImmediate(Stream &stream, uint64_t value, 
                                  lldb::ProcessSP process_sp);
static bool FormatOxCamlPointer(Stream &stream, uint64_t value,
                                DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatOxCamlGenericBlock(Stream &stream, uint64_t value, uint8_t tag,
                                     uint64_t wosize, DataExtractor& data, 
                                     lldb::ProcessSP process_sp);
static bool FormatOxCamlLazy(Stream &stream, uint64_t value, uint64_t wosize,
                             DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatOxCamlClosure(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatOxCamlObject(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatOxCamlInfix(Stream &stream, uint64_t value, uint64_t wosize,
                              DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatOxCamlForward(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatOxCamlAbstract(Stream &stream, uint64_t value, uint64_t wosize,
                                 DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatOxCamlString(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatOxCamlDouble(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatOxCamlDoubleArray(Stream &stream, uint64_t value, uint64_t wosize,
                                    DataExtractor& data, lldb::ProcessSP process_sp);
static bool FormatOxCamlCustom(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp);

bool lldb_private::formatters::oxcaml::FormatOxCamlValue(Stream &stream,
                                                         OxCamlValueType* value_type,
                                                         DataExtractor& data,
                                                         lldb::ProcessSP process_sp) {
  assert(value_type->GetByteSize() == 8 && "OCaml value types must be 8 bytes");
  
  // Fatal error: OCaml value formatting requires a valid process for memory access
  if (!process_sp) {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log, "FATAL: FormatOxCamlValue called without valid process - this is a critical system error");
    llvm::report_fatal_error("FormatOxCamlValue called without valid process - OCaml values require memory access");
  }

  // Extract 8-byte unsigned integer from data
  lldb::offset_t offset = 0;
  uint64_t value = data.GetU64(&offset);

  if (offset == 0) {
    stream.Printf("<could not read OCaml value>");
    return false;
  }

  // Check LSB for immediate vs pointer discrimination
  if ((value & 0x1) == 1) {
    // LSB = 1: Immediate value (tagged integer, unit, etc.)
    return FormatOxCamlImmediate(stream, value, process_sp);
  } else {
    // LSB = 0: Pointer to heap block
    return FormatOxCamlPointer(stream, value, data, process_sp);
  }
}

static bool FormatOxCamlImmediate(Stream &stream, uint64_t value,
                                  lldb::ProcessSP process_sp) {
  // Placeholder: OCaml immediate value (tagged integer, unit, etc.)
  // Will decode value >> 1 for integers, handle unit (0x1), etc.
  stream.Printf("<immediate>");
  return true;
}

static bool FormatOxCamlPointer(Stream &stream, uint64_t value,
                                DataExtractor& data, lldb::ProcessSP process_sp) {
  // Check for null pointer
  if (value == 0) {
    stream.Printf("<null>");
    return true;
  }

  // Read header from offset -8 to get tag and wosize
  // Note: process_sp is guaranteed to be valid by FormatOxCamlValue
  Status error;
  uint64_t header = process_sp->ReadUnsignedIntegerFromMemory(value - 8, 8, 0, error);

  if (error.Fail()) {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log, "WARNING: Cannot read OCaml block header at 0x{0:x} - memory may be invalid or corrupted", value - 8);
    stream.Printf("<error reading block header>");
    return false;
  }

  // Extract tag (lower 8 bits) and wosize (upper bits >> 10)
  uint8_t tag = header & 0xff;
  uint64_t wosize = header >> 10;

  // Dispatch based on tag
  switch (tag) {
    case static_cast<uint8_t>(OxCamlSpecialTag::Lazy_tag):
      return FormatOxCamlLazy(stream, value, wosize, data, process_sp);
    case static_cast<uint8_t>(OxCamlSpecialTag::Closure_tag):
      return FormatOxCamlClosure(stream, value, wosize, data, process_sp);
    case static_cast<uint8_t>(OxCamlSpecialTag::Object_tag):
      return FormatOxCamlObject(stream, value, wosize, data, process_sp);
    case static_cast<uint8_t>(OxCamlSpecialTag::Infix_tag):
      return FormatOxCamlInfix(stream, value, wosize, data, process_sp);
    case static_cast<uint8_t>(OxCamlSpecialTag::Forward_tag):
      return FormatOxCamlForward(stream, value, wosize, data, process_sp);
    case static_cast<uint8_t>(OxCamlSpecialTag::Abstract_tag):
      return FormatOxCamlAbstract(stream, value, wosize, data, process_sp);
    case static_cast<uint8_t>(OxCamlSpecialTag::String_tag):
      return FormatOxCamlString(stream, value, wosize, data, process_sp);
    case static_cast<uint8_t>(OxCamlSpecialTag::Double_tag):
      return FormatOxCamlDouble(stream, value, wosize, data, process_sp);
    case static_cast<uint8_t>(OxCamlSpecialTag::Double_array_tag):
      return FormatOxCamlDoubleArray(stream, value, wosize, data, process_sp);
    case static_cast<uint8_t>(OxCamlSpecialTag::Custom_tag):
      return FormatOxCamlCustom(stream, value, wosize, data, process_sp);
    default:
      // Generic block (tag < 246)
      return FormatOxCamlGenericBlock(stream, value, tag, wosize, data, process_sp);
  }
}

static bool FormatOxCamlGenericBlock(Stream &stream, uint64_t value, uint8_t tag,
                                     uint64_t wosize, DataExtractor& data,
                                     lldb::ProcessSP process_sp) {
  // Placeholder: Generic OCaml block (variant, record, array, etc.)
  stream.Printf("<block>");
  return true;
}

static bool FormatOxCamlLazy(Stream &stream, uint64_t value, uint64_t wosize,
                             DataExtractor& data, lldb::ProcessSP process_sp) {
  // Placeholder: OCaml lazy value
  stream.Printf("<lazy>");
  return true;
}

static bool FormatOxCamlClosure(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor& data, lldb::ProcessSP process_sp) {
  // Placeholder: OCaml function closure
  stream.Printf("<closure>");
  return true;
}

static bool FormatOxCamlObject(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp) {
  // Placeholder: OCaml object instance
  stream.Printf("<object>");
  return true;
}

static bool FormatOxCamlInfix(Stream &stream, uint64_t value, uint64_t wosize,
                              DataExtractor& data, lldb::ProcessSP process_sp) {
  // Placeholder: OCaml infix closure
  stream.Printf("<infix>");
  return true;
}

static bool FormatOxCamlForward(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor& data, lldb::ProcessSP process_sp) {
  // Placeholder: OCaml forwarding pointer (GC)
  stream.Printf("<forward>");
  return true;
}

static bool FormatOxCamlAbstract(Stream &stream, uint64_t value, uint64_t wosize,
                                 DataExtractor& data, lldb::ProcessSP process_sp) {
  // Placeholder: OCaml abstract value
  stream.Printf("<abstract>");
  return true;
}

static bool FormatOxCamlString(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp) {
  // Placeholder: OCaml string value
  stream.Printf("<string>");
  return true;
}

static bool FormatOxCamlDouble(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp) {
  // Placeholder: OCaml boxed float value
  stream.Printf("<float>");
  return true;
}

static bool FormatOxCamlDoubleArray(Stream &stream, uint64_t value, uint64_t wosize,
                                    DataExtractor& data, lldb::ProcessSP process_sp) {
  // Placeholder: OCaml float array
  stream.Printf("<float array>");
  return true;
}

static bool FormatOxCamlCustom(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp) {
  // Placeholder: OCaml custom block (Int32.t, Int64.t, etc.)
  stream.Printf("<custom>");
  return true;
}
