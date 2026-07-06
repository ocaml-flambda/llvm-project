//===-- OxCamlLanguage.h ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLLANGUAGE_H
#define LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLLANGUAGE_H

#include "lldb/Target/Language.h"
#include "lldb/Utility/FileSpec.h"
#include "lldb/lldb-private.h"

namespace lldb_private {

class OxCamlLanguage : public Language {
public:
  OxCamlLanguage() = default;

  ~OxCamlLanguage() override = default;

  lldb::LanguageType GetLanguageType() const override;

  bool IsSourceFile(llvm::StringRef file_path) const override;

  llvm::StringRef GetUserEntryPointName() const override;

  lldb::TypeCategoryImplSP GetFormatters() override;

  llvm::Expected<std::string>
  GetExternalFormatterInput(ValueObject &valobj,
                            std::vector<uint8_t> &data) override;

  /// The plugin.oxcaml.display.external-summary-executable setting: the
  /// external pretty-printer used before falling back to the built-in
  /// formatter.  Empty when not configured.
  static FileSpec GetExternalSummaryExecutable();

  static void Initialize();

  static void Terminate();

  static lldb_private::Language *CreateInstance(lldb::LanguageType language);

  static void DebuggerInitialize(Debugger &debugger);

  static llvm::StringRef GetPluginNameStatic();

  llvm::StringRef GetPluginName() override;

  bool ShouldShowTypesInFunctionArguments() const override { return true; }
};

} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLLANGUAGE_H
