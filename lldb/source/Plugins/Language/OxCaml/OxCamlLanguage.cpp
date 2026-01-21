//===-- OxCamlLanguage.cpp --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OxCamlLanguage.h"
#include "LogChannelOxCaml.h"
#include "OxCamlFormatters.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/DataFormatters/DataVisualization.h"
#include "lldb/DataFormatters/TypeSummary.h"
#include "lldb/Target/Language.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Stream.h"

using namespace lldb;
using namespace lldb_private;

lldb::LanguageType OxCamlLanguage::GetLanguageType() const {
  return lldb::eLanguageTypeOCaml;
}

bool OxCamlLanguage::IsSourceFile(llvm::StringRef file_path) const {
  return file_path.ends_with_insensitive(".ml") ||
      file_path.ends_with_insensitive(".mli");
}

llvm::StringRef OxCamlLanguage::GetUserEntryPointName() const { return "main"; }

lldb::TypeCategoryImplSP OxCamlLanguage::GetFormatters() {
  static llvm::once_flag g_initialize;
  static TypeCategoryImplSP g_category;

  llvm::call_once(g_initialize, [this]() -> void {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log, "GetFormatters: Initializing OxCaml formatters");

    DataVisualization::Categories::GetCategory(ConstString(GetPluginName()),
                                               g_category);
    if (g_category) {
      LLDB_LOG(log, "GetFormatters: Successfully created category '{0}'",
               GetPluginName());

      TypeSummaryImpl::Flags flags;
      flags.SetCascades(true)
          .SetSkipPointers(false)
          .SetSkipReferences(false)
          .SetDontShowChildren(false)
          .SetDontShowValue(false);
      auto formatter = formatters::oxcaml::OxCamlValue_SummaryProvider;

      LLDB_LOG(log,
               "GetFormatters: Registering formatter for type 'ocaml_value'");
      g_category->AddTypeSummary(
          "ocaml_value", eFormatterMatchExact,
          TypeSummaryImplSP(new CXXFunctionSummaryFormat(
              flags, formatter, "OCaml value formatter")));
    }
  });

  return g_category;
}

void OxCamlLanguage::Initialize() {
  LogChannelOxCaml::Initialize();
  PluginManager::RegisterPlugin(GetPluginNameStatic(), "OxCaml Language",
                                CreateInstance);
}

void OxCamlLanguage::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
  LogChannelOxCaml::Terminate();
}

lldb_private::Language *
OxCamlLanguage::CreateInstance(lldb::LanguageType language) {
  if (language == lldb::eLanguageTypeOCaml) {
    return new OxCamlLanguage();
  }
  return nullptr;
}

llvm::StringRef OxCamlLanguage::GetPluginNameStatic() { return "oxcaml"; }

llvm::StringRef OxCamlLanguage::GetPluginName() {
  return GetPluginNameStatic();
}

LLDB_PLUGIN_DEFINE(OxCamlLanguage)
