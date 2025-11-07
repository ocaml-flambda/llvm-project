//===-- LogChannelOxCaml.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "LogChannelOxCaml.h"

using namespace lldb_private;

static constexpr Log::Category g_categories[] = {
    {{"types"}, {"log DWARF type parsing"}, OxCamlLog::TypeParsing},
    {{"functions"},
     {"log function parsing and name resolution"},
     OxCamlLog::Functions},
    {{"formatting"},
     {"log value formatting and display operations"},
     OxCamlLog::Formatting},
    {{"registry"}, {"log type registry operations"}, OxCamlLog::TypeRegistry},
    {{"verbose"}, {"log verbose debugging information"}, OxCamlLog::Verbose},
    {{"user-visible-errors"},
     {"log user-visible error diagnostics"},
     OxCamlLog::UserVisibleErrors},
};

static Log::Channel g_channel(g_categories, OxCamlLog::TypeParsing);

template <> Log::Channel &lldb_private::LogChannelFor<OxCamlLog>() {
  return g_channel;
}

void LogChannelOxCaml::Initialize() { Log::Register("oxcaml", g_channel); }

void LogChannelOxCaml::Terminate() { Log::Unregister("oxcaml"); }
