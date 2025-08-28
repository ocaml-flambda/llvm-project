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
  // Handle DW_TAG_base_type for basic OCaml types
  if (die.Tag() == llvm::dwarf::DW_TAG_base_type) {
    const char *name = die.GetName();
    if (!name)
      name = "ocaml_value";

    // Get the SymbolFileDWARF
    SymbolFileDWARF *dwarf = die.GetDWARF();
    if (!dwarf)
      return TypeSP();

    // Get the type pointer from TypeSystem and use it
    void* type_ptr = m_oxcaml_typesystem.GetOxCamlValueType();
    CompilerType compiler_type = m_oxcaml_typesystem.GetTypeForFormatters(type_ptr);

    // Create the Type object using MakeType
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

  // Handle DW_TAG_typedef for OCaml type aliases (e.g., "int @ value")
  if (die.Tag() == llvm::dwarf::DW_TAG_typedef) {
    const char *typedef_name = die.GetName();

    // For anonymous typedefs (intermediate in typedef chain),
    // skip to the underlying type
    if (!typedef_name) {
      DWARFDIE type_die = die.GetReferencedDIE(llvm::dwarf::DW_AT_type);
      if (!type_die) {
        return TypeSP();
      }
      // Recursively resolve the underlying type
      return ParseTypeFromDWARF(sc, type_die, type_is_new_ptr);
    }

    // Get the underlying type that this typedef refers to
    DWARFDIE type_die = die.GetReferencedDIE(llvm::dwarf::DW_AT_type);
    if (!type_die) {
      return TypeSP();
    }

    // Get the SymbolFileDWARF
    SymbolFileDWARF *dwarf = die.GetDWARF();
    if (!dwarf)
      return TypeSP();

    // Resolve the underlying type
    Type *underlying_type = dwarf->ResolveTypeUID(type_die, true);
    if (!underlying_type) {
      return TypeSP();
    }

    // Create a typedef Type that references the underlying type
    // For OCaml, we want to preserve the typedef name (e.g., "int @ value")
    // but still reference the base ocaml_value type
    auto byte_size_or_error = underlying_type->GetByteSize(nullptr);
    uint64_t byte_size = byte_size_or_error ? *byte_size_or_error : 8;

    TypeSP type_sp = dwarf->MakeType(
        die.GetID(),
        ConstString(typedef_name),
        byte_size,
        nullptr,  // context
        type_die.GetID(),  // encoding_uid - reference to the underlying type
        Type::eEncodingIsTypedefUID,
        nullptr,  // declaration
        underlying_type->GetForwardCompilerType(),  // Use the underlying type's CompilerType
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
  // CR sspies: This function would be the right place to compute OCaml linkage
  // names from mangled symbol names if they weren't provided via DW_AT_linkage_name.
  //
  // Currently, OxCaml emits both:
  // - DW_AT_name: mangled symbol (e.g., "camlModule__function_1_2_code")
  // - DW_AT_linkage_name: OCaml name (e.g., "Module.function")
  //
  // If OxCaml stopped emitting DW_AT_linkage_name, we would implement the
  // demangling logic here.
  //
  // ParseFunctionFromDWARF would need modification.
  // Instead of just using the symbol name when linkage_name is missing:
  //   if (mangled && strlen(mangled) > 0) {
  //     func_name.SetValue(ConstString(mangled));
  //   } else if (name && strlen(name) > 0) {
  //     ConstString demangled = ConstructDemangledNameFromDWARF(die);
  //     if (demangled) {
  //       func_name.SetValue(demangled);
  //     } else {
  //       func_name.SetValue(ConstString(name));
  //     }
  //   }

  return ConstString();  // Currently returns empty as linkage names are provided
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
  // For OCaml, the linkage name is already human-readable (e.g., "Module.function")
  // and the regular name is the assembly-level symbol (e.g., "camlModule__function_1_2_code")
  Mangled func_name;
  if (mangled && strlen(mangled) > 0) {
    // OCaml's linkage names are already human-readable, so just use SetValue
    // which will treat it as demangled since it doesn't look mangled
    func_name.SetValue(ConstString(mangled));
  } else if (name && strlen(name) > 0) {
    // Only have the symbol name, use it
    // CR sspies: Change here for reconstructing linkage names from mangled names.
    func_name.SetValue(ConstString(name));
  } else {
    return nullptr; // Must have a name
  }

  // Note: Declaration info is available but not used in minimal implementation
  // CR sspies: Declaration info could be set here for faster source lookups:
  //   Declaration decl(comp_unit.GetSupportFiles().GetFileSpecAtIndex(*decl_file),
  //                    *decl_line, decl_column ? *decl_column : 0);
  // Currently, LLDB falls back to LineTable lookups for source locations.
  // See Function::GetStartLineSourceInfo() which calls CompileUnit::GetLineTable()
  // to find line entries by address. The LineTable is built from DWARF .debug_line
  // section and provides address-to-source mappings.

  const user_id_t func_user_id = die.GetID();

  // Safety check - SymbolFileDWARF should never pass empty ranges
  // If it does, we can't create a valid function
  if (func_ranges.empty()) {
    return nullptr;
  }

  // Get the function's base address
  Address func_addr = func_ranges[0].GetBaseAddress();

  // Create the Function object
  FunctionSP func_sp = std::make_shared<Function>(
      &comp_unit,           // CompileUnit
      func_user_id,         // UserID for function DIE
      func_user_id,         // UserID for function Type (same for now)
      func_name,            // Mangled function name
      nullptr,              // FunctionType (we'll add this later)
                            // CR sspies: FunctionType could parse DW_TAG_formal_parameter children to build
                            // parameter info, but LLDB already handles parameter display separately via
                            // direct DWARF DIE inspection when stopped in the function.
      std::move(func_addr), // Base address
      std::move(func_ranges) // All address ranges
  );

  if (func_sp) {
    // Set frame base expression if available
    if (frame_base.IsValid())
      func_sp->GetFrameBaseExpression() = frame_base;

    // Add the function to the compile unit - crucial for proper integration!
    comp_unit.AddFunction(func_sp);
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
