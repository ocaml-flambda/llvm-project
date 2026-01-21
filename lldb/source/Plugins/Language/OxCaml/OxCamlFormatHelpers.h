//===-- OxCamlFormatHelpers.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// \file
/// This file provides helper functions for formatting integers and floats
/// using LLVM's arbitrary precision types (APInt, APFloat).
///
/// These helpers are shared between unboxed value formatting and OCaml
/// value decoding to ensure consistent number formatting across the
/// OCaml LLDB plugin.

#ifndef LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLFORMATHELPERS_H
#define LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLFORMATHELPERS_H

#include "LLDBFormatHelpersForOxCaml.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/Stream.h"
#include "lldb/lldb-private.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include <optional>
#include <string>

namespace lldb_private {
namespace formatters {
namespace oxcaml {
namespace helpers {

/// Float size specification for IEEE semantics selection
enum class FloatSize : unsigned {
  Half = 2,   ///< IEEE half precision (float16#)
  Single = 4, ///< IEEE single precision (float32#)
  Double = 8  ///< IEEE double precision (float#)
};

/// Convert byte size to FloatSize enum.
/// \param byte_size Size in bytes (2, 4, or 8)
/// \returns FloatSize enum value if valid size, nullopt otherwise
std::optional<FloatSize> ByteSizeToFloatSize(uint64_t byte_size);

/// Format an arbitrary precision float to a stream with OCaml conventions.
/// Handles OCaml-specific formatting like negative sign placement and
/// trailing ".0" for integer-looking floats.
/// \param stream Output stream to write to
/// \param apfloat The APFloat value to format
/// \param format_max_padding Optional maximum zero padding for formatting
/// \param prefix String to prepend (e.g., "#" for unboxed floats)
/// \param suffix String to append (e.g., "s" for float32)
void FormatAPFloat(Stream *stream, const llvm::APFloat &apfloat,
                   std::optional<unsigned> format_max_padding = std::nullopt,
                   const std::string &prefix = "",
                   const std::string &suffix = "");

/// Extract an APFloat from a DataExtractor using IEEE semantics.
/// Automatically selects the correct IEEE semantics based on float size.
/// Supports OCaml float types:
///   - Half: IEEE half precision (future float16# support)
///   - Single: IEEE single precision (float32# @ float32)
///   - Double: IEEE double precision (float# @ float64)
/// \param data The DataExtractor to read from
/// \param offset_ptr Pointer to offset, updated after reading
/// \param float_size Float precision specification
/// \returns APFloat if successful, nullopt if unsupported size or read fails
std::optional<llvm::APFloat> ExtractAPFloat(const DataExtractor &data,
                                            lldb::offset_t *offset_ptr,
                                            FloatSize float_size);

/// Format an OCaml string with proper escaping and quotes.
/// Uses OCaml's string literal format matching bytes.ml unsafe_escape function.
/// \param stream Output stream to write to
/// \param data Pointer to string data
/// \param string_length Number of characters in string
void FormatOCamlString(Stream *stream, const char *data,
                       uint64_t string_length);

/// Read OCaml string data from process memory.
/// Reads the string data from a String_tag OCaml block in memory.
/// Handles padding byte extraction and string length calculation.
/// Does NOT perform tag validation - caller must ensure address points to
/// string data.
/// \param data_addr Address of string data (NOT including header)
/// \param wosize Word size from block header
/// \param process_sp Process to read from
/// \returns String contents if successful, std::nullopt on read failure
std::optional<std::string> ReadOCamlStringData(lldb::addr_t data_addr,
                                               uint64_t wosize,
                                               lldb::ProcessSP process_sp);

/// Read OCaml block header from memory.
/// This is a convenience function that combines header address calculation
/// and memory reading into a single operation with proper error handling.
/// \param block_ptr Address of OCaml block data (pointer points to data, not
/// header) \param process_sp Process to read from \returns Header word value
/// if successful, std::nullopt on read failure
std::optional<uint64_t> ReadBlockHeader(lldb::addr_t block_ptr,
                                        lldb::ProcessSP process_sp);

} // namespace helpers
} // namespace oxcaml
} // namespace formatters
} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLFORMATHELPERS_H
