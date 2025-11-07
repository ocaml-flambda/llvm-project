//===-- LogChannelOxCaml.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_LOGCHANNELOXCAML_H
#define LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_LOGCHANNELOXCAML_H

#include "lldb/Utility/Log.h"
#include "llvm/ADT/BitmaskEnum.h"
#include "llvm/Support/FormatVariadic.h"

namespace lldb_private {

enum class OxCamlLog : Log::MaskType {
  TypeParsing = Log::ChannelFlag<0>,    // DWARF type parsing (base, typedef, enum)
  Functions = Log::ChannelFlag<1>,      // Function parsing and name resolution
  Formatting = Log::ChannelFlag<2>,     // Value formatting and display
  TypeRegistry = Log::ChannelFlag<3>,   // Type registry operations (add/lookup)
  Verbose = Log::ChannelFlag<4>,        // Verbose debugging information
  UserVisibleErrors = Log::ChannelFlag<5>,  // User-visible error diagnostics
  LLVM_MARK_AS_BITMASK_ENUM(UserVisibleErrors)
};
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

#define OXCAML_EXPLAIN_ERROR_MARKER(DISPLAY_TOKEN, FORMAT_STRING, ...)                       \
  do {                                                                                       \
    ::lldb_private::Log *oxcaml_log__ =                                                      \
        ::lldb_private::GetLog(::lldb_private::OxCamlLog::UserVisibleErrors);                \
    if (oxcaml_log__)                                                                        \
      LLDB_LOG(oxcaml_log__, "{0}; displaying {1} ({2})",                                    \
               llvm::formatv(FORMAT_STRING, __VA_ARGS__),                                    \
               (DISPLAY_TOKEN), __func__);                                                   \
  } while (false)

class LogChannelOxCaml {
public:
  static void Initialize();
  static void Terminate();
};

template <> Log::Channel &LogChannelFor<OxCamlLog>();
} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_LOGCHANNELOXCAML_H
