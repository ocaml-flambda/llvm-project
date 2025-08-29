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

namespace lldb_private {

enum class OxCamlLog : Log::MaskType {
  TypeParsing = Log::ChannelFlag<0>,    // DWARF type parsing (base, typedef, enum)
  Functions = Log::ChannelFlag<1>,      // Function parsing and name resolution
  Formatting = Log::ChannelFlag<2>,     // Value formatting and display
  TypeRegistry = Log::ChannelFlag<3>,   // Type registry operations (add/lookup)
  Verbose = Log::ChannelFlag<4>,        // Verbose debugging information
  LLVM_MARK_AS_BITMASK_ENUM(Verbose)
};
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

class LogChannelOxCaml {
public:
  static void Initialize();
  static void Terminate();
};

template <> Log::Channel &LogChannelFor<OxCamlLog>();
} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_LOGCHANNELOXCAML_H