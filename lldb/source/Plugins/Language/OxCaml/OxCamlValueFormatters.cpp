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
                                  const ExecutionContextRef &exe_ctx_ref);
static bool FormatOxCamlPointer(Stream &stream, uint64_t value,
                                DataExtractor& data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref);
static bool FormatOxCamlGenericBlock(Stream &stream, uint64_t value, uint8_t tag,
                                     uint64_t wosize, DataExtractor& data,
                                     lldb::ProcessSP process_sp,
                                     const ExecutionContextRef &exe_ctx_ref);
static bool FormatOxCamlLazy(Stream &stream, uint64_t value, uint64_t wosize,
                             DataExtractor& data, lldb::ProcessSP process_sp,
                             const ExecutionContextRef &exe_ctx_ref);
static bool FormatOxCamlClosure(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor& data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref, bool is_infix);
static bool FormatOxCamlObject(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref);
static bool FormatOxCamlForward(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor& data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref);
static bool FormatOxCamlAbstract(Stream &stream, uint64_t value, uint64_t wosize,
                                 DataExtractor& data, lldb::ProcessSP process_sp,
                                 const ExecutionContextRef &exe_ctx_ref);
static bool FormatOxCamlString(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref);
static bool FormatOxCamlDouble(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref);
static bool FormatOxCamlDoubleArray(Stream &stream, uint64_t value, uint64_t wosize,
                                    DataExtractor& data, lldb::ProcessSP process_sp,
                                    const ExecutionContextRef &exe_ctx_ref);
static bool FormatOxCamlCustom(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref);

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
                                      const ExecutionContextRef &exe_ctx_ref) {
  // Check LSB for immediate vs pointer discrimination
  if ((value & 0x1) == 1) {
    // LSB = 1: Immediate value (tagged integer, unit, etc.)
    return FormatOxCamlImmediate(stream, value, process_sp, exe_ctx_ref);
  } else {
    // LSB = 0: Pointer to heap block
    return FormatOxCamlPointer(stream, value, data, process_sp, exe_ctx_ref);
  }
}

bool lldb_private::formatters::oxcaml::FormatOxCamlValue(Stream &stream,
                                                         OxCamlValueType* value_type,
                                                         DataExtractor& data,
                                                         lldb::ProcessSP process_sp,
                                                         const ExecutionContextRef &exe_ctx_ref) {
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

  // Delegate to internal helper
  return FormatOxCamlValueInternal(stream, value, data, process_sp, exe_ctx_ref);
}

static bool FormatOxCamlImmediate(Stream &stream, uint64_t value,
                                  lldb::ProcessSP process_sp,
                                  const ExecutionContextRef &exe_ctx_ref) {
  // OCaml immediate value: decode tagged integer by right-shifting by 1
  // Convert to signed int64_t to handle negative values correctly
  int64_t signed_value = static_cast<int64_t>(value);
  int64_t untagged_value = signed_value >> 1;

  // Display with "i" suffix to indicate tagged integer
  stream.Printf("%" PRId64 "i", untagged_value);
  return true;
}

static bool FormatOxCamlPointer(Stream &stream, uint64_t value,
                                DataExtractor& data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref) {
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
      return FormatOxCamlLazy(stream, value, wosize, data, process_sp, exe_ctx_ref);
    case static_cast<uint8_t>(OxCamlSpecialTag::Closure_tag):
      return FormatOxCamlClosure(stream, value, wosize, data, process_sp, exe_ctx_ref, false);
    case static_cast<uint8_t>(OxCamlSpecialTag::Object_tag):
      return FormatOxCamlObject(stream, value, wosize, data, process_sp, exe_ctx_ref);
    case static_cast<uint8_t>(OxCamlSpecialTag::Infix_tag):
      return FormatOxCamlClosure(stream, value, wosize, data, process_sp, exe_ctx_ref, true);
    case static_cast<uint8_t>(OxCamlSpecialTag::Forward_tag):
      return FormatOxCamlForward(stream, value, wosize, data, process_sp, exe_ctx_ref);
    case static_cast<uint8_t>(OxCamlSpecialTag::Abstract_tag):
      return FormatOxCamlAbstract(stream, value, wosize, data, process_sp, exe_ctx_ref);
    case static_cast<uint8_t>(OxCamlSpecialTag::String_tag):
      return FormatOxCamlString(stream, value, wosize, data, process_sp, exe_ctx_ref);
    case static_cast<uint8_t>(OxCamlSpecialTag::Double_tag):
      return FormatOxCamlDouble(stream, value, wosize, data, process_sp, exe_ctx_ref);
    case static_cast<uint8_t>(OxCamlSpecialTag::Double_array_tag):
      return FormatOxCamlDoubleArray(stream, value, wosize, data, process_sp, exe_ctx_ref);
    case static_cast<uint8_t>(OxCamlSpecialTag::Custom_tag):
      return FormatOxCamlCustom(stream, value, wosize, data, process_sp, exe_ctx_ref);
    default:
      // Generic block (tag < 246)
      return FormatOxCamlGenericBlock(stream, value, tag, wosize, data, process_sp, exe_ctx_ref);
  }
}

static bool FormatOxCamlGenericBlock(Stream &stream, uint64_t value, uint8_t tag,
                                     uint64_t wosize, DataExtractor& data,
                                     lldb::ProcessSP process_sp,
                                     const ExecutionContextRef &exe_ctx_ref) {
  // Placeholder: Generic OCaml block (variant, record, array, etc.)
  stream.Printf("<block>");
  return true;
}

static bool FormatOxCamlLazy(Stream &stream, uint64_t value, uint64_t wosize,
                             DataExtractor& data, lldb::ProcessSP process_sp,
                             const ExecutionContextRef &exe_ctx_ref) {
  // OCaml lazy value: read computation pointer and recursively format contents
  Status error;

  // Read computation pointer from the first word (offset 0 from value base)
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

  // Create DataExtractor with the computation pointer value
  uint64_t computation_value = computation_ptr;
  DataExtractor pointed_data(&computation_value, 8, process_sp->GetByteOrder(), 8);

  // Format using OCaml syntax: "Lazy.from_val contents"
  stream.Printf("Lazy.from_val ");

  // Recursively format the pointed value using the internal helper
  FormatOxCamlValueInternal(stream, computation_ptr, pointed_data,
                           process_sp, exe_ctx_ref);

  return true;
}

static bool FormatOxCamlClosure(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor& data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref,
                                bool is_infix) {
  Status error;
  const uint64_t word_size = 8;
  const char* closure_type = is_infix ? "infix closure" : "closure";

  // Read closinfo word from first data word (offset 8 from value)
  uint64_t closinfo = process_sp->ReadUnsignedIntegerFromMemory(value + word_size, word_size, 0, error);
  if (error.Fail()) {
    stream.Printf("<%s, code ptr unreadable>", closure_type);
    return true;
  }

  // Extract arity from top 8 bits of closinfo
  uint8_t arity = closinfo >> 56;

  // Calculate code pointer offset based on arity
  // If arity is 0 or 1, offset is 0; otherwise offset is 2
  int offset = (arity == 0 || arity == 1) ? 0 : 2;

  // Read full application code pointer
  lldb::addr_t code_ptr = process_sp->ReadPointerFromMemory(value + offset * word_size, error);
  if (error.Fail()) {
    stream.Printf("<%s, code ptr unreadable>", closure_type);
    return true;
  }

  // Try to resolve function name from code pointer
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

  // Create a string stream to capture the address output
  StreamString addr_stream;
  addr.Dump(&addr_stream, nullptr, Address::DumpStyleResolvedDescription,
            Address::DumpStyleFileAddress, 8, false);

  // Display resolved function name using new format
  stream.Printf("<%s>@%s", closure_type, addr_stream.GetData());
  return true;
}

static bool FormatOxCamlObject(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref) {
  // OCaml object: display word size and address
  stream.Printf("<object|%" PRIu64 " words|%p>", wosize, (void*)value);
  return true;
}

static bool FormatOxCamlForward(Stream &stream, uint64_t value, uint64_t wosize,
                                DataExtractor& data, lldb::ProcessSP process_sp,
                                const ExecutionContextRef &exe_ctx_ref) {
  // OCaml forwarding pointer: transparently display the target value
  Status error;

  // Read forwarding pointer from the first word (offset 0 from value base)
  lldb::addr_t forward_ptr = process_sp->ReadPointerFromMemory(value, error);
  if (error.Fail()) {
    stream.Printf("<forward, ptr unreadable>");
    return true;
  }

  // Check for null forwarding pointer
  if (forward_ptr == 0) {
    stream.Printf("<forward, null ptr>");
    return true;
  }

  uint64_t forwarded_value = forward_ptr;
  DataExtractor forwarded_data(&forwarded_value, 8, process_sp->GetByteOrder(), 8);

  // Transparently forward to the internal helper - no wrapper tags
  // This makes forward pointers completely invisible to the user
  return FormatOxCamlValueInternal(stream, forward_ptr, forwarded_data,
                                   process_sp, exe_ctx_ref);
}

static bool FormatOxCamlAbstract(Stream &stream, uint64_t value, uint64_t wosize,
                                 DataExtractor& data, lldb::ProcessSP process_sp,
                                 const ExecutionContextRef &exe_ctx_ref) {
  // OCaml abstract value: display word size and address
  stream.Printf("<abstract|%" PRIu64 " words|%p>", wosize, (void*)value);
  return true;
}

static bool FormatOxCamlString(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref) {
  // OCaml string: read string bytes from heap memory
  Status error;
  const uint64_t word_size = 8;

  // Read the last word to get the padding byte (OCaml string encoding)
  uint64_t last_word_address = value + (wosize - 1) * word_size;
  uint64_t last_word = process_sp->ReadUnsignedIntegerFromMemory(
      last_word_address, word_size, 0, error);

  if (error.Fail()) {
    stream.Printf("<could not read string length>");
    return false;
  }

  // Extract padding byte from bits 56-63 (last_word >> 56)
  // OCaml string length = wosize * word_size - padding - 1
  // CR sspies: This fixes a particular endianness. Generalize.
  uint8_t padding_byte = last_word >> 56;
  uint64_t string_length = wosize * word_size - padding_byte - 1;

  // Read the string data from memory
  std::vector<uint8_t> str_buffer(string_length);
  size_t bytes_read = process_sp->ReadMemory(value, str_buffer.data(),
                                            string_length, error);

  if (error.Fail() || bytes_read < string_length) {
    stream.Printf("<could not read string data>");
    return false;
  }

  // Use the helper function to format with proper OCaml escaping
  const char *string_data = reinterpret_cast<const char*>(str_buffer.data());
  lldb_private::formatters::oxcaml::helpers::FormatOCamlString(&stream,
                                                               string_data,
                                                               string_length);
  return true;
}

static bool FormatOxCamlDouble(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref) {
  // OCaml boxed float: read 8 bytes of IEEE double precision data
  Status error;
  const uint64_t word_size = 8;

  // Read the float bits from memory at the value address
  uint64_t float_bits = process_sp->ReadUnsignedIntegerFromMemory(
      value, word_size, 0, error);

  if (error.Fail()) {
    stream.Printf("<could not read float data>");
    return false;
  }

  // Convert to APFloat using IEEE double semantics
  llvm::APInt apint(8 * word_size, float_bits);
  llvm::APFloat apfloat(llvm::APFloat::IEEEdouble(), apint);

  // Use helper function for proper OCaml float formatting
  lldb_private::formatters::oxcaml::helpers::FormatAPFloat(&stream, apfloat);
  return true;
}

static bool FormatOxCamlDoubleArray(Stream &stream, uint64_t value, uint64_t wosize,
                                    DataExtractor& data, lldb::ProcessSP process_sp,
                                    const ExecutionContextRef &exe_ctx_ref) {
  // OCaml float array: display in OCaml syntax [| float1; float2; ... |]
  Status error;
  const uint64_t word_size = 8;
  bool had_error = false;

  stream.Printf("[| ");

  for (uint64_t index = 0; index < wosize; index++) {
    // Read float bits from memory at value + (index * word_size)
    uint64_t element_address = value + (index * word_size);
    uint64_t float_bits = process_sp->ReadUnsignedIntegerFromMemory(
        element_address, word_size, 0, error);

    if (error.Fail()) {
      stream.Printf("<could not read float array element %" PRIu64 ">", index);
      had_error = true;
    } else {
      // Convert to APFloat using IEEE double semantics
      llvm::APInt apint(8 * word_size, float_bits);
      llvm::APFloat apfloat(llvm::APFloat::IEEEdouble(), apint);

      // Use helper function for proper OCaml float formatting
      // CR sspies: Consider adding "#" prefix for float array elements since they are internally unboxed
      lldb_private::formatters::oxcaml::helpers::FormatAPFloat(&stream, apfloat);
    }

    // Add separator between elements (but not after the last one)
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
  // Int32.t: read 32-bit integer from data_ptr and format with "l" suffix
  Status error;

  // Read only 4 bytes for Int32.t (matches actual data size)
  uint32_t int_value = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr, 4, 0, error);

  if (error.Fail()) {
    stream.Printf("<custom \"%s\" (could not read data field for int32)>", identifier.c_str());
    return false;
  }

  // Use helper function for consistent integer formatting
  llvm::APInt apint(32, int_value);
  lldb_private::formatters::oxcaml::helpers::FormatAPInt(&stream, apint, true, "", "l");
  return true;
}

static bool FormatInt64Custom(Stream &stream, const std::string &identifier,
                             uint64_t data_ptr, uint64_t wosize,
                             lldb::ProcessSP process_sp) {
  // Int64.t: read 64-bit integer from data_ptr and format with "L" suffix
  Status error;
  const uint64_t word_size = 8;

  uint64_t int_value = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr, word_size, 0, error);

  if (error.Fail()) {
    stream.Printf("<custom \"%s\" (could not read data field for int64)>", identifier.c_str());
    return false;
  }

  // Use helper function for consistent integer formatting
  llvm::APInt apint(64, int_value);
  lldb_private::formatters::oxcaml::helpers::FormatAPInt(&stream, apint, true, "", "L");
  return true;
}

static bool FormatNativeIntCustom(Stream &stream, const std::string &identifier,
                                 uint64_t data_ptr, uint64_t wosize,
                                 lldb::ProcessSP process_sp) {
  // Nativeint.t: read native integer from data_ptr and format with "n" suffix
  Status error;
  const uint64_t word_size = 8;

  uint64_t int_value = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr, word_size, 0, error);

  if (error.Fail()) {
    stream.Printf("<custom \"%s\" (could not read data field for nativeint)>", identifier.c_str());
    return false;
  }

  // Use helper function for consistent integer formatting (64-bit for native int)
  llvm::APInt apint(64, int_value);
  lldb_private::formatters::oxcaml::helpers::FormatAPInt(&stream, apint, true, "", "n");
  return true;
}

static bool FormatBigarrayCustom(Stream &stream, const std::string &identifier,
                                uint64_t data_ptr, uint64_t wosize,
                                lldb::ProcessSP process_sp) {
  // Bigarray: read data pointer and dimensions from the data area
  Status error;
  const uint64_t word_size = 8;

  // Read bigarray data pointer from data_ptr (first field)
  lldb::addr_t bigarray_data_ptr = process_sp->ReadPointerFromMemory(data_ptr, error);
  if (error.Fail()) {
    stream.Printf("<bigarray (could not read data pointer)>");
    return false;
  }

  // Read number of dimensions from data_ptr+8 (second field)
  uint64_t num_dims = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr + word_size, word_size, 0, error);
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
  // Float32: read 32-bit float from data_ptr and format with "s" suffix
  Status error;

  // Read 32-bit float bits from data_ptr (only 4 bytes)
  uint32_t float_bits = process_sp->ReadUnsignedIntegerFromMemory(
      data_ptr, 4, 0, error);

  if (error.Fail()) {
    stream.Printf("<could not read float32>");
    return false;
  }

  // Convert to APFloat using IEEE single precision semantics
  llvm::APInt apint(32, float_bits);
  llvm::APFloat apfloat(llvm::APFloat::IEEEsingle(), apint);

  // Use helper function with "s" suffix for float32
  lldb_private::formatters::oxcaml::helpers::FormatAPFloat(&stream, apfloat,
                                                          std::nullopt, "", "s");
  return true;
}

// Custom formatter registry: maps identifier strings to formatter functions
static const std::map<std::string, CustomTypeFormatter> custom_formatters = {
    {"_i", FormatInt32Custom},        // Int32.t
    {"_j", FormatInt64Custom},        // Int64.t
    {"_n", FormatNativeIntCustom},    // Nativeint.t
    {"_bigarr02", FormatBigarrayCustom},  // Bigarray
    {"_f32", FormatFloat32Custom}     // Float32
};

static bool FormatOxCamlCustom(Stream &stream, uint64_t value, uint64_t wosize,
                               DataExtractor& data, lldb::ProcessSP process_sp,
                               const ExecutionContextRef &exe_ctx_ref) {
  // OCaml custom block: dispatch to appropriate formatter based on identifier
  Status error;

  // Read custom_operations pointer from the first word
  lldb::addr_t custom_ops_ptr = process_sp->ReadPointerFromMemory(value, error);
  if (error.Fail()) {
    stream.Printf("<could not read struct custom_operations pointer from custom block>");
    return false;
  }

  // Read identifier pointer from the custom_operations struct (first field)
  lldb::addr_t identifier_ptr = process_sp->ReadPointerFromMemory(custom_ops_ptr, error);
  if (error.Fail()) {
    stream.Printf("<could not read identifier pointer from struct custom_operations>");
    return false;
  }

  // Read the identifier string from memory
  std::string identifier_str;
  if (!process_sp->ReadCStringFromMemory(identifier_ptr, identifier_str, error) || error.Fail()) {
    stream.Printf("<could not read identifier string from custom block>");
    return false;
  }

  // Look up the formatter in our registry
  auto formatter_it = custom_formatters.find(identifier_str);
  if (formatter_it != custom_formatters.end()) {
    // Found a specific formatter, pass data pointer (skipping the header)
    const uint64_t word_size = 8;
    uint64_t data_ptr = value + word_size;
    return formatter_it->second(stream, identifier_str, data_ptr, wosize, process_sp);
  } else {
    // Unknown custom type, use fallback format
    stream.Printf("<custom|\"%s\">", identifier_str.c_str());
    return true;
  }
}
