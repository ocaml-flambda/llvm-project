//===-- OxCamlValueFormatters.h ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// \file
/// This file provides formatting functions for OCaml values.
///
/// These functions handle the decoding and display of OCaml's tagged
/// pointer values, including immediate integers, heap-allocated structures,
/// and special OCaml runtime objects.

#ifndef LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLVALUEFORMATTERS_H
#define LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLVALUEFORMATTERS_H

#include "Plugins/TypeSystem/OxCaml/OxCamlTypes.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/Stream.h"
#include "lldb/lldb-private.h"

namespace lldb_private {
namespace formatters {
namespace oxcaml {

/// Format an OCaml boxed value (ocaml_value type).
/// \param stream Output stream to write to
/// \param data DataExtractor containing the 8-byte value
/// \param process_sp Process for memory access
/// \param exe_ctx_ref ExecutionContext for address resolution
/// \returns true if formatting succeeded, false otherwise
bool FormatOxCamlValue(Stream &stream, DataExtractor &data,
                       lldb::ProcessSP process_sp,
                       const ExecutionContextRef &exe_ctx_ref);

} // namespace oxcaml
} // namespace formatters
} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLVALUEFORMATTERS_H
