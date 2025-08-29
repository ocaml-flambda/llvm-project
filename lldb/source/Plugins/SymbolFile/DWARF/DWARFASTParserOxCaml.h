//===-- DWARFASTParserOxCaml.h ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_SYMBOLFILE_DWARF_DWARFASTPARSEROXCAML_H
#define LLDB_SOURCE_PLUGINS_SYMBOLFILE_DWARF_DWARFASTPARSEROXCAML_H

#include "DWARFASTParser.h"
#include "DWARFDIE.h"

namespace lldb_private {
class TypeSystemOxCaml;
class OxCamlType;
}

class DWARFASTParserOxCaml : public lldb_private::plugin::dwarf::DWARFASTParser {
public:
  using DWARFDIE = lldb_private::plugin::dwarf::DWARFDIE;
  
  DWARFASTParserOxCaml(lldb_private::TypeSystemOxCaml &oxcaml_typesystem);

  ~DWARFASTParserOxCaml() override;

  // DWARFASTParser interface.
  lldb::TypeSP ParseTypeFromDWARF(const lldb_private::SymbolContext &sc,
                                  const DWARFDIE &die,
                                  bool *type_is_new_ptr) override;

  lldb_private::ConstString 
  ConstructDemangledNameFromDWARF(const DWARFDIE &die) override;

  lldb_private::Function *
  ParseFunctionFromDWARF(lldb_private::CompileUnit &comp_unit,
                         const DWARFDIE &die,
                         lldb_private::AddressRanges func_ranges) override;

  bool CompleteTypeFromDWARF(const DWARFDIE &die, 
                             lldb_private::Type *type,
                             const lldb_private::CompilerType &compiler_type) override;

  lldb_private::CompilerDecl GetDeclForUIDFromDWARF(const DWARFDIE &die) override;

  void EnsureAllDIEsInDeclContextHaveBeenParsed(
      lldb_private::CompilerDeclContext decl_context) override;

  lldb_private::CompilerDeclContext 
  GetDeclContextForUIDFromDWARF(const DWARFDIE &die) override;

  lldb_private::CompilerDeclContext
  GetDeclContextContainingUIDFromDWARF(const DWARFDIE &die) override;

  std::string GetDIEClassTemplateParams(DWARFDIE die) override;

  static bool classof(const DWARFASTParser *Parser) {
    return Parser->GetKind() == Kind::DWARFASTParserOxCaml;
  }

private:
  [[maybe_unused]] lldb_private::TypeSystemOxCaml &m_oxcaml_typesystem;
  
  // Helper methods for parsing different DWARF DIE types
  std::unique_ptr<lldb_private::OxCamlType> ParseBaseType(const DWARFDIE &die);
  std::unique_ptr<lldb_private::OxCamlType> ParseTypedefType(const lldb_private::SymbolContext &sc, const DWARFDIE &die);
  std::unique_ptr<lldb_private::OxCamlType> ParseEnumType(const DWARFDIE &die);
  lldb::TypeSP CreateLLDBType(const DWARFDIE &die, lldb_private::OxCamlType* oxcaml_type);
};

#endif // LLDB_SOURCE_PLUGINS_SYMBOLFILE_DWARF_DWARFASTPARSEROXCAML_H