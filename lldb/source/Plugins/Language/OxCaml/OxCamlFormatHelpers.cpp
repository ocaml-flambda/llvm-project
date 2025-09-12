//===-- OxCamlFormatHelpers.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// \file
/// This file implements helper functions for formatting integers and floats
/// using LLVM's arbitrary precision types (APInt, APFloat).
///
/// The implementations are based on LLVM's DumpDataExtractor.cpp with
/// OCaml-specific formatting conventions.

#include "OxCamlFormatHelpers.h"
#include "lldb/Target/ExecutionContextScope.h"
#include "lldb/Target/Target.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters::oxcaml::helpers;


// Helper function to determine if float needs trailing ".0"
// Returns true if the string contains only digits and minus sign
static bool NeedsTrailingDotZero(llvm::StringRef str) {
  return str.find_first_not_of("-0123456789") == llvm::StringRef::npos;
}

// Based on GetAPInt/DumpAPInt from upstream LLDB's DumpDataExtractor.cpp
std::optional<llvm::APInt> 
lldb_private::formatters::oxcaml::helpers::ExtractAPInt(const DataExtractor &data,
                                                        lldb::offset_t *offset_ptr,
                                                        lldb::offset_t byte_size) {
  if (byte_size == 0)
    return std::nullopt;

  llvm::SmallVector<uint64_t, 2> uint64_array;
  lldb::offset_t bytes_left = byte_size;
  uint64_t u64;
  const lldb::ByteOrder byte_order = data.GetByteOrder();
  
  if (byte_order == lldb::eByteOrderLittle) {
    while (bytes_left > 0) {
      if (bytes_left >= 8) {
        u64 = data.GetU64(offset_ptr);
        bytes_left -= 8;
      } else {
        u64 = data.GetMaxU64(offset_ptr, (uint32_t)bytes_left);
        bytes_left = 0;
      }
      uint64_array.push_back(u64);
    }
    return llvm::APInt(byte_size * 8, llvm::ArrayRef<uint64_t>(uint64_array));
  } else if (byte_order == lldb::eByteOrderBig) {
    lldb::offset_t be_offset = *offset_ptr + byte_size;
    lldb::offset_t temp_offset;
    while (bytes_left > 0) {
      if (bytes_left >= 8) {
        be_offset -= 8;
        temp_offset = be_offset;
        u64 = data.GetU64(&temp_offset);
        bytes_left -= 8;
      } else {
        be_offset -= bytes_left;
        temp_offset = be_offset;
        u64 = data.GetMaxU64(&temp_offset, (uint32_t)bytes_left);
        bytes_left = 0;
      }
      uint64_array.push_back(u64);
    }
    *offset_ptr += byte_size;
    return llvm::APInt(byte_size * 8, llvm::ArrayRef<uint64_t>(uint64_array));
  }
  return std::nullopt;
}

// Based on GetAPInt/DumpAPInt from upstream LLDB's DumpDataExtractor.cpp
void lldb_private::formatters::oxcaml::helpers::FormatAPInt(Stream *stream, 
                                                            const llvm::APInt &apint,
                                                            bool is_signed,
                                                            const std::string &prefix,
                                                            const std::string &suffix) {
  if (!stream)
    return;
    
  std::string apint_str = llvm::toString(apint, 10, is_signed);
  
  // OCaml Specific:
  // Handle negative sign placement for OCaml format
  if (apint_str.size() > 0 && apint_str[0] == '-') {
    stream->PutChar('-');
    stream->Write(prefix.data(), prefix.size());
    stream->Write(apint_str.data() + 1, apint_str.size() - 1);
  } else {
    stream->Write(prefix.data(), prefix.size());
    stream->Write(apint_str.data(), apint_str.size());
  }
  
  stream->Write(suffix.data(), suffix.size());
}

// Format APFloat with minimal precision that preserves exact value
// Tries progressively higher precisions (5, 12, 15) and uses the first
// one that can exactly represent the original float value.
// Falls back to full precision if none of the shorter ones work.
// Based on oxcaml-lldb fork's APFloat.h extension.
static void FormatFloatWithMinimalPrecision(const llvm::APFloat &apfloat, 
                                            llvm::SmallVectorImpl<char> &Str,
                                            std::optional<unsigned> format_max_padding) {
  unsigned FormatMaxPadding = format_max_padding.value_or(3);
  bool TruncateZero = true;
  
  // Try these precision levels to find the shortest accurate representation
  static const unsigned precisions[] = { 5, 12, 15 };
  static const size_t num_precisions = sizeof(precisions) / sizeof(precisions[0]);
  
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
void lldb_private::formatters::oxcaml::helpers::FormatAPFloat(Stream *stream, 
                                                              const llvm::APFloat &apfloat,
                                                              std::optional<unsigned> format_max_padding,
                                                              const std::string &prefix,
                                                              const std::string &suffix) {
  if (!stream)
    return;
    
  llvm::SmallVector<char, 256> sv;
  FormatFloatWithMinimalPrecision(apfloat, sv, format_max_padding);

  // OCaml Specific:
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
lldb_private::formatters::oxcaml::helpers::ExtractAPFloat(const DataExtractor &data,
                                                          lldb::offset_t *offset_ptr,
                                                          FloatSize float_size) {
  // Select IEEE semantics based on float size
  const llvm::fltSemantics *semantics;
  switch (float_size) {
    case FloatSize::Half:
      semantics = &llvm::APFloat::IEEEhalf();    // Future float16# support
      break;
    case FloatSize::Single:
      semantics = &llvm::APFloat::IEEEsingle();  // float32# @ float32
      break;
    case FloatSize::Double:
      semantics = &llvm::APFloat::IEEEdouble();  // float# @ float64
      break;
  }
  
  // Get the raw bits as APInt first
  size_t byte_size = static_cast<size_t>(float_size);
  std::optional<llvm::APInt> apint = ExtractAPInt(data, offset_ptr, byte_size);
  if (!apint)
    return std::nullopt;
    
  // Convert APInt to APFloat using the selected IEEE semantics
  return llvm::APFloat(*semantics, *apint);
}

