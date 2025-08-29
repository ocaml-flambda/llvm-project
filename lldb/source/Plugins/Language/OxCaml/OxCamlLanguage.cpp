//===-- OxCamlLanguage.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OxCamlLanguage.h"
#include "OxCamlFormatters.h"
#include "LogChannelOxCaml.h"
#include "lldb/Target/Language.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/DataFormatters/DataVisualization.h"
#include "lldb/DataFormatters/TypeSummary.h"
#include "lldb/Utility/Stream.h"
#include "lldb/Utility/Log.h"

using namespace lldb;
using namespace lldb_private;

lldb::LanguageType OxCamlLanguage::GetLanguageType() const {
  return lldb::eLanguageTypeOCaml;
}

bool OxCamlLanguage::IsSourceFile(llvm::StringRef file_path) const {
  const auto suffixes = {".ml", ".mli"};
  for (auto suffix : suffixes) {
    if (file_path.ends_with_insensitive(suffix))
      return true;
  }
  return false;
}

llvm::StringRef OxCamlLanguage::GetUserEntryPointName() const {
  return "main";
}

lldb::TypeCategoryImplSP OxCamlLanguage::GetFormatters() {
  static llvm::once_flag g_initialize;
  static TypeCategoryImplSP g_category;

  llvm::call_once(g_initialize, [this]() -> void {
    Log *log = GetLog(OxCamlLog::Formatting);
    LLDB_LOG(log, "GetFormatters: Initializing OxCaml formatters");

    DataVisualization::Categories::GetCategory(ConstString(GetPluginName()),
                                               g_category);
    if (g_category) {
      LLDB_LOG(log, "GetFormatters: Successfully created category '{0}'", GetPluginName());
      // Create formatter for ocaml_value base type
      TypeSummaryImpl::Flags flags;
      flags.SetCascades(true)
           .SetSkipPointers(false)
           .SetSkipReferences(false)
           .SetDontShowChildren(false)
           .SetDontShowValue(false);

      // Use the formatter from OxCamlFormatters
      auto formatter = formatters::oxcaml::OxCamlValue_SummaryProvider;

      // Register formatter for base type "ocaml_value"
      LLDB_LOG(log, "GetFormatters: Registering formatter for type 'ocaml_value'");
      g_category->AddTypeSummary(
          "ocaml_value",
          eFormatterMatchExact,
          TypeSummaryImplSP(new CXXFunctionSummaryFormat(
              flags, formatter, "OCaml value formatter"))
      );
    }
  });

  return g_category;
}

void OxCamlLanguage::Initialize() {
  LogChannelOxCaml::Initialize();
  PluginManager::RegisterPlugin(GetPluginNameStatic(),
                                "OxCaml Language",
                                CreateInstance);
}

void OxCamlLanguage::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
  LogChannelOxCaml::Terminate();
}

lldb_private::Language *OxCamlLanguage::CreateInstance(lldb::LanguageType language) {
  if (language == lldb::eLanguageTypeOCaml)
    return new OxCamlLanguage();
  return nullptr;
}

llvm::StringRef OxCamlLanguage::GetPluginNameStatic() {
  return "oxcaml";
}

llvm::StringRef OxCamlLanguage::GetPluginName() {
  return GetPluginNameStatic();
}

LLDB_PLUGIN_DEFINE(OxCamlLanguage)
