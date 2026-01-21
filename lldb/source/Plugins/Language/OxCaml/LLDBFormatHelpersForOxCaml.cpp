//===-- LLDBFormatHelpersForOxCaml.cpp -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "LLDBFormatHelpersForOxCaml.h"

#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/Stream.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"

using namespace lldb;
using namespace lldb_private;

// Based on GetAPInt/DumpAPInt from upstream LLDB's DumpDataExtractor.cpp
std::optional<llvm::APInt>
lldb_private::formatters::oxcaml::helpers::ExtractAPInt(
    const DataExtractor &data, lldb::offset_t *offset_ptr,
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

// Based loosely on GetAPInt/DumpAPInt from upstream LLDB's
// DumpDataExtractor.cpp
void lldb_private::formatters::oxcaml::helpers::FormatAPInt(
    Stream *stream, const llvm::APInt &apint, bool is_signed,
    const std::string &prefix, const std::string &suffix) {
  if (!stream)
    return;

  std::string apint_str = llvm::toString(apint, 10, is_signed);

  if (!apint_str.empty() && apint_str[0] == '-') {
    stream->PutChar('-');
    stream->Write(prefix.data(), prefix.size());
    stream->Write(apint_str.data() + 1, apint_str.size() - 1);
  } else {
    stream->Write(prefix.data(), prefix.size());
    stream->Write(apint_str.data(), apint_str.size());
  }

  stream->Write(suffix.data(), suffix.size());
}
