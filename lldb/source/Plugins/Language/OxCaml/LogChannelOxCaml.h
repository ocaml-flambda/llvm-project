//===-- LogChannelOxCaml.h --------------------------------------*- C++ -*-===//
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
#include <string>

namespace lldb_private {

enum class OxCamlLog : Log::MaskType {
  TypeParsing =
      Log::ChannelFlag<0>, // DWARF type parsing (base, typedef, enum, etc.)
  FunctionParsing = Log::ChannelFlag<1>, // DWARF function parsing (subprograms)
  Formatting = Log::ChannelFlag<2>,      // Value formatting and display
  TypeRegistry = Log::ChannelFlag<3>, // Type registry operations (add/lookup)
  UserVisibleErrors = Log::ChannelFlag<4>, // User-visible error diagnostics
  LLVM_MARK_AS_BITMASK_ENUM(UserVisibleErrors)
};
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

#define OXCAML_EXPLAIN_ERROR_MARKER(DISPLAY_TOKEN, FORMAT_STRING, ...)         \
  do {                                                                         \
    /* The format arguments must be evaluated exactly once, whether or not     \
       the log channel is enabled: call sites pass expressions with side       \
       effects, such as llvm::toString(expected.takeError()), which must run   \
       to consume the error (an unconsumed llvm::Expected error aborts). */    \
    std::string oxcaml_explanation__ =                                         \
        llvm::formatv(FORMAT_STRING, __VA_ARGS__).str();                       \
    ::lldb_private::Log *oxcaml_log__ =                                        \
        ::lldb_private::GetLog(::lldb_private::OxCamlLog::UserVisibleErrors);  \
    if (oxcaml_log__)                                                          \
      LLDB_LOG(oxcaml_log__, "{0}; displaying {1} ({2})", oxcaml_explanation__, \
               (DISPLAY_TOKEN), __func__);                                     \
  } while (false)

#define OXCAML_EMIT_MARKER(STREAM, MARKER_EXPR, ERROR_FMT, ...)                \
  do {                                                                         \
    /* Keep the formatted marker alive while logging/printing; taking          \
       c_str() on a temporary would leave a dangling pointer. */               \
    std::string oxcaml_marker_storage__ = (MARKER_EXPR);                       \
    const char *oxcaml_marker__ = oxcaml_marker_storage__.c_str();             \
    OXCAML_EXPLAIN_ERROR_MARKER(oxcaml_marker__, ERROR_FMT, __VA_ARGS__);      \
    (STREAM).PutCString(oxcaml_marker__);                                      \
  } while (false)

#define OXCAML_CONDITIONALLY_EMIT_MARKER_AND_RETURN(                           \
    STREAM, CONDITION, RETURN_VALUE, MARKER_EXPR, ERROR_FMT, ...)              \
  do {                                                                         \
    if (!(CONDITION)) {                                                        \
      OXCAML_EMIT_MARKER(STREAM, MARKER_EXPR, ERROR_FMT, __VA_ARGS__);         \
      return (RETURN_VALUE);                                                   \
    }                                                                          \
  } while (false)

class LogChannelOxCaml {
public:
  static void Initialize();
  static void Terminate();
};

template <> Log::Channel &LogChannelFor<OxCamlLog>();
} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_LOGCHANNELOXCAML_H
