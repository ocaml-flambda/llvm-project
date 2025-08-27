//===-- OxCamlLanguage.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OxCamlLanguage.h"
#include "lldb/Target/Language.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/DataFormatters/DataVisualization.h"
#include "lldb/DataFormatters/TypeSummary.h"
#include "lldb/Utility/Stream.h"
#include <cinttypes>

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
    DataVisualization::Categories::GetCategory(ConstString(GetPluginName()),
                                               g_category);
    if (g_category) {
      // Create formatter for ocaml_value base type
      TypeSummaryImpl::Flags flags;
      flags.SetCascades(true)
           .SetSkipPointers(false)
           .SetSkipReferences(false)
           .SetDontShowChildren(false)
           .SetDontShowValue(false);

      auto formatter = [](ValueObject &valobj, Stream &stream,
                         const TypeSummaryOptions &options) -> bool {
        // Get the raw data directly
        DataExtractor data;
        Status error;
        uint64_t data_size = valobj.GetData(data, error);
        
        if (error.Success() && data_size >= 8) {
          lldb::offset_t offset = 0;
          uint64_t value = data.GetU64(&offset);
          
          // OCaml value interpretation
          if (value & 1) {
            // Immediate value - show as integer
            int64_t int_val = ((int64_t)value) >> 1;
            stream.Printf("%" PRId64, int_val);
          } else {
            // Pointer value
            if (value == 0) {
              stream.Printf("()"); // unit value
            } else {
              stream.Printf("<pointer: 0x%" PRIx64 ">", value);
            }
          }
        } else {
          stream.Printf("<unavailable>");
        }
        return true;
      };

      // Register formatter for base type "ocaml_value"
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
  PluginManager::RegisterPlugin(GetPluginNameStatic(),
                                "OxCaml Language",
                                CreateInstance);
}

void OxCamlLanguage::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
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
