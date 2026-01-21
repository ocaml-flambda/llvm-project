//===-- LLDBFormatHelpersForOxCaml.h ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_LLDBFORMATHELPERSFOROXCAML_H
#define LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_LLDBFORMATHELPERSFOROXCAML_H

#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/Stream.h"
#include "llvm/ADT/APInt.h"
#include <optional>
#include <string>

namespace lldb_private {
namespace formatters {
namespace oxcaml {
namespace helpers {

/// Extract an arbitrary precision integer from a DataExtractor.
/// Based on the upstream LLDB implementation in DumpDataExtractor.cpp.
/// Handles endianness correctly and supports any byte size.
std::optional<llvm::APInt> ExtractAPInt(const DataExtractor &data,
                                        lldb::offset_t *offset_ptr,
                                        lldb::offset_t byte_size);

/// Format an arbitrary precision integer to a stream in decimal format.
/// Based on the upstream LLDB implementation in DumpDataExtractor.cpp.
/// Supports signed/unsigned interpretation with OCaml sign conventions.
void FormatAPInt(Stream *stream, const llvm::APInt &apint, bool is_signed,
                 const std::string &prefix = "",
                 const std::string &suffix = "");

} // namespace helpers
} // namespace oxcaml
} // namespace formatters
} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_LLDBFORMATHELPERSFOROXCAML_H
