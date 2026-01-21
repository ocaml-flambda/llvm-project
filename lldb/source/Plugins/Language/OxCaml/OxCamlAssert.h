//===-- OxCamlAssert.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// \file
/// Defines a lightweight assertion macro shared across the OxCaml plugin.

#ifndef LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLASSERT_H
#define LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLASSERT_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include <string>

#define OX_ASSERT(COND, FMT, ...)                                              \
  do {                                                                         \
    if (!(COND)) {                                                             \
      std::string ox_msg = llvm::formatv(FMT, __VA_ARGS__).str();              \
      llvm::report_fatal_error(llvm::StringRef(ox_msg));                       \
    }                                                                          \
  } while (false)

#endif // LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLASSERT_H
