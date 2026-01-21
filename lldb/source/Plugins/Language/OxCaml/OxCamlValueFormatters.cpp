//===-- OxCamlValueFormatters.cpp -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// \file
/// This file implements formatting for OCaml boxed values and immediates
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
#include "OxCamlAssert.h"
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

#define ENSURE(CONDITION, STREAM, MARKER_EXPR, ERROR_FMT, ...)                 \
  OXCAML_CONDITIONALLY_EMIT_MARKER_AND_RETURN(                                 \
      STREAM, CONDITION, true, MARKER_EXPR, ERROR_FMT, __VA_ARGS__)

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
static bool
FormatOxCamlDoubleArrayInternal(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor &data, lldb::ProcessSP process_sp,
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
  }

  // Check depth limit before processing blocks (central depth check)
  if (depth <= 0) {
    auto header_opt = helpers::ReadBlockHeader(value, process_sp);
    ENSURE(header_opt.has_value(), stream, "<block>",
           "Failed to read block header for pointer 0x{0:x}", value);
    uint8_t tag = header::ExtractTag(*header_opt);
    stream.Printf("[ tag = %u, addr = %p ]", tag, (void *)value);
    return true;
  }

  return FormatOxCamlPointer(stream, value, data, process_sp, exe_ctx_ref,
                             depth - 1);
}

bool lldb_private::formatters::oxcaml::FormatOxCamlValue(
    Stream &stream, DataExtractor &data, lldb::ProcessSP process_sp,
    const ExecutionContextRef &exe_ctx_ref) {
  OX_ASSERT(process_sp, "FormatOxCamlValue requires ProcessSP (ptr={0:P})",
            process_sp.get());

  ENSURE(data.GetByteSize() >= helpers::constants::WORD_SIZE, stream, "<value>",
         "Data size {0} < expected word size {1}", data.GetByteSize(),
         helpers::constants::WORD_SIZE);

  lldb::offset_t offset = 0;
  uint64_t value = data.GetU64(&offset);

  ENSURE(offset != 0, stream, "<value>",
         "Failed to read OCaml value bytes (size {0})", data.GetByteSize());

  uint32_t max_depth = 5; // Default to 5 levels of depth
  lldb::TargetSP target_sp = exe_ctx_ref.GetTargetSP();
  if (target_sp) {
    auto [depth, is_default] = target_sp->GetMaximumDepthOfChildrenToDisplay();
    // Only use the setting if it's not the default (user has explicitly set it)
    if (!is_default)
      max_depth = depth;
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
  ENSURE(value != 0, stream, "<null>", "{0}",
         "Encountered unexpected null OCaml block pointer");

  // Note: process_sp is guaranteed to be valid by FormatOxCamlValue
  auto header_opt = helpers::ReadBlockHeader(value, process_sp);
  ENSURE(header_opt.has_value(), stream, "<block>",
         "Failed to read OCaml block header for pointer 0x{0:x}", value);

  uint64_t header = *header_opt;
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
    return FormatOxCamlDoubleArrayInternal(stream, value, wosize, data,
                                           process_sp, exe_ctx_ref, depth);
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

  for (uint64_t i = 0; i < scannable_wosize; i++) {
    if (i > 0) {
      stream.Printf(", ");
    }

    Status error;
    uint64_t read_address = value + i * helpers::constants::WORD_SIZE;
    uint64_t field_value = process_sp->ReadUnsignedIntegerFromMemory(
        read_address, helpers::constants::WORD_SIZE, 0, error);

    if (error.Fail()) {
      OXCAML_EMIT_MARKER(stream,
                         llvm::formatv("<field@0x{0:x}>", read_address).str(),
                         "Failed to read OCaml block field {0} at 0x{1:x}: {2}",
                         i, read_address, error.AsCString());
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
  ENSURE(wosize != 0, stream, "<lazy>",
         "Lazy block 0x{0:x}: empty payload (wosize=0)", value);

  Status error;

  lldb::addr_t computation_ptr =
      process_sp->ReadPointerFromMemory(value, error);
  ENSURE(!error.Fail(), stream, "<lazy>",
         "Lazy block 0x{0:x}: computation pointer unreadable", value);

  // CR sspies: Double check this assumption in the future.
  // Check if the lazy value is forced (has a valid pointer)
  // Unforced lazy values typically have special marker values
  if (computation_ptr == 0) {
    stream.PutCString("<lazy>");
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
  ENSURE(!error.Fail(), stream, closure_marker,
         "{0} 0x{1:x}: closinfo unreadable", closure_type, value);

  uint64_t code_ptr_offset = helpers::closure::GetCodePtrOffset(closinfo);
  ENSURE(code_ptr_offset < wosize, stream, closure_marker,
         "{0} 0x{1:x}: code pointer offset {2} out of range (wosize={3})",
         closure_type, value, code_ptr_offset, wosize);

  lldb::addr_t code_ptr = process_sp->ReadPointerFromMemory(
      value + code_ptr_offset * helpers::constants::WORD_SIZE, error);
  ENSURE(!error.Fail(), stream, closure_marker,
         "{0} 0x{1:x}: code pointer unreadable", closure_type, value);

  lldb::TargetSP target_sp = exe_ctx_ref.GetTargetSP();
  ENSURE(target_sp, stream, closure_marker,
         "{0} 0x{1:x}: cannot resolve code pointer 0x{2:x} (no target)",
         closure_type, value, code_ptr);

  Address addr;
  ENSURE(addr.SetLoadAddress(code_ptr, target_sp.get()), stream, closure_marker,
         "{0} 0x{1:x}: code pointer 0x{2:x} not mapped in target", closure_type,
         value, code_ptr);

  StreamString addr_stream;
  addr.Dump(&addr_stream, nullptr, Address::DumpStyleResolvedDescription,
            Address::DumpStyleFileAddress, 8, false);

  const char *resolved = addr_stream.GetData();
  ENSURE(resolved && resolved[0] != '\0', stream, closure_marker,
         "{0} 0x{1:x}: code pointer 0x{2:x} has no symbol description",
         closure_type, value, code_ptr);

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
  lldb::addr_t forward_ptr = process_sp->ReadPointerFromMemory(value, error);
  ENSURE(!error.Fail(), stream, "<forward>",
         "Forward pointer 0x{0:x}: target pointer unreadable", value);

  ENSURE(forward_ptr != 0, stream, "<forward>",
         "Forward pointer 0x{0:x}: target pointer is null", value);

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
  ENSURE(string_opt.has_value(), stream, "<string>",
         "String at 0x{0:x}: data unreadable", value);

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

  ENSURE(!error.Fail(), stream, "<float>", "Float at 0x{0:x}: data unreadable",
         value);

  llvm::APInt apint(8 * constants::DOUBLE_SIZE, float_bits);
  llvm::APFloat apfloat(llvm::APFloat::IEEEdouble(), apint);

  helpers::FormatAPFloat(&stream, apfloat);
  return true;
}

bool lldb_private::formatters::oxcaml::FormatOxCamlDoubleArray(
    Stream &stream, lldb::addr_t array_ptr, uint64_t wosize,
    lldb::ProcessSP process_sp) {
  stream.Printf("[| ");

  for (uint64_t index = 0; index < wosize; index++) {
    if (index > 0)
      stream.Printf("; ");

    uint64_t element_address =
        array_ptr + (index * helpers::constants::DOUBLE_SIZE);
    Status error;
    uint64_t float_bits = process_sp->ReadUnsignedIntegerFromMemory(
        element_address, helpers::constants::DOUBLE_SIZE, 0, error);

    if (error.Fail()) {
      OXCAML_EMIT_MARKER(stream, "<float>",
                         "Float array element {0} at 0x{1:x}: data unreadable",
                         index, element_address);
      continue;
    }

    llvm::APInt apint(8 * helpers::constants::DOUBLE_SIZE, float_bits);
    llvm::APFloat apfloat(llvm::APFloat::IEEEdouble(), apint);

    // CR sspies: Consider adding "#" prefix for float array elements since
    // they are internally unboxed
    helpers::FormatAPFloat(&stream, apfloat);
  }

  stream.Printf(" |]");
  return true;
}

static bool
FormatOxCamlDoubleArrayInternal(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor &data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref,
                                uint32_t depth) {
  return lldb_private::formatters::oxcaml::FormatOxCamlDoubleArray(
      stream, value, wosize, process_sp);
}

// Custom type formatters implementation

static bool FormatIntegerCustom(Stream &stream, const std::string &identifier,
                                uint64_t data_ptr, lldb::ProcessSP process_sp,
                                size_t byte_size, const char *marker,
                                const char *payload_desc, const char *suffix) {
  Status error;
  uint64_t int_value =
      process_sp->ReadUnsignedIntegerFromMemory(data_ptr, byte_size, 0, error);
  ENSURE(!error.Fail(), stream, marker,
         "Custom block '{0}' at 0x{1:x}: {2} payload unreadable", identifier,
         data_ptr, payload_desc);

  llvm::APInt apint(byte_size * 8, int_value);
  helpers::FormatAPInt(&stream, apint, true, "", suffix);
  return true;
}

static bool FormatInt32Custom(Stream &stream, const std::string &identifier,
                              uint64_t data_ptr, uint64_t wosize,
                              lldb::ProcessSP process_sp) {
  // Int32.t custom block: custom_ops pointer (1 word) + payload (1 word)
  ENSURE(
      wosize == 2, stream, "<int32>",
      "Int32 custom block '{0}' at 0x{1:x}: unexpected wosize {2} (expected 2)",
      identifier, data_ptr, wosize);
  return FormatIntegerCustom(stream, identifier, data_ptr, process_sp,
                             helpers::constants::INT32_SIZE, "<int32>", "int32",
                             helpers::suffixes::INT32_SUFFIX);
}

static bool FormatInt64Custom(Stream &stream, const std::string &identifier,
                              uint64_t data_ptr, uint64_t wosize,
                              lldb::ProcessSP process_sp) {
  // Int64.t custom block: custom_ops pointer (1 word) + payload (1 word)
  ENSURE(
      wosize == 2, stream, "<int64>",
      "Int64 custom block '{0}' at 0x{1:x}: unexpected wosize {2} (expected 2)",
      identifier, data_ptr, wosize);
  return FormatIntegerCustom(stream, identifier, data_ptr, process_sp,
                             helpers::constants::INT64_SIZE, "<int64>", "int64",
                             helpers::suffixes::INT64_SUFFIX);
}

static bool FormatNativeIntCustom(Stream &stream, const std::string &identifier,
                                  uint64_t data_ptr, uint64_t wosize,
                                  lldb::ProcessSP process_sp) {
  // Nativeint.t custom block: custom_ops pointer (1 word) + payload (1 word)
  ENSURE(wosize == 2, stream, "<nativeint>",
         "Nativeint custom block '{0}' at 0x{1:x}: unexpected wosize {2} "
         "(expected 2)",
         identifier, data_ptr, wosize);
  return FormatIntegerCustom(stream, identifier, data_ptr, process_sp,
                             helpers::constants::WORD_SIZE, "<nativeint>",
                             "nativeint", helpers::suffixes::NATIVEINT_SUFFIX);
}

static bool FormatBigarrayCustom(Stream &stream, const std::string &identifier,
                                 uint64_t data_ptr, uint64_t wosize,
                                 lldb::ProcessSP process_sp) {
  Status error;

  ENSURE(wosize >= 2, stream, "<bigarray>",
         "Bigarray custom block '{0}' at 0x{1:x}: payload too small "
         "(wosize={2})",
         identifier, data_ptr, wosize);

  lldb::addr_t bigarray_data_ptr =
      process_sp->ReadPointerFromMemory(data_ptr, error);
  ENSURE(!error.Fail(), stream, "<bigarray>",
         "Bigarray custom block '{0}' at 0x{1:x}: data pointer unreadable",
         identifier, data_ptr);

  uint64_t num_dims = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr + helpers::constants::WORD_SIZE, helpers::constants::WORD_SIZE,
      0, error);
  ENSURE(!error.Fail(), stream, "<bigarray>",
         "Bigarray custom block '{0}' at 0x{1:x}: dimension count unreadable",
         identifier, data_ptr);

  // CR sspies: Consider changing this to a non-angle-bracket form for clarity.
  stream.Printf("<bigarray%" PRIu64 "|data=%p>", num_dims,
                (void *)bigarray_data_ptr);
  return true;
}

static bool FormatFloat32Custom(Stream &stream, const std::string &identifier,
                                uint64_t data_ptr, uint64_t wosize,
                                lldb::ProcessSP process_sp) {
  // Float32.t custom block: custom_ops pointer (1 word) + payload (1 word)
  ENSURE(wosize == 2, stream, "<float32>",
         "Float32 custom block '{0}' at 0x{1:x}: unexpected wosize {2} "
         "(expected 2)",
         identifier, data_ptr, wosize);

  Status error;
  uint32_t float_bits = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr, helpers::constants::FLOAT32_SIZE, 0, error);

  ENSURE(!error.Fail(), stream, "<float32>",
         "Float32 custom block '{0}' at 0x{1:x}: payload unreadable",
         identifier, data_ptr);

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

  ENSURE(wosize >= 1, stream, "<custom>",
         "Custom block at 0x{0:x}: payload too small (wosize={1})", value,
         wosize);

  lldb::addr_t custom_ops_ptr = process_sp->ReadPointerFromMemory(value, error);
  ENSURE(!error.Fail(), stream, "<custom>",
         "Custom block at 0x{0:x}: custom_operations pointer unreadable",
         value);

  lldb::addr_t identifier_ptr =
      process_sp->ReadPointerFromMemory(custom_ops_ptr, error);
  ENSURE(!error.Fail(), stream, "<custom>",
         "Custom block at 0x{0:x}: identifier pointer unreadable", value);

  std::string identifier_str;
  ENSURE(process_sp->ReadCStringFromMemory(identifier_ptr, identifier_str,
                                           error) &&
             !error.Fail(),
         stream, "<custom>",
         "Custom block at 0x{0:x}: identifier string unreadable", value);

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
