//===-- OxCamlValueFormatters.cpp --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// \file
/// This file implements formatting for OCaml boxed values and immediates
/// (ocaml_value type).
///
/// Provides decoding for:
/// - Immediate values (tagged integers, etc.)
/// - Heap-allocated blocks (strings, floats, arrays, closures, etc.)
/// - Custom blocks (Int32.t, Int64.t, Nativeint.t, Bigarray, Float32)
/// - Special runtime blocks (lazy values, objects, forwarding pointers)
///
/// The formatter uses a depth-limited recursive approach to handle nested
/// structures while preventing infinite loops on cyclic data.

#include "OxCamlValueFormatters.h"
#include "LogChannelOxCaml.h"
#include "OxCamlFormatHelpers.h"
#include "OxCamlHelpers.h"
#include "lldb/Core/Address.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/ExecutionContextScope.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Status.h"
#include "lldb/Utility/StreamString.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include <cassert>
#include <map>
#include <string>

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters::oxcaml;
using namespace lldb_private::formatters::oxcaml::helpers;

// Forward declarations of helper functions
static bool FormatOxCamlImmediate(Stream &stream, uint64_t value,
                                  lldb::ProcessSP process_sp,
                                  const ExecutionContextRef &exe_ctx_ref,
                                  uint32_t depth);
static bool FormatOxCamlPointer(Stream &stream, uint64_t value,
                                DataExtractor &data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref,
                                uint32_t depth);
static bool FormatOxCamlGenericBlock(Stream &stream, uint64_t value,
                                     uint8_t tag, uint64_t scannable_wosize,
                                     uint64_t non_scannable_wosize,
                                     DataExtractor &data,
                                     lldb::ProcessSP process_sp,
                                     const ExecutionContextRef &exe_ctx_ref,
                                     uint32_t depth);
static bool FormatOxCamlLazy(Stream &stream, uint64_t value, uint64_t wosize,
                             DataExtractor &data, lldb::ProcessSP process_sp,
                             const ExecutionContextRef &exe_ctx_ref,
                             uint32_t depth);
static bool FormatOxCamlClosure(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor &data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref,
                                bool is_infix, uint32_t depth);
static bool FormatOxCamlObject(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor &data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref,
                               uint32_t depth);
static bool FormatOxCamlForward(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor &data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref,
                                uint32_t depth);
static bool FormatOxCamlAbstract(Stream &stream, uint64_t value,
                                 uint64_t wosize, DataExtractor &data,
                                 lldb::ProcessSP process_sp,
                                 const ExecutionContextRef &exe_ctx_ref,
                                 uint32_t depth);
static bool FormatOxCamlString(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor &data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref,
                               uint32_t depth);
static bool FormatOxCamlDouble(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor &data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref,
                               uint32_t depth);
static bool FormatOxCamlDoubleArray(Stream &stream, uint64_t value,
                                    uint64_t wosize, DataExtractor &data,
                                    lldb::ProcessSP process_sp,
                                    const ExecutionContextRef &exe_ctx_ref,
                                    uint32_t depth);
static bool FormatOxCamlCustom(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor &data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref,
                               uint32_t depth);

// Custom type formatter function pointer
//
// Custom formatters receive:
// - stream: Output stream for formatting
// - identifier: Custom block identifier string (e.g., "_i", "_j", "_n")
// - data_ptr: Pointer to first word AFTER the custom block header (value + 8)
// - wosize: Word size of the custom block from the header
// - process_sp: Process for additional memory reads if needed
//
// The main FormatOxCamlCustom function handles reading the custom_operations
// pointer and identifier string from the header, then passes the data pointer
// (skipping the header) to the specific formatter.
typedef bool (*CustomTypeFormatter)(Stream &stream,
                                    const std::string &identifier,
                                    uint64_t data_ptr, uint64_t wosize,
                                    lldb::ProcessSP process_sp);

// Forward declarations for custom type formatters
static bool FormatInt32Custom(Stream &stream, const std::string &identifier,
                              uint64_t data_ptr, uint64_t wosize,
                              lldb::ProcessSP process_sp);
static bool FormatInt64Custom(Stream &stream, const std::string &identifier,
                              uint64_t data_ptr, uint64_t wosize,
                              lldb::ProcessSP process_sp);
static bool FormatNativeIntCustom(Stream &stream, const std::string &identifier,
                                  uint64_t data_ptr, uint64_t wosize,
                                  lldb::ProcessSP process_sp);
static bool FormatBigarrayCustom(Stream &stream, const std::string &identifier,
                                 uint64_t data_ptr, uint64_t wosize,
                                 lldb::ProcessSP process_sp);
static bool FormatFloat32Custom(Stream &stream, const std::string &identifier,
                                uint64_t data_ptr, uint64_t wosize,
                                lldb::ProcessSP process_sp);

// Internal helper function that contains the core dispatch logic
// Used by FormatOxCamlValue and FormatOxCamlForward for recursive formatting
static bool FormatOxCamlValueInternal(Stream &stream, uint64_t value,
                                      DataExtractor &data,
                                      lldb::ProcessSP process_sp,
                                      const ExecutionContextRef &exe_ctx_ref,
                                      uint32_t depth) {
  if (helpers::value::IsImmediate(value)) {
    // Immediates don't recurse, so no depth check needed
    return FormatOxCamlImmediate(stream, value, process_sp, exe_ctx_ref, depth);
  } else {
    // Check depth limit before processing blocks (central depth check)
    if (depth <= 0) {
      Status error;
      uint64_t header_address = header::GetHeaderAddress(value);
      uint64_t header = process_sp->ReadUnsignedIntegerFromMemory(
          header_address, constants::WORD_SIZE, 0, error);
      if (error.Fail()) {
        constexpr const char *marker = "<block>";
        OXCAML_EXPLAIN_ERROR_MARKER(marker, "Header unreadable at 0x{0:x}",
                                    header_address);
        stream.PutCString(marker);
        return true;
      }
      uint8_t tag = header::ExtractTag(header);
      stream.Printf("[ tag = %u, addr = %p ]", tag, (void *)value);
      return true;
    }
    return FormatOxCamlPointer(stream, value, data, process_sp, exe_ctx_ref,
                               depth - 1);
  }
}

bool lldb_private::formatters::oxcaml::FormatOxCamlValue(
    Stream &stream, DataExtractor &data, lldb::ProcessSP process_sp,
    const ExecutionContextRef &exe_ctx_ref) {
  if (!process_sp) {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log, "FATAL: FormatOxCamlValue called without valid process - "
                  "this is an implementation error");
    llvm::report_fatal_error("FormatOxCamlValue called without valid process - "
                             "OCaml values require memory access");
  }

  lldb::offset_t offset = 0;
  uint64_t value = data.GetU64(&offset);

  if (offset == 0) {
    constexpr const char *marker = "<value>";
    OXCAML_EXPLAIN_ERROR_MARKER(marker,
                                "Failed to read OCaml value bytes (size {0})",
                                data.GetByteSize());
    stream.PutCString(marker);
    return false;
  }

  uint32_t max_depth = 5; // Default to 5 levels of depth
  lldb::TargetSP target_sp = exe_ctx_ref.GetTargetSP();
  if (target_sp) {
    auto [depth, is_default] = target_sp->GetMaximumDepthOfChildrenToDisplay();
    // Only use the setting if it's not the default (user has explicitly set it)
    // Otherwise use our OCaml-specific default of 5
    if (!is_default) {
      max_depth = depth;
    }
  }

  return FormatOxCamlValueInternal(stream, value, data, process_sp, exe_ctx_ref,
                                   max_depth);
}

static bool FormatOxCamlImmediate(Stream &stream, uint64_t value,
                                  lldb::ProcessSP process_sp,
                                  const ExecutionContextRef &exe_ctx_ref,
                                  uint32_t depth) {
  int64_t untagged_value = helpers::value::UntagImmediate(value);
  stream.Printf("%" PRId64 "%s", untagged_value,
                helpers::suffixes::TAGGED_INT_SUFFIX);
  return true;
}

static bool FormatOxCamlPointer(Stream &stream, uint64_t value,
                                DataExtractor &data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref,
                                uint32_t depth) {
  if (value == 0) {
    constexpr const char *marker = "<null>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Encountered unexpected null OCaml block pointer");
    stream.PutCString(marker);
    return true;
  }

  // Note: process_sp is guaranteed to be valid by FormatOxCamlValue
  Status error;
  lldb::addr_t header_addr = header::GetHeaderAddress(value);
  uint64_t header = process_sp->ReadUnsignedIntegerFromMemory(
      header_addr, constants::WORD_SIZE, 0, error);

  if (error.Fail()) {
    constexpr const char *marker = "<block>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Failed to read OCaml block header at 0x{0:x}: {1}",
        header_addr, error.AsCString());
    stream.PutCString(marker);
    return false;
  }

  uint8_t tag;
  uint64_t wosize;
  uint8_t reserved;
  helpers::header::ParseHeader(header, tag, wosize, reserved);

  // Dispatch based on tag; none of the special tags support mixed blocks
  switch (tag) {
  case static_cast<uint8_t>(constants::SpecialTag::Lazy_tag):
    return FormatOxCamlLazy(stream, value, wosize, data, process_sp,
                            exe_ctx_ref, depth);
  case static_cast<uint8_t>(constants::SpecialTag::Closure_tag):
    return FormatOxCamlClosure(stream, value, wosize, data, process_sp,
                               exe_ctx_ref, false, depth);
  case static_cast<uint8_t>(constants::SpecialTag::Object_tag):
    return FormatOxCamlObject(stream, value, wosize, data, process_sp,
                              exe_ctx_ref, depth);
  case static_cast<uint8_t>(constants::SpecialTag::Infix_tag):
    return FormatOxCamlClosure(stream, value, wosize, data, process_sp,
                               exe_ctx_ref, true, depth);
  case static_cast<uint8_t>(constants::SpecialTag::Forward_tag):
    return FormatOxCamlForward(stream, value, wosize, data, process_sp,
                               exe_ctx_ref, depth);
  case static_cast<uint8_t>(constants::SpecialTag::Abstract_tag):
    return FormatOxCamlAbstract(stream, value, wosize, data, process_sp,
                                exe_ctx_ref, depth);
  case static_cast<uint8_t>(constants::SpecialTag::String_tag):
    return FormatOxCamlString(stream, value, wosize, data, process_sp,
                              exe_ctx_ref, depth);
  case static_cast<uint8_t>(constants::SpecialTag::Double_tag):
    return FormatOxCamlDouble(stream, value, wosize, data, process_sp,
                              exe_ctx_ref, depth);
  case static_cast<uint8_t>(constants::SpecialTag::Double_array_tag):
    return FormatOxCamlDoubleArray(stream, value, wosize, data, process_sp,
                                   exe_ctx_ref, depth);
  case static_cast<uint8_t>(constants::SpecialTag::Custom_tag):
    return FormatOxCamlCustom(stream, value, wosize, data, process_sp,
                              exe_ctx_ref, depth);
  default: {
    // Generic block (tag < 246): for mixed blocks, calculate scannable and
    // non-scannable sizes
    uint64_t scannable_wosize =
        helpers::header::ExtractScannableWosize(reserved, wosize);
    uint64_t non_scannable_wosize =
        helpers::header::ExtractNonScannableWosize(reserved, wosize);
    return FormatOxCamlGenericBlock(stream, value, tag, scannable_wosize,
                                    non_scannable_wosize, data, process_sp,
                                    exe_ctx_ref, depth);
  }
  }
}

static bool FormatOxCamlGenericBlock(Stream &stream, uint64_t value,
                                     uint8_t tag, uint64_t scannable_wosize,
                                     uint64_t non_scannable_wosize,
                                     DataExtractor &data,
                                     lldb::ProcessSP process_sp,
                                     const ExecutionContextRef &exe_ctx_ref,
                                     uint32_t depth) {
  // Note: alternative compact form we may adopt later:
  // block(tag | field1, field2 | non-value words: N)

  if (tag > 0)
    stream.Printf("%u:[", tag);
  else
    stream.Printf("[");

  Status error;

  for (uint64_t i = 0; i < scannable_wosize; i++) {
    if (i > 0) {
      stream.Printf(", ");
    }

    uint64_t read_address = value + i * helpers::constants::WORD_SIZE;
    uint64_t field_value = process_sp->ReadUnsignedIntegerFromMemory(
        read_address, helpers::constants::WORD_SIZE, 0, error);

    if (error.Fail()) {
      Log *log = GetLog(OxCamlLog::Formatting);
      LLDB_LOG(log, "ERROR: Failed to read field {0} at address 0x{1:x}: {2}",
               i, read_address, error.AsCString());
      std::string marker_storage =
          llvm::formatv("<field@0x{0:x}>", read_address).str();
      const char *marker = marker_storage.c_str();
      OXCAML_EXPLAIN_ERROR_MARKER(
          marker, "Failed to read OCaml block field {0} at 0x{1:x}: {2}", i,
          read_address, error.AsCString());
      stream.PutCString(marker);
      error.Clear();
      continue;
    }

    DataExtractor field_data(&field_value, helpers::constants::WORD_SIZE,
                             process_sp->GetByteOrder(),
                             helpers::constants::WORD_SIZE);

    // Recursively format the field (depth already decremented by caller)
    FormatOxCamlValueInternal(stream, field_value, field_data, process_sp,
                              exe_ctx_ref, depth);
  }

  if (non_scannable_wosize > 0)
    stream.Printf(" | non-value words: %llu",
                  (unsigned long long)non_scannable_wosize);

  stream.Printf("]");
  return true;
}

static bool FormatOxCamlLazy(Stream &stream, uint64_t value, uint64_t wosize,
                             DataExtractor &data, lldb::ProcessSP process_sp,
                             const ExecutionContextRef &exe_ctx_ref,
                             uint32_t depth) {
  if (wosize == 0) {
    constexpr const char *marker = "<lazy>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Lazy block 0x{0:x}: empty payload (wosize=0)", value);
    stream.PutCString(marker);
    return true;
  }

  Status error;

  lldb::addr_t computation_ptr =
      process_sp->ReadPointerFromMemory(value, error);
  if (error.Fail()) {
    constexpr const char *marker = "<lazy>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Lazy block 0x{0:x}: computation pointer unreadable", value);
    stream.PutCString(marker);
    return true;
  }

  // Check if the lazy value is forced (has a valid pointer)
  // Unforced lazy values typically have special marker values
  if (computation_ptr == 0) {
    constexpr const char *marker = "<lazy>";
    stream.PutCString(marker);
    return true;
  }

  uint64_t computation_value = computation_ptr;
  DataExtractor pointed_data(&computation_value, helpers::constants::WORD_SIZE,
                             process_sp->GetByteOrder(),
                             helpers::constants::WORD_SIZE);

  // Format using OCaml syntax: "Lazy.from_val contents"
  stream.Printf("Lazy.from_val ");

  // Depth was already decremented by FormatOxCamlValueInternal before calling
  // this function
  FormatOxCamlValueInternal(stream, computation_ptr, pointed_data, process_sp,
                            exe_ctx_ref, depth);

  return true;
}

static bool FormatOxCamlClosure(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor &data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref,
                                bool is_infix, uint32_t depth) {
  Status error;
  const char *closure_type = is_infix ? "infix closure" : "closure";
  const char *closure_marker = is_infix ? "<infix closure>" : "<closure>";

  uint64_t closinfo = process_sp->ReadUnsignedIntegerFromMemory(
      value + helpers::constants::WORD_SIZE, helpers::constants::WORD_SIZE, 0,
      error);
  if (error.Fail()) {
    OXCAML_EXPLAIN_ERROR_MARKER(closure_marker,
                                "{0} 0x{1:x}: closinfo unreadable",
                                closure_type, value);
    stream.PutCString(closure_marker);
    return true;
  }

  uint64_t code_ptr_offset = helpers::closure::GetCodePtrOffset(closinfo);
  if (code_ptr_offset >= wosize) {
    OXCAML_EXPLAIN_ERROR_MARKER(
        closure_marker,
        "{0} 0x{1:x}: code pointer offset {2} out of range (wosize={3})",
        closure_type, value, code_ptr_offset, wosize);
    stream.PutCString(closure_marker);
    return true;
  }

  lldb::addr_t code_ptr = process_sp->ReadPointerFromMemory(
      value + code_ptr_offset * helpers::constants::WORD_SIZE, error);
  if (error.Fail()) {
    OXCAML_EXPLAIN_ERROR_MARKER(closure_marker,
                                "{0} 0x{1:x}: code pointer unreadable",
                                closure_type, value);
    stream.PutCString(closure_marker);
    return true;
  }

  lldb::TargetSP target_sp = exe_ctx_ref.GetTargetSP();
  if (!target_sp) {
    OXCAML_EXPLAIN_ERROR_MARKER(
        closure_marker,
        "{0} 0x{1:x}: cannot resolve code pointer 0x{2:x} (no target)",
        closure_type, value, code_ptr);
    stream.PutCString(closure_marker);
    return true;
  }

  Address addr;
  if (!addr.SetLoadAddress(code_ptr, target_sp.get())) {
    OXCAML_EXPLAIN_ERROR_MARKER(
        closure_marker,
        "{0} 0x{1:x}: code pointer 0x{2:x} not mapped in target", closure_type,
        value, code_ptr);
    stream.PutCString(closure_marker);
    return true;
  }

  StreamString addr_stream;
  addr.Dump(&addr_stream, nullptr, Address::DumpStyleResolvedDescription,
            Address::DumpStyleFileAddress, 8, false);

  const char *resolved = addr_stream.GetData();
  if (!resolved || resolved[0] == '\0') {
    OXCAML_EXPLAIN_ERROR_MARKER(
        closure_marker,
        "{0} 0x{1:x}: code pointer 0x{2:x} has no symbol description",
        closure_type, value, code_ptr);
    stream.PutCString(closure_marker);
  } else
    stream.Printf("%s", resolved);
  return true;
}

static bool FormatOxCamlObject(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor &data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref,
                               uint32_t depth) {
  // CR sspies: Consider changing this to a non-angle-bracket form for clarity.
  stream.Printf("<object|%" PRIu64 " words>@%p", wosize, (void *)value);
  return true;
}

static bool FormatOxCamlForward(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor &data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref,
                                uint32_t depth) {
  Status error;
  constexpr const char *marker = "<forward>";
  lldb::addr_t forward_ptr = process_sp->ReadPointerFromMemory(value, error);
  if (error.Fail()) {
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Forward pointer 0x{0:x}: target pointer unreadable", value);
    stream.PutCString(marker);
    return true;
  }

  if (forward_ptr == 0) {
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Forward pointer 0x{0:x}: target pointer is null", value);
    stream.PutCString(marker);
    return true;
  }

  uint64_t forwarded_value = forward_ptr;
  DataExtractor forwarded_data(&forwarded_value, helpers::constants::WORD_SIZE,
                               process_sp->GetByteOrder(),
                               helpers::constants::WORD_SIZE);

  // Transparently forward to the internal helper - no wrapper tags
  // This makes forward pointers completely invisible to the user
  // Depth was already decremented by FormatOxCamlValueInternal before calling
  // this function
  return FormatOxCamlValueInternal(stream, forward_ptr, forwarded_data,
                                   process_sp, exe_ctx_ref, depth);
}

static bool FormatOxCamlAbstract(Stream &stream, uint64_t value,
                                 uint64_t wosize, DataExtractor &data,
                                 lldb::ProcessSP process_sp,
                                 const ExecutionContextRef &exe_ctx_ref,
                                 uint32_t depth) {
  // CR sspies: Consider changing this to a non-angle-bracket form for clarity.
  stream.Printf("<abstract|%" PRIu64 " words>@%p", wosize, (void *)value);
  return true;
}

static bool FormatOxCamlString(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor &data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref,
                               uint32_t depth) {
  auto string_opt = helpers::ReadOCamlStringData(value, wosize, process_sp);
  if (!string_opt) {
    constexpr const char *marker = "<string>";
    OXCAML_EXPLAIN_ERROR_MARKER(marker, "String at 0x{0:x}: data unreadable",
                                value);
    stream.PutCString(marker);
    return false;
  }

  helpers::FormatOCamlString(&stream, string_opt->c_str(),
                             string_opt->length());
  return true;
}

static bool FormatOxCamlDouble(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor &data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref,
                               uint32_t depth) {
  Status error;

  uint64_t float_bits = process_sp->ReadUnsignedIntegerFromMemory(
      value, constants::DOUBLE_SIZE, 0, error);

  if (error.Fail()) {
    constexpr const char *marker = "<float>";
    OXCAML_EXPLAIN_ERROR_MARKER(marker, "Float at 0x{0:x}: data unreadable",
                                value);
    stream.PutCString(marker);
    return false;
  }

  llvm::APInt apint(8 * constants::DOUBLE_SIZE, float_bits);
  llvm::APFloat apfloat(llvm::APFloat::IEEEdouble(), apint);

  helpers::FormatAPFloat(&stream, apfloat);
  return true;
}

static bool FormatOxCamlDoubleArray(Stream &stream, uint64_t value,
                                    uint64_t wosize, DataExtractor &data,
                                    lldb::ProcessSP process_sp,
                                    const ExecutionContextRef &exe_ctx_ref,
                                    uint32_t depth) {
  Status error;
  bool had_error = false;

  stream.Printf("[| ");

  for (uint64_t index = 0; index < wosize; index++) {
    uint64_t element_address =
        value + (index * helpers::constants::DOUBLE_SIZE);
    uint64_t float_bits = process_sp->ReadUnsignedIntegerFromMemory(
        element_address, helpers::constants::DOUBLE_SIZE, 0, error);

    if (error.Fail()) {
      constexpr const char *marker = "<float>";
      OXCAML_EXPLAIN_ERROR_MARKER(
          marker, "Float array element {0} at 0x{1:x}: data unreadable", index,
          element_address);
      stream.PutCString(marker);
      had_error = true;
      error.Clear();
    } else {
      llvm::APInt apint(8 * helpers::constants::DOUBLE_SIZE, float_bits);
      llvm::APFloat apfloat(llvm::APFloat::IEEEdouble(), apint);

      // CR sspies: Consider adding "#" prefix for float array elements since
      // they are internally unboxed
      helpers::FormatAPFloat(&stream, apfloat);
    }

    if (index < wosize - 1) {
      stream.Printf("; ");
    }
  }

  stream.Printf(" |]");
  return !had_error;
}

// Custom type formatters implementation

static bool FormatInt32Custom(Stream &stream, const std::string &identifier,
                              uint64_t data_ptr, uint64_t wosize,
                              lldb::ProcessSP process_sp) {
  Status error;

  uint32_t int_value = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr, helpers::constants::INT32_SIZE, 0, error);

  if (error.Fail()) {
    constexpr const char *marker = "<int32>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Custom block '{0}' at 0x{1:x}: int32 payload unreadable",
        identifier, data_ptr);
    stream.PutCString(marker);
    return false;
  }

  llvm::APInt apint(helpers::constants::INT32_SIZE * 8, int_value);
  helpers::FormatAPInt(&stream, apint, true, "",
                       helpers::suffixes::INT32_SUFFIX);
  return true;
}

static bool FormatInt64Custom(Stream &stream, const std::string &identifier,
                              uint64_t data_ptr, uint64_t wosize,
                              lldb::ProcessSP process_sp) {
  Status error;

  uint64_t int_value = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr, helpers::constants::WORD_SIZE, 0, error);

  if (error.Fail()) {
    constexpr const char *marker = "<int64>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Custom block '{0}' at 0x{1:x}: int64 payload unreadable",
        identifier, data_ptr);
    stream.PutCString(marker);
    return false;
  }

  llvm::APInt apint(helpers::constants::INT64_SIZE * 8, int_value);
  helpers::FormatAPInt(&stream, apint, true, "",
                       helpers::suffixes::INT64_SUFFIX);
  return true;
}

static bool FormatNativeIntCustom(Stream &stream, const std::string &identifier,
                                  uint64_t data_ptr, uint64_t wosize,
                                  lldb::ProcessSP process_sp) {
  Status error;

  uint64_t int_value = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr, helpers::constants::WORD_SIZE, 0, error);

  if (error.Fail()) {
    constexpr const char *marker = "<nativeint>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Custom block '{0}' at 0x{1:x}: nativeint payload unreadable",
        identifier, data_ptr);
    stream.PutCString(marker);
    return false;
  }

  llvm::APInt apint(helpers::constants::WORD_SIZE * 8, int_value);
  helpers::FormatAPInt(&stream, apint, true, "",
                       helpers::suffixes::NATIVEINT_SUFFIX);
  return true;
}

static bool FormatBigarrayCustom(Stream &stream, const std::string &identifier,
                                 uint64_t data_ptr, uint64_t wosize,
                                 lldb::ProcessSP process_sp) {
  Status error;

  if (wosize < 2) {
    constexpr const char *marker = "<bigarray>";
    OXCAML_EXPLAIN_ERROR_MARKER(marker,
                                "Bigarray custom block '{0}' at 0x{1:x}: "
                                "payload too small (wosize={2})",
                                identifier, data_ptr, wosize);
    stream.PutCString(marker);
    return false;
  }

  lldb::addr_t bigarray_data_ptr =
      process_sp->ReadPointerFromMemory(data_ptr, error);
  if (error.Fail()) {
    constexpr const char *marker = "<bigarray>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker,
        "Bigarray custom block '{0}' at 0x{1:x}: data pointer unreadable",
        identifier, data_ptr);
    stream.PutCString(marker);
    return false;
  }

  uint64_t num_dims = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr + helpers::constants::WORD_SIZE, helpers::constants::WORD_SIZE,
      0, error);
  if (error.Fail()) {
    constexpr const char *marker = "<bigarray>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker,
        "Bigarray custom block '{0}' at 0x{1:x}: dimension count unreadable",
        identifier, data_ptr);
    stream.PutCString(marker);
    return false;
  }

  // CR sspies: Consider changing this to a non-angle-bracket form for clarity.
  stream.Printf("<bigarray%" PRIu64 "|data=%p>", num_dims,
                (void *)bigarray_data_ptr);
  return true;
}

static bool FormatFloat32Custom(Stream &stream, const std::string &identifier,
                                uint64_t data_ptr, uint64_t wosize,
                                lldb::ProcessSP process_sp) {
  Status error;

  uint32_t float_bits = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr, helpers::constants::FLOAT32_SIZE, 0, error);

  if (error.Fail()) {
    constexpr const char *marker = "<float32>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Float32 custom block '{0}' at 0x{1:x}: payload unreadable",
        identifier, data_ptr);
    stream.PutCString(marker);
    return false;
  }

  llvm::APInt apint(helpers::constants::FLOAT32_SIZE * 8, float_bits);
  llvm::APFloat apfloat(llvm::APFloat::IEEEsingle(), apint);

  helpers::FormatAPFloat(&stream, apfloat, std::nullopt, "",
                         helpers::suffixes::FLOAT32_SUFFIX);
  return true;
}

static const std::map<std::string, CustomTypeFormatter> custom_formatters = {
    {"_i", FormatInt32Custom},           // Int32.t
    {"_j", FormatInt64Custom},           // Int64.t
    {"_n", FormatNativeIntCustom},       // Nativeint.t
    {"_bigarr02", FormatBigarrayCustom}, // Bigarray.t
    {"_f32", FormatFloat32Custom}        // Float32.t
};

static bool FormatOxCamlCustom(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor &data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref,
                               uint32_t depth) {
  Status error;

  if (wosize < 1) {
    constexpr const char *marker = "<custom>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Custom block at 0x{0:x}: payload too small (wosize={1})",
        value, wosize);
    stream.PutCString(marker);
    return false;
  }

  lldb::addr_t custom_ops_ptr = process_sp->ReadPointerFromMemory(value, error);
  if (error.Fail()) {
    constexpr const char *marker = "<custom>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Custom block at 0x{0:x}: custom_operations pointer unreadable",
        value);
    stream.PutCString(marker);
    return false;
  }

  lldb::addr_t identifier_ptr =
      process_sp->ReadPointerFromMemory(custom_ops_ptr, error);
  if (error.Fail()) {
    constexpr const char *marker = "<custom>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Custom block at 0x{0:x}: identifier pointer unreadable",
        value);
    stream.PutCString(marker);
    return false;
  }

  std::string identifier_str;
  if (!process_sp->ReadCStringFromMemory(identifier_ptr, identifier_str,
                                         error) ||
      error.Fail()) {
    constexpr const char *marker = "<custom>";
    OXCAML_EXPLAIN_ERROR_MARKER(
        marker, "Custom block at 0x{0:x}: identifier string unreadable", value);
    stream.PutCString(marker);
    return false;
  }

  auto formatter_it = custom_formatters.find(identifier_str);
  if (formatter_it != custom_formatters.end()) {
    uint64_t data_ptr = value + helpers::constants::WORD_SIZE;
    return formatter_it->second(stream, identifier_str, data_ptr, wosize,
                                process_sp);
  } else {
    // CR sspies: Consider changing this to a non-angle-bracket form for
    // clarity.
    stream.Printf("<custom|\"%s\">", identifier_str.c_str());
    return true;
  }
}
