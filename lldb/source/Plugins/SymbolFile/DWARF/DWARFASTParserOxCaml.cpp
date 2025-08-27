//===-- DWARFASTParserOxCaml.cpp -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DWARFASTParserOxCaml.h"

#include "DWARFDIE.h"
#include "DWARFDebugInfoEntry.h"
#include "DWARFDefines.h"
#include "SymbolFileDWARF.h"
#include "llvm/BinaryFormat/Dwarf.h"

#include "lldb/Symbol/Type.h"

#include "Plugins/TypeSystem/OxCaml/TypeSystemOxCaml.h"
#include "lldb/Core/Module.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Target/Language.h"
#include "lldb/Utility/Log.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::plugin::dwarf;

DWARFASTParserOxCaml::DWARFASTParserOxCaml(TypeSystemOxCaml &oxcaml_typesystem)
    : DWARFASTParser(Kind::DWARFASTParserOxCaml),
      m_oxcaml_typesystem(oxcaml_typesystem) {}

DWARFASTParserOxCaml::~DWARFASTParserOxCaml() = default;

lldb::TypeSP DWARFASTParserOxCaml::ParseTypeFromDWARF(const SymbolContext &sc,
                                                       const DWARFDIE &die,
                                                       bool *type_is_new_ptr) {
  // Add debug output
  fprintf(stderr, "OxCaml: ParseTypeFromDWARF called, tag=0x%x, name=%s\n", 
          die.Tag(), die.GetName() ? die.GetName() : "<none>");
  
  // Handle DW_TAG_base_type for basic OCaml types
  if (die.Tag() == llvm::dwarf::DW_TAG_base_type) {
    fprintf(stderr, "OxCaml: Processing DW_TAG_base_type\n");
    const char *name = die.GetName();
    if (!name)
      name = "ocaml_value";

    // Get the SymbolFileDWARF
    SymbolFileDWARF *dwarf = die.GetDWARF();
    if (!dwarf)
      return TypeSP();

    // Create a CompilerType from our TypeSystem
    CompilerType compiler_type = m_oxcaml_typesystem.GetTypeForFormatters(nullptr);

    // Create the Type object using MakeType
    fprintf(stderr, "OxCaml: Creating Type with name=%s\n", name);
    TypeSP type_sp = dwarf->MakeType(
        die.GetID(),
        ConstString(name),
        /*byte_size=*/ 8,  // OCaml values are 64-bit
        /*context=*/ nullptr,
        LLDB_INVALID_UID,
        Type::eEncodingIsUID,
        nullptr,
        compiler_type,
        Type::ResolveState::Full
    );

    if (type_is_new_ptr)
      *type_is_new_ptr = true;

    return type_sp;
  }

  // For other tags, return nullptr for now
  if (type_is_new_ptr)
    *type_is_new_ptr = false;
  return TypeSP();
}

ConstString
DWARFASTParserOxCaml::ConstructDemangledNameFromDWARF(const DWARFDIE &die) {
  // For now, just return empty - OCaml names are typically not mangled
  // in the same way as C++
  return ConstString();
}

Function *DWARFASTParserOxCaml::ParseFunctionFromDWARF(CompileUnit &comp_unit,
                                                        const DWARFDIE &die,
                                                        AddressRanges func_ranges) {
  if (die.Tag() != llvm::dwarf::DW_TAG_subprogram)
    return nullptr;

  const char *name = nullptr;
  const char *mangled = nullptr;
  std::optional<int> decl_file;
  std::optional<int> decl_line;
  std::optional<int> decl_column;
  std::optional<int> call_file;
  std::optional<int> call_line;
  std::optional<int> call_column;
  DWARFExpressionList frame_base;

  llvm::DWARFAddressRangesVector unused_ranges;

  if (!die.GetDIENamesAndRanges(name, mangled, unused_ranges, decl_file,
                                decl_line, decl_column, call_file, call_line,
                                call_column, &frame_base)) {
    return nullptr;
  }

  // Create the function name
  Mangled func_name;
  if (mangled && strlen(mangled) > 0)
    func_name.SetValue(ConstString(mangled));
  else if (name && strlen(name) > 0)
    func_name.SetValue(ConstString(name));
  else
    return nullptr; // Must have a name

  // Note: Declaration info is available but not used in minimal implementation

  const user_id_t func_user_id = die.GetID();

  // Get the function's base address
  Address func_addr;
  if (!func_ranges.empty()) {
    func_addr = func_ranges[0].GetBaseAddress();
  }

  // Create the Function object
  FunctionSP func_sp = std::make_shared<Function>(
      &comp_unit,           // CompileUnit
      func_user_id,         // UserID for function DIE
      func_user_id,         // UserID for function Type (same for now)
      func_name,            // Mangled function name
      nullptr,              // FunctionType (we'll add this later)
      std::move(func_addr), // Base address
      std::move(func_ranges) // All address ranges
  );

  // Note: Function doesn't store declaration directly - it's stored elsewhere

  return func_sp.get();
}

bool DWARFASTParserOxCaml::CompleteTypeFromDWARF(const DWARFDIE &die,
                                                  Type *type,
                                                  const CompilerType &compiler_type) {
  // Not implemented yet
  return false;
}

CompilerDecl DWARFASTParserOxCaml::GetDeclForUIDFromDWARF(const DWARFDIE &die) {
  // Not implemented yet - return invalid decl
  return CompilerDecl();
}

void DWARFASTParserOxCaml::EnsureAllDIEsInDeclContextHaveBeenParsed(
    CompilerDeclContext decl_context) {
  // Not implemented yet
}

CompilerDeclContext
DWARFASTParserOxCaml::GetDeclContextForUIDFromDWARF(const DWARFDIE &die) {
  // Not implemented yet - return invalid context
  return CompilerDeclContext();
}

CompilerDeclContext
DWARFASTParserOxCaml::GetDeclContextContainingUIDFromDWARF(const DWARFDIE &die) {
  // Not implemented yet - return invalid context
  return CompilerDeclContext();
}

std::string DWARFASTParserOxCaml::GetDIEClassTemplateParams(DWARFDIE die) {
  // OCaml doesn't have C++-style templates
  return {};
}
