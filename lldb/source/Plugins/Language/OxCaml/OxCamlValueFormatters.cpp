//===-- OxCamlValueFormatters.cpp --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// \file
/// This file implements formatting for OCaml boxed values and immediates (ocaml_value type).
///
/// Provides decoding for:
/// - Immediate values (tagged integers, etc.)
/// - Heap-allocated blocks (strings, floats, arrays, closures, etc.)
/// - Custom blocks (Int32.t, Int64.t, Nativeint.t, Bigarray, Float32)
/// - Special runtime blocks (lazy values, objects, forwarding pointers)
///
/// The formatter uses a depth-limited recursive approach to handle nested structures
/// while preventing infinite loops on cyclic data.

#include "OxCamlValueFormatters.h"
#include "OxCamlFormatHelpers.h"
#include "OxCamlHelpers.h"
#include "LogChannelOxCaml.h"
#include "lldb/Core/Address.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/ExecutionContextScope.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Status.h"
#include "lldb/Utility/StreamString.h"
#include "llvm/Support/ErrorHandling.h"
#include <cassert>
#include <map>

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters::oxcaml;
using namespace lldb_private::formatters::oxcaml::helpers;

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
                                  lldb::ProcessSP process_sp,
                                  const ExecutionContextRef &exe_ctx_ref,
                                  uint32_t depth);
static bool FormatOxCamlPointer(Stream &stream, uint64_t value,
                                DataExtractor& data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref,
                                uint32_t depth);
static bool FormatOxCamlGenericBlock(Stream &stream, uint64_t value, uint8_t tag,
                                     uint64_t scannable_wosize, uint64_t non_scannable_wosize,
                                     DataExtractor& data,
                                     lldb::ProcessSP process_sp,
                                     const ExecutionContextRef &exe_ctx_ref,
                                     uint32_t depth);
static bool FormatOxCamlLazy(Stream &stream, uint64_t value, uint64_t wosize,
                             DataExtractor& data, lldb::ProcessSP process_sp,
                             const ExecutionContextRef &exe_ctx_ref,
                             uint32_t depth);
static bool FormatOxCamlClosure(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor& data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref, bool is_infix,
                                uint32_t depth);
static bool FormatOxCamlObject(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref,
                               uint32_t depth);
static bool FormatOxCamlForward(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor& data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref,
                                uint32_t depth);
static bool FormatOxCamlAbstract(Stream &stream, uint64_t value, uint64_t wosize,
                                 DataExtractor& data, lldb::ProcessSP process_sp,
                                 const ExecutionContextRef &exe_ctx_ref,
                                 uint32_t depth);
static bool FormatOxCamlString(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref,
                               uint32_t depth);
static bool FormatOxCamlDouble(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref,
                               uint32_t depth);
static bool FormatOxCamlDoubleArray(Stream &stream, uint64_t value, uint64_t wosize,
                                    DataExtractor& data, lldb::ProcessSP process_sp,
                                    const ExecutionContextRef &exe_ctx_ref,
                                    uint32_t depth);
static bool FormatOxCamlCustom(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
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
typedef bool (*CustomTypeFormatter)(Stream &stream, const std::string &identifier,
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
                                      DataExtractor& data,
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
      uint64_t header = process_sp->ReadUnsignedIntegerFromMemory(
          header::GetHeaderAddress(value),
          constants::WORD_SIZE, 0, error);
      if (error.Fail()) {
        stream.Printf("<max depth reached>");
        return true;
      }
      uint8_t tag = header::ExtractTag(header);
      stream.Printf("[ tag = %u, addr = %p ]", tag, (void*)value);
      return true;
    }
    return FormatOxCamlPointer(stream, value, data, process_sp, exe_ctx_ref, depth - 1);
  }
}

bool lldb_private::formatters::oxcaml::FormatOxCamlValue(Stream &stream,
                                                         OxCamlValueType* value_type,
                                                         DataExtractor& data,
                                                         lldb::ProcessSP process_sp,
                                                         const ExecutionContextRef &exe_ctx_ref) {
  assert(value_type->GetByteSize() == helpers::constants::WORD_SIZE && "OCaml value types must be 8 bytes");

  if (!process_sp) {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log, "FATAL: FormatOxCamlValue called without valid process - this is a critical system error");
    llvm::report_fatal_error("FormatOxCamlValue called without valid process - OCaml values require memory access");
  }

  lldb::offset_t offset = 0;
  uint64_t value = data.GetU64(&offset);

  if (offset == 0) {
    stream.Printf("<could not read OCaml value>");
    return false;
  }

  uint32_t max_depth = 5;  // Default to 5 levels of depth
  lldb::TargetSP target_sp = exe_ctx_ref.GetTargetSP();
  if (target_sp) {
    auto [depth, is_default] = target_sp->GetMaximumDepthOfChildrenToDisplay();
    // Only use the setting if it's not the default (user has explicitly set it)
    // Otherwise use our OCaml-specific default of 5
    if (!is_default) {
      max_depth = depth;
    }
  }

  return FormatOxCamlValueInternal(stream, value, data, process_sp, exe_ctx_ref, max_depth);
}

static bool FormatOxCamlImmediate(Stream &stream, uint64_t value,
                                  lldb::ProcessSP process_sp,
                                  const ExecutionContextRef &exe_ctx_ref,
                                  uint32_t depth) {
  int64_t untagged_value = helpers::value::UntagImmediate(value);
  stream.Printf("%" PRId64 "%s", untagged_value, helpers::suffixes::TAGGED_INT_SUFFIX);
  return true;
}

static bool FormatOxCamlPointer(Stream &stream, uint64_t value,
                                DataExtractor& data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref,
                                uint32_t depth) {
  if (value == 0) {
    stream.Printf("<null>");
    return true;
  }

  // Note: process_sp is guaranteed to be valid by FormatOxCamlValue
  Status error;
  lldb::addr_t header_addr = header::GetHeaderAddress(value);
  uint64_t header = process_sp->ReadUnsignedIntegerFromMemory(
      header_addr, constants::WORD_SIZE, 0, error);

  if (error.Fail()) {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log, "WARNING: Cannot read OCaml block header at 0x{0:x} - memory may be invalid or corrupted",
             header_addr);
    stream.Printf("<error reading block header>");
    return false;
  }

  uint8_t tag;
  uint64_t wosize;
  uint8_t reserved;
  helpers::header::ParseHeader(header, tag, wosize, reserved);

  // Dispatch based on tag; none of the special tags support mixed blocks
  switch (tag) {
    case static_cast<uint8_t>(OxCamlSpecialTag::Lazy_tag):
      return FormatOxCamlLazy(stream, value, wosize, data, process_sp, exe_ctx_ref, depth);
    case static_cast<uint8_t>(OxCamlSpecialTag::Closure_tag):
      return FormatOxCamlClosure(stream, value, wosize, data, process_sp, exe_ctx_ref, false, depth);
    case static_cast<uint8_t>(OxCamlSpecialTag::Object_tag):
      return FormatOxCamlObject(stream, value, wosize, data, process_sp, exe_ctx_ref, depth);
    case static_cast<uint8_t>(OxCamlSpecialTag::Infix_tag):
      return FormatOxCamlClosure(stream, value, wosize, data, process_sp, exe_ctx_ref, true, depth);
    case static_cast<uint8_t>(OxCamlSpecialTag::Forward_tag):
      return FormatOxCamlForward(stream, value, wosize, data, process_sp, exe_ctx_ref, depth);
    case static_cast<uint8_t>(OxCamlSpecialTag::Abstract_tag):
      return FormatOxCamlAbstract(stream, value, wosize, data, process_sp, exe_ctx_ref, depth);
    case static_cast<uint8_t>(OxCamlSpecialTag::String_tag):
      return FormatOxCamlString(stream, value, wosize, data, process_sp, exe_ctx_ref, depth);
    case static_cast<uint8_t>(OxCamlSpecialTag::Double_tag):
      return FormatOxCamlDouble(stream, value, wosize, data, process_sp, exe_ctx_ref, depth);
    case static_cast<uint8_t>(OxCamlSpecialTag::Double_array_tag):
      return FormatOxCamlDoubleArray(stream, value, wosize, data, process_sp, exe_ctx_ref, depth);
    case static_cast<uint8_t>(OxCamlSpecialTag::Custom_tag):
      return FormatOxCamlCustom(stream, value, wosize, data, process_sp, exe_ctx_ref, depth);
    default: {
      // Generic block (tag < 246): for mixed blocks, calculate scannable and non-scannable sizes
      uint64_t scannable_wosize = helpers::header::ExtractScannableWosize(reserved, wosize);
      uint64_t non_scannable_wosize = helpers::header::ExtractNonScannableWosize(reserved, wosize);
      return FormatOxCamlGenericBlock(stream, value, tag, scannable_wosize, non_scannable_wosize, data, process_sp, exe_ctx_ref, depth);
    }
  }
}

static bool FormatOxCamlGenericBlock(Stream &stream, uint64_t value, uint8_t tag,
                                     uint64_t scannable_wosize, uint64_t non_scannable_wosize,
                                     DataExtractor& data,
                                     lldb::ProcessSP process_sp,
                                     const ExecutionContextRef &exe_ctx_ref,
                                     uint32_t depth) {
  // Format: [ tag = N | field1, field2, ... ] or [ tag = N; non value fields: M | field1, field2, ... ]
  if (non_scannable_wosize > 0) {
    stream.Printf("[ tag = %u; non value fields: %llu) | ", tag, (unsigned long long)non_scannable_wosize);
  } else {
    stream.Printf("[ tag = %u | ", tag);
  }

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
      stream.Printf("<unreadable at 0x%llx>", (unsigned long long)read_address);
      continue;
    }

    DataExtractor field_data(&field_value, helpers::constants::WORD_SIZE,
                             process_sp->GetByteOrder(), helpers::constants::WORD_SIZE);

    // Recursively format the field (depth already decremented by caller)
    FormatOxCamlValueInternal(stream, field_value, field_data,
                              process_sp, exe_ctx_ref, depth);
  }

  stream.Printf(" ]");
  return true;
}

static bool FormatOxCamlLazy(Stream &stream, uint64_t value, uint64_t wosize,
                             DataExtractor& data, lldb::ProcessSP process_sp,
                             const ExecutionContextRef &exe_ctx_ref,
                             uint32_t depth) {
  Status error;

  lldb::addr_t computation_ptr = process_sp->ReadPointerFromMemory(value, error);
  if (error.Fail()) {
    stream.Printf("<lazy, computation ptr unreadable>");
    return true;
  }

  // Check if the lazy value is forced (has a valid pointer)
  // Unforced lazy values typically have special marker values
  if (computation_ptr == 0) {
    stream.Printf("<lazy, unforced>");
    return true;
  }

  uint64_t computation_value = computation_ptr;
  DataExtractor pointed_data(&computation_value, helpers::constants::WORD_SIZE,
                             process_sp->GetByteOrder(), helpers::constants::WORD_SIZE);

  // Format using OCaml syntax: "Lazy.from_val contents"
  stream.Printf("Lazy.from_val ");

  // Depth was already decremented by FormatOxCamlValueInternal before calling this function
  FormatOxCamlValueInternal(stream, computation_ptr, pointed_data,
                           process_sp, exe_ctx_ref, depth);

  return true;
}

static bool FormatOxCamlClosure(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor& data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref,
                                bool is_infix,
                                uint32_t depth) {
  Status error;
  const char* closure_type = is_infix ? "infix closure" : "closure";

  uint64_t closinfo = process_sp->ReadUnsignedIntegerFromMemory(
      value + helpers::constants::WORD_SIZE,
      helpers::constants::WORD_SIZE, 0, error);
  if (error.Fail()) {
    stream.Printf("<%s, code ptr unreadable>", closure_type);
    return true;
  }

  uint64_t code_ptr_offset = helpers::closure::GetCodePtrOffset(closinfo);

  lldb::addr_t code_ptr = process_sp->ReadPointerFromMemory(
      value + code_ptr_offset * helpers::constants::WORD_SIZE, error);
  if (error.Fail()) {
    stream.Printf("<%s, code ptr unreadable>", closure_type);
    return true;
  }

  lldb::TargetSP target_sp = exe_ctx_ref.GetTargetSP();
  if (!target_sp) {
    stream.Printf("<%s, code ptr %p>", closure_type, (void*)code_ptr);
    return true;
  }

  Address addr;
  if (!addr.SetLoadAddress(code_ptr, target_sp.get())) {
    stream.Printf("<%s, code ptr %p>", closure_type, (void*)code_ptr);
    return true;
  }

  StreamString addr_stream;
  addr.Dump(&addr_stream, nullptr, Address::DumpStyleResolvedDescription,
            Address::DumpStyleFileAddress, 8, false);

  stream.Printf("<%s>@%s", closure_type, addr_stream.GetData());
  return true;
}

static bool FormatOxCamlObject(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref,
                               uint32_t depth) {
  stream.Printf("<object|%" PRIu64 " words|%p>", wosize, (void*)value);
  return true;
}

static bool FormatOxCamlForward(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor& data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref,
                                uint32_t depth) {
  Status error;

  lldb::addr_t forward_ptr = process_sp->ReadPointerFromMemory(value, error);
  if (error.Fail()) {
    stream.Printf("<forward, ptr unreadable>");
    return true;
  }

  if (forward_ptr == 0) {
    stream.Printf("<forward, null ptr>");
    return true;
  }

  uint64_t forwarded_value = forward_ptr;
  DataExtractor forwarded_data(&forwarded_value, helpers::constants::WORD_SIZE,
                               process_sp->GetByteOrder(), helpers::constants::WORD_SIZE);

  // Transparently forward to the internal helper - no wrapper tags
  // This makes forward pointers completely invisible to the user
  // Depth was already decremented by FormatOxCamlValueInternal before calling this function
  return FormatOxCamlValueInternal(stream, forward_ptr, forwarded_data,
                                   process_sp, exe_ctx_ref, depth);
}

static bool FormatOxCamlAbstract(Stream &stream, uint64_t value, uint64_t wosize,
                                 DataExtractor& data, lldb::ProcessSP process_sp,
                                 const ExecutionContextRef &exe_ctx_ref,
                                 uint32_t depth) {
  stream.Printf("<abstract|%" PRIu64 " words|%p>", wosize, (void*)value);
  return true;
}

static bool FormatOxCamlString(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref,
                               uint32_t depth) {
  Status error;

  uint64_t last_word_address = header::GetLastWordAddress(value, wosize);
  uint64_t last_word = process_sp->ReadUnsignedIntegerFromMemory(
      last_word_address, helpers::constants::WORD_SIZE, 0, error);

  if (error.Fail()) {
    stream.Printf("<could not read string length>");
    return false;
  }

  // CR sspies: This fixes a particular endianness. Generalize.
  uint8_t padding_byte = helpers::string::ExtractPaddingByte(last_word);
  uint64_t string_length = helpers::string::CalculateStringLength(wosize, padding_byte);

  std::vector<uint8_t> str_buffer(string_length);
  size_t bytes_read = process_sp->ReadMemory(value, str_buffer.data(),
                                            string_length, error);

  if (error.Fail() || bytes_read < string_length) {
    stream.Printf("<could not read string data>");
    return false;
  }

  const char *string_data = reinterpret_cast<const char*>(str_buffer.data());
  helpers::FormatOCamlString(&stream, string_data, string_length);
  return true;
}

static bool FormatOxCamlDouble(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref,
                               uint32_t depth) {
  Status error;

  uint64_t float_bits = process_sp->ReadUnsignedIntegerFromMemory(
      value, constants::DOUBLE_SIZE, 0, error);

  if (error.Fail()) {
    stream.Printf("<could not read float data>");
    return false;
  }

  llvm::APInt apint(8 * constants::DOUBLE_SIZE, float_bits);
  llvm::APFloat apfloat(llvm::APFloat::IEEEdouble(), apint);

  helpers::FormatAPFloat(&stream, apfloat);
  return true;
}

static bool FormatOxCamlDoubleArray(Stream &stream, uint64_t value, uint64_t wosize,
                                    DataExtractor& data, lldb::ProcessSP process_sp,
                                    const ExecutionContextRef &exe_ctx_ref,
                                    uint32_t depth) {
  Status error;
  bool had_error = false;

  stream.Printf("[| ");

  for (uint64_t index = 0; index < wosize; index++) {
    uint64_t element_address = value + (index * helpers::constants::WORD_SIZE);
    uint64_t float_bits = process_sp->ReadUnsignedIntegerFromMemory(
        element_address, helpers::constants::WORD_SIZE, 0, error);

    if (error.Fail()) {
      stream.Printf("<could not read float array element %" PRIu64 ">", index);
      had_error = true;
    } else {
      llvm::APInt apint(8 * helpers::constants::WORD_SIZE, float_bits);
      llvm::APFloat apfloat(llvm::APFloat::IEEEdouble(), apint);

      // CR sspies: Consider adding "#" prefix for float array elements since they are internally unboxed
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
    stream.Printf("<custom \"%s\" (could not read data field for int32)>", identifier.c_str());
    return false;
  }

  llvm::APInt apint(helpers::constants::INT32_SIZE * 8, int_value);
  helpers::FormatAPInt(&stream, apint, true, "", helpers::suffixes::INT32_SUFFIX);
  return true;
}

static bool FormatInt64Custom(Stream &stream, const std::string &identifier,
                             uint64_t data_ptr, uint64_t wosize,
                             lldb::ProcessSP process_sp) {
  Status error;

  uint64_t int_value = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr, helpers::constants::WORD_SIZE, 0, error);

  if (error.Fail()) {
    stream.Printf("<custom \"%s\" (could not read data field for int64)>", identifier.c_str());
    return false;
  }

  llvm::APInt apint(helpers::constants::INT64_SIZE * 8, int_value);
  helpers::FormatAPInt(&stream, apint, true, "", helpers::suffixes::INT64_SUFFIX);
  return true;
}

static bool FormatNativeIntCustom(Stream &stream, const std::string &identifier,
                                 uint64_t data_ptr, uint64_t wosize,
                                 lldb::ProcessSP process_sp) {
  Status error;

  uint64_t int_value = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr, helpers::constants::WORD_SIZE, 0, error);

  if (error.Fail()) {
    stream.Printf("<custom \"%s\" (could not read data field for nativeint)>", identifier.c_str());
    return false;
  }

  llvm::APInt apint(helpers::constants::WORD_SIZE * 8, int_value);
  helpers::FormatAPInt(&stream, apint, true, "", helpers::suffixes::NATIVEINT_SUFFIX);
  return true;
}

static bool FormatBigarrayCustom(Stream &stream, const std::string &identifier,
                                uint64_t data_ptr, uint64_t wosize,
                                lldb::ProcessSP process_sp) {
  Status error;

  lldb::addr_t bigarray_data_ptr = process_sp->ReadPointerFromMemory(data_ptr, error);
  if (error.Fail()) {
    stream.Printf("<bigarray (could not read data pointer)>");
    return false;
  }

  uint64_t num_dims = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr + helpers::constants::WORD_SIZE,
      helpers::constants::WORD_SIZE, 0, error);
  if (error.Fail()) {
    stream.Printf("<bigarray (could not read dimensions)>");
    return false;
  }

  stream.Printf("<bigarray%" PRIu64 "|data=%p>", num_dims, (void*)bigarray_data_ptr);
  return true;
}

static bool FormatFloat32Custom(Stream &stream, const std::string &identifier,
                               uint64_t data_ptr, uint64_t wosize,
                               lldb::ProcessSP process_sp) {
  Status error;

  uint32_t float_bits = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr, helpers::constants::FLOAT32_SIZE, 0, error);

  if (error.Fail()) {
    stream.Printf("<could not read float32>");
    return false;
  }

  llvm::APInt apint(helpers::constants::FLOAT32_SIZE * 8, float_bits);
  llvm::APFloat apfloat(llvm::APFloat::IEEEsingle(), apint);

  helpers::FormatAPFloat(&stream, apfloat,
                         std::nullopt, "", helpers::suffixes::FLOAT32_SUFFIX);
  return true;
}

static const std::map<std::string, CustomTypeFormatter> custom_formatters = {
    {"_i", FormatInt32Custom},            // Int32.t
    {"_j", FormatInt64Custom},            // Int64.t
    {"_n", FormatNativeIntCustom},        // Nativeint.t
    {"_bigarr02", FormatBigarrayCustom},  // Bigarray.t
    {"_f32", FormatFloat32Custom}         // Float32.t
};

static bool FormatOxCamlCustom(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref,
                               uint32_t depth) {
  Status error;

  lldb::addr_t custom_ops_ptr = process_sp->ReadPointerFromMemory(value, error);
  if (error.Fail()) {
    stream.Printf("<could not read struct custom_operations pointer from custom block>");
    return false;
  }

  lldb::addr_t identifier_ptr = process_sp->ReadPointerFromMemory(custom_ops_ptr, error);
  if (error.Fail()) {
    stream.Printf("<could not read identifier pointer from struct custom_operations>");
    return false;
  }

  std::string identifier_str;
  if (!process_sp->ReadCStringFromMemory(identifier_ptr, identifier_str, error) || error.Fail()) {
    stream.Printf("<could not read identifier string from custom block>");
    return false;
  }

  auto formatter_it = custom_formatters.find(identifier_str);
  if (formatter_it != custom_formatters.end()) {
    uint64_t data_ptr = value + helpers::constants::WORD_SIZE;
    return formatter_it->second(stream, identifier_str, data_ptr, wosize, process_sp);
  } else {
    stream.Printf("<custom|\"%s\">", identifier_str.c_str());
    return true;
  }
}
