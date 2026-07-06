//===-- OxCamlMarshal.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A marshaller ("extern"), producing OCaml's Marshal wire format, that walks
// an OCaml object graph living in a debugged process rather than in the
// marshaller's own address space.  Adapted from the OxCaml runtime's
// runtime/extern.c (via extern_standalone.cpp in this repository); all reads
// of the debuggee's heap go through a MarshalMemoryReader instead of raw
// pointer dereferences.
//
// The output is accepted by Marshal.from_channel / Marshal.from_bytes in an
// OxCaml program, which is how the external pretty-printer scheme consumes
// it (see OxCamlExternalPrinter.cpp).
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLMARSHAL_H
#define LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLMARSHAL_H

#include "lldb/lldb-forward.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <vector>

namespace lldb_private {
namespace formatters {
namespace oxcaml {

/// Access to the debuggee's memory for the marshaller.  Reads go through
/// LLDB rather than raw pointers so that the object graph can live in a
/// debugged process; other implementations (for example one materializing
/// values described by DWARF expressions, such as implicit pointers) can be
/// substituted without touching the marshaller itself.
class MarshalMemoryReader {
public:
  virtual ~MarshalMemoryReader() = default;

  /// Read \p len bytes at target address \p addr into \p buf.  Returns
  /// false if any part of the range cannot be read.
  virtual bool ReadBytes(uint64_t addr, void *buf, uint64_t len) = 0;

  /// Read one 8-byte word at target address \p addr.  The default
  /// implementation reads the bytes and assumes the debuggee shares the
  /// host's byte order (true for all supported OxCaml targets).
  virtual bool ReadWord(uint64_t addr, uint64_t &word);
};

/// A MarshalMemoryReader that reads from a live process.
class ProcessMemoryReader : public MarshalMemoryReader {
public:
  explicit ProcessMemoryReader(lldb::ProcessSP process_sp)
      : m_process_sp(std::move(process_sp)) {}

  bool ReadBytes(uint64_t addr, void *buf, uint64_t len) override;

private:
  lldb::ProcessSP m_process_sp;
};

/// Marshal the OCaml value \p root (a tagged word: an immediate, the null
/// value, or a pointer to a heap block) into OCaml's Marshal wire format,
/// following heap pointers through \p reader.  On success the returned
/// buffer holds the complete marshalled representation, header included,
/// as expected by Marshal.from_channel.  Sharing within the value is
/// preserved.
///
/// Values with no external representation (closures, continuations,
/// custom / abstract / mixed blocks), unreadable memory, and outputs that
/// would exceed \p max_output_bytes all yield an error.
llvm::Expected<std::vector<uint8_t>>
MarshalOCamlValue(uint64_t root, MarshalMemoryReader &reader,
                  uint64_t max_output_bytes = 64 * 1024 * 1024);

} // namespace oxcaml
} // namespace formatters
} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLMARSHAL_H
