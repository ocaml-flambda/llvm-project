//===-- OxCamlExternalPrinter.h ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Support for pretty-printing OCaml values with a user-provided external
// program.  The value is marshalled out of the debuggee (see OxCamlMarshal.h)
// and sent to the program on its standard input, together with the OCaml
// type name as the program's only argument; the program demarshals the value
// (Marshal.from_channel), prints its rendering on standard output and exits
// with code 0.  A nonzero exit code means "type not recognized" and makes
// the caller fall back to the built-in formatter.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLEXTERNALPRINTER_H
#define LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLEXTERNALPRINTER_H

#include "lldb/lldb-forward.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <string>
#include <vector>

namespace lldb_private {

class Stream;
class ValueObject;

namespace formatters {
namespace oxcaml {

/// Serialize \p valobj for an external summary program: marshal the OCaml
/// value into \p data and return the OCaml type name to pass to the program
/// (the display type name with its layout annotation stripped, e.g.
/// "Env.t @ value" becomes "Env.t").  Fails for values that are not
/// represented as tagged OCaml words (unboxed primitives), for values
/// without a live process, and for object graphs the marshaller cannot
/// represent (closures, custom blocks, unreadable memory, ...).
llvm::Expected<std::string>
GetOCamlExternalFormatterInput(ValueObject &valobj, std::vector<uint8_t> &data);

/// If the user has configured an external pretty-printer executable
/// (settings set plugin.oxcaml.display.external-summary-executable <path>),
/// try to use it for \p valobj, writing its output to \p stream.  Returns
/// true on success; any failure returns false (with the reason logged to
/// the "oxcaml formatting" log channel) so the caller can fall back to the
/// built-in formatter.
bool TryExternalPrettyPrinter(ValueObject &valobj, Stream &stream);

} // namespace oxcaml
} // namespace formatters
} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLEXTERNALPRINTER_H
