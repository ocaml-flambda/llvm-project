//===-- OxCamlFormatHelpers.cpp ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// \file
/// This file implements helper functions for formatting integers and floats
/// using LLVM's arbitrary precision types (APInt, APFloat).

#include "OxCamlFormatHelpers.h"
#include "LogChannelOxCaml.h"
#include "OxCamlHelpers.h"
#include "lldb/Target/ExecutionContextScope.h"
#include "lldb/Target/Process.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Status.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters::oxcaml::helpers;
using namespace lldb_private::formatters::oxcaml::helpers::constants;

std::optional<FloatSize>
lldb_private::formatters::oxcaml::helpers::ByteSizeToFloatSize(
    uint64_t byte_size) {
  switch (byte_size) {
  case FLOAT16_SIZE:
    return FloatSize::Half;
  case FLOAT32_SIZE:
    return FloatSize::Single;
  case FLOAT64_SIZE:
    return FloatSize::Double;
  default:
    return std::nullopt;
  }
}

// Helper function to determine if float needs trailing ".0"
// Returns true if the string contains only digits and minus sign
static bool NeedsTrailingDotZero(llvm::StringRef str) {
  return str.find_first_not_of("-0123456789") == llvm::StringRef::npos;
}

// Format APFloat with minimal precision that preserves exact value
// Tries progressively higher precisions (5, 12, 15) and uses the first
// one that can exactly represent the original float value.
// Falls back to full precision if none of the shorter ones work.
// Based on oxcaml-lldb fork's APFloat.h extension.
static void
FormatFloatWithMinimalPrecision(const llvm::APFloat &apfloat,
                                llvm::SmallVectorImpl<char> &Str,
                                std::optional<unsigned> format_max_padding) {
  unsigned FormatMaxPadding = format_max_padding.value_or(3);
  bool TruncateZero = true;

  // Try these precision levels to find the shortest accurate representation
  static const unsigned precisions[] = {5, 12, 15};
  static const size_t num_precisions =
      sizeof(precisions) / sizeof(precisions[0]);

  for (size_t i = 0; i < num_precisions; ++i) {
    apfloat.toString(Str, precisions[i], FormatMaxPadding, TruncateZero);
    llvm::StringRef sr = llvm::StringRef(Str.data(), Str.size());
    llvm::APFloat maybe_self = llvm::APFloat(apfloat.getSemantics(), sr);
    // We have found an identical float at this precision
    if (maybe_self.bitwiseIsEqual(apfloat)) {
      return;
    } else {
      Str.clear();
    }
  }

  // Default back to full precision if shorter ones don't work
  apfloat.toString(Str, 0, FormatMaxPadding, TruncateZero);
}

// Based on PrintAPIntAsFloat from oxcaml-lldb fork's DumpDataExtractor.cpp
void lldb_private::formatters::oxcaml::helpers::FormatAPFloat(
    Stream *stream, const llvm::APFloat &apfloat,
    std::optional<unsigned> format_max_padding, const std::string &prefix,
    const std::string &suffix) {
  if (!stream)
    return;

  llvm::SmallVector<char, 256> sv;
  FormatFloatWithMinimalPrecision(apfloat, sv, format_max_padding);

  // Handle negative sign placement for OCaml format
  if (sv.size() > 0 && sv[0] == '-') {
    stream->PutChar('-');
    stream->Write(prefix.data(), prefix.size());
    stream->Write(sv.data() + 1, sv.size() - 1);
  } else {
    stream->Write(prefix.data(), prefix.size());
    stream->Write(sv.data(), sv.size());
  }

  // Following OCaml conventions, print the trailing ".0" to
  // identify that the integer is in fact a float, but don't
  // print any trailing zeros beyond that.
  if (NeedsTrailingDotZero(llvm::StringRef(sv.data(), sv.size()))) {
    stream->PutCString(".0");
  }

  stream->Write(suffix.data(), suffix.size());
}

std::optional<llvm::APFloat>
lldb_private::formatters::oxcaml::helpers::ExtractAPFloat(
    const DataExtractor &data, lldb::offset_t *offset_ptr,
    FloatSize float_size) {
  const llvm::fltSemantics *semantics;
  switch (float_size) {
  case FloatSize::Half:
    semantics = &llvm::APFloat::IEEEhalf();
    break;
  case FloatSize::Single:
    semantics = &llvm::APFloat::IEEEsingle();
    break;
  case FloatSize::Double:
    semantics = &llvm::APFloat::IEEEdouble();
    break;
  }

  size_t byte_size = static_cast<size_t>(float_size);
  std::optional<llvm::APInt> apint = ExtractAPInt(data, offset_ptr, byte_size);
  if (!apint) {
    return std::nullopt;
  }

  return llvm::APFloat(*semantics, *apint);
}

// Helper function to dump character with OCaml string literal escaping
// Based on DumpEscapedCharacterOCaml from the oxcaml-lldb fork
static void FormatOCamlCharacter(Stream &s, const char c) {
  switch (c) {
  case '"':
    s.Printf("\\\"");
    return;
  case '\\':
    s.Printf("\\\\");
    return;
  case '\n':
    s.Printf("\\n");
    return;
  case '\t':
    s.Printf("\\t");
    return;
  case '\r':
    s.Printf("\\r");
    return;
  case '\b':
    s.Printf("\\b");
    return;
  default:
    break;
  }

  // Handle printable ASCII range ' ' to '~' (32 to 126)
  if (c >= ' ' && c <= '~') {
    s.PutChar(c);
    return;
  }

  // Use OCaml's 3-digit decimal escape format for non-printable characters
  // This matches the logic in bytes.ml unsafe_escape function
  unsigned char a = (unsigned char)c;
  s.Printf("\\%03d", a);
}

// Format OCaml string with proper escaping and quotes
// Based on DumpStringOCaml from the oxcaml-lldb fork's DumpDataExtractor.cpp
void lldb_private::formatters::oxcaml::helpers::FormatOCamlString(
    Stream *stream, const char *data, uint64_t string_length) {
  if (!stream)
    return;

  stream->Printf("\"");
  for (uint64_t i = 0; i < string_length; ++i) {
    FormatOCamlCharacter(*stream, data[i]);
  }
  stream->Printf("\"");
}

// Read OCaml string data from process memory
// Core implementation shared by FormatOxCamlString and ReadOCamlString
std::optional<std::string>
lldb_private::formatters::oxcaml::helpers::ReadOCamlStringData(
    lldb::addr_t data_addr, uint64_t wosize, lldb::ProcessSP process_sp) {
  Status error;
  Log *log = GetLog(OxCamlLog::Formatting);

  if (!process_sp)
    return std::nullopt;

  if (wosize == 0)
    return std::nullopt;

  // Read last word to extract padding byte
  uint64_t last_word_address = header::GetLastWordAddress(data_addr, wosize);
  uint64_t last_word = process_sp->ReadUnsignedIntegerFromMemory(
      last_word_address, constants::WORD_SIZE, 0, error);

  if (error.Fail()) {
    LLDB_LOG(log, "Failed to read last word at address 0x{0:x}: {1}",
             last_word_address, error.AsCString());
    return std::nullopt;
  }

  // Determine string length and read from memory
  uint8_t padding_byte = string::ExtractPaddingByte(last_word);
  if (padding_byte >= constants::WORD_SIZE)
    return std::nullopt;

  uint64_t string_length = string::CalculateStringLength(wosize, padding_byte);

  std::vector<uint8_t> str_buffer(string_length);
  size_t bytes_read = 0;
  if (string_length > 0)
    bytes_read = process_sp->ReadMemory(data_addr, str_buffer.data(),
                                        string_length, error);

  if (error.Fail() || bytes_read < string_length) {
    LLDB_LOG(log,
             "Failed to read string data at address 0x{0:x}, expected {1} "
             "bytes, got {2}: {3}",
             data_addr, string_length, bytes_read, error.AsCString());
    return std::nullopt;
  }

  // std::string handles null-bytes correctly, because it tracks the length
  // of the string.
  return std::string(reinterpret_cast<const char *>(str_buffer.data()),
                     string_length);
}

// Read OCaml block header from memory
std::optional<uint64_t>
lldb_private::formatters::oxcaml::helpers::ReadBlockHeader(
    lldb::addr_t block_ptr, lldb::ProcessSP process_sp) {
  Status error;
  lldb::addr_t header_addr = header::GetHeaderAddress(block_ptr);
  uint64_t header = process_sp->ReadUnsignedIntegerFromMemory(
      header_addr, constants::WORD_SIZE, 0, error);

  if (error.Fail()) {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log, "Failed to read block header at address 0x{0:x}: {1}",
             header_addr, error.AsCString());
    return std::nullopt;
  }

  return header;
}
