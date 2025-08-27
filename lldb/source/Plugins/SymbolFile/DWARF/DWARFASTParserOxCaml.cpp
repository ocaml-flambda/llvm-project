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

    // Get the type pointer from TypeSystem and use it
    void* type_ptr = m_oxcaml_typesystem.GetOxCamlValueType();
    fprintf(stderr, "OxCaml: Got type_ptr=%p from GetOxCamlValueType\n", type_ptr);
    CompilerType compiler_type = m_oxcaml_typesystem.GetTypeForFormatters(type_ptr);

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

  // Debug output for function names
  fprintf(stderr, "OxCaml: ParseFunctionFromDWARF - name='%s', mangled='%s'\n",
          name ? name : "<null>", mangled ? mangled : "<null>");

  // Create the function name
  // For OCaml, the linkage name is already human-readable (e.g., "Module.function")
  // and the regular name is the assembly-level symbol (e.g., "camlModule__function_1_2_code")
  Mangled func_name;
  if (mangled && strlen(mangled) > 0) {
    // OCaml's linkage names are already human-readable, so just use SetValue
    // which will treat it as demangled since it doesn't look mangled
    func_name.SetValue(ConstString(mangled));
    fprintf(stderr, "OxCaml: Using linkage name '%s' (symbol: '%s')\n", mangled, name ? name : "<none>");
  } else if (name && strlen(name) > 0) {
    // Only have the symbol name, use it
    func_name.SetValue(ConstString(name));
    fprintf(stderr, "OxCaml: Using symbol name only: %s\n", name);
  } else {
    fprintf(stderr, "OxCaml: No name found for function, returning nullptr\n");
    return nullptr; // Must have a name
  }

  // Note: Declaration info is available but not used in minimal implementation

  const user_id_t func_user_id = die.GetID();

  // Safety check - SymbolFileDWARF should never pass empty ranges
  // If it does, we can't create a valid function
  if (func_ranges.empty()) {
    fprintf(stderr, "OxCaml: Error - Function has no address ranges, cannot create function\n");
    return nullptr;
  }

  // Get the function's base address
  // Following Clang and Swift pattern - ranges should never be empty at this point
  Address func_addr = func_ranges[0].GetBaseAddress();
  fprintf(stderr, "OxCaml: Function has %zu ranges, base address: 0x%llx\n",
          func_ranges.size(), func_addr.GetFileAddress());

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

  if (func_sp) {
    // Set frame base expression if available
    if (frame_base.IsValid())
      func_sp->GetFrameBaseExpression() = frame_base;

    // Add the function to the compile unit - crucial for proper integration!
    comp_unit.AddFunction(func_sp);

    fprintf(stderr, "OxCaml: Successfully created and added Function object for %s\n",
            func_name.GetName().GetCString());
  }

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
