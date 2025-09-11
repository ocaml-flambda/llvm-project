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
#include "../../Language/OxCaml/LogChannelOxCaml.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::plugin::dwarf;

// Custom OCaml DWARF attribute DW_AT_ocaml_offset_record_from_pointer
// This attribute indicates the offset to apply when dereferencing pointers to this type
// CR sspies: Technically, this requires an extension of the DWARF standard. If
// we want to upstream this, we should think about whether the attribute should
// be on the structure or on the pointer itself (indicating a base offset).
// For now, we declare it as an ad-hoc attribute here.
static constexpr uint32_t DW_AT_ocaml_offset_record_from_pointer = 0x3106;

DWARFASTParserOxCaml::DWARFASTParserOxCaml(TypeSystemOxCaml &oxcaml_typesystem)
    : DWARFASTParser(Kind::DWARFASTParserOxCaml),
      m_oxcaml_typesystem(oxcaml_typesystem) {}

DWARFASTParserOxCaml::~DWARFASTParserOxCaml() = default;

lldb::TypeSP DWARFASTParserOxCaml::ParseTypeFromDWARF(const SymbolContext &sc,
                                                       const DWARFDIE &die,
                                                       bool *type_is_new_ptr) {
  Log *log = GetLog(OxCamlLog::TypeParsing);

  if (!die.IsValid()) {
    LLDB_LOG(log, "ParseTypeFromDWARF: Invalid DIE");
    return TypeSP();
  }

  user_id_t die_id = die.GetID();
  const char* die_name = die.GetName();

  LLDB_LOG(log, "ParseTypeFromDWARF: DIE 0x{0:x16} tag={1} name=\"{2}\"",
           die_id, die.Tag(), die_name ? die_name : "<anonymous>");

  // Recursive Type Handling Strategy:
  // 1. Check if we already have a Reference for this DIE
  // 2. If not, create a placeholder and register it immediately
  // 3. Parse the type (recursive calls will find the placeholder)
  // 4. Replace the placeholder with the actual type atomically
  // This ensures all References point to the same object and see updates

  // Check if Reference already exists
  auto existing_opt = m_oxcaml_typesystem.GetType(die_id);
  if (existing_opt.has_value()) {
    Reference<OxCamlType>* existing_ref = existing_opt.value();

    // Return existing type (whether placeholder or actual)
    LLDB_LOG(log, "ParseTypeFromDWARF: Found existing reference for DIE 0x{0:x16}", die_id);
    return CreateLLDBType(die, existing_ref);
  }

  // First time seeing this type - create placeholder with DWARF info
  LLDB_LOG(log, "ParseTypeFromDWARF: Registering new type for DIE 0x{0:x16}", die_id);

  // Extract DWARF information for placeholder
  std::optional<std::string> dwarf_name = ExtractTypeName(die);
  uint64_t dwarf_size = die.GetAttributeValueAsUnsigned(llvm::dwarf::DW_AT_byte_size, 8);

  // Create placeholder with actual DWARF info
  auto placeholder = std::make_unique<OxCamlPlaceholderType>(die_id, dwarf_name, dwarf_size);

  // Register with the placeholder
  Reference<OxCamlType>* type_ref = m_oxcaml_typesystem.RegisterType(die_id, std::move(placeholder));

  // Parse the type (recursive calls will get same empty reference)
  std::unique_ptr<OxCamlType> new_type;

  switch (die.Tag()) {
    case llvm::dwarf::DW_TAG_base_type:
      LLDB_LOG(log, "ParseTypeFromDWARF: Parsing base type");
      new_type = ParseBaseType(die);
      break;
    case llvm::dwarf::DW_TAG_typedef:
      LLDB_LOG(log, "ParseTypeFromDWARF: Parsing typedef type");
      new_type = ParseTypedefType(sc, die);
      break;
    case llvm::dwarf::DW_TAG_enumeration_type:
      LLDB_LOG(log, "ParseTypeFromDWARF: Parsing enum type");
      new_type = ParseEnumType(die);
      break;
    case llvm::dwarf::DW_TAG_pointer_type:
      LLDB_LOG(log, "ParseTypeFromDWARF: Parsing pointer type");
      new_type = ParsePointerType(sc, die);
      break;
    case llvm::dwarf::DW_TAG_reference_type:
      LLDB_LOG(log, "ParseTypeFromDWARF: Parsing reference type");
      new_type = ParseReferenceType(sc, die);
      break;
    case llvm::dwarf::DW_TAG_structure_type:
      LLDB_LOG(log, "ParseTypeFromDWARF: Parsing structure type");
      new_type = ParseStructureType(sc, die);
      break;
    default:
      LLDB_LOG(log, "ParseTypeFromDWARF: Unsupported DWARF tag: 0x{0:x} - creating unknown type", die.Tag());
      new_type = ParseUnknownType(die);
      break;
  }

  if (!new_type) {
    LLDB_LOG(log, "ParseTypeFromDWARF: Failed to create type for DIE 0x{0:x16} - creating unknown type as fallback", die_id);
    // Create unknown type as fallback for failed parsing
    new_type = ParseUnknownType(die);
  }

  // Replace placeholder with actual type - this updates ALL CompilerTypes that point to it!
  LLDB_LOG(log, "ParseTypeFromDWARF: Replacing placeholder with actual type for DIE 0x{0:x16}: {1}",
           die_id, new_type->GetDisplayName());
  type_ref->set(std::move(new_type));

  if (type_is_new_ptr)
    *type_is_new_ptr = true;

  return CreateLLDBType(die, type_ref);
}

// Helper methods for parsing different DWARF DIE types

// Helper to extract optional name from DIE
std::optional<std::string> DWARFASTParserOxCaml::ExtractTypeName(const DWARFDIE &die) {
  if (const char* name = die.GetName())
    return std::string(name);
  return std::nullopt;
}

std::unique_ptr<lldb_private::OxCamlType> DWARFASTParserOxCaml::ParseBaseType(const DWARFDIE &die) {
  return std::make_unique<OxCamlBaseType>(die.GetID(), ExtractTypeName(die));
}

std::unique_ptr<lldb_private::OxCamlType> DWARFASTParserOxCaml::ParseTypedefType(const SymbolContext &sc, const DWARFDIE &die) {
  // Get underlying type DIE
  DWARFDIE underlying_die = die.GetReferencedDIE(llvm::dwarf::DW_AT_type);
  if (!underlying_die)
    return nullptr;

  // Ensure the underlying type has a Reference
  ParseTypeFromDWARF(sc, underlying_die, nullptr);

  // Get the Reference
  auto underlying_opt = m_oxcaml_typesystem.GetType(underlying_die.GetID());
  if (!underlying_opt.has_value()) {
    llvm::report_fatal_error("ParseTypedefType: Failed to get reference for underlying type");
  }
  Reference<OxCamlType>* underlying_ref = underlying_opt.value();

  return std::make_unique<OxCamlTypedefType>(die.GetID(), ExtractTypeName(die), underlying_ref);
}

std::unique_ptr<lldb_private::OxCamlType> DWARFASTParserOxCaml::ParseEnumType(const DWARFDIE &die) {
  // Get byte size
  uint64_t byte_size = die.GetAttributeValueAsUnsigned(llvm::dwarf::DW_AT_byte_size, 8);  // Default to 8 for OCaml

  // Parse enumerators
  std::vector<OxCamlEnumType::Enumerator> enumerators;
  for (DWARFDIE child : die.children()) {
    if (child.Tag() == llvm::dwarf::DW_TAG_enumerator) {
      const char* enum_name = child.GetName();
      if (!enum_name)
        continue;

      // Get the enumerator value - OCaml uses unsigned values (odd numbers)
      int64_t enum_value = child.GetAttributeValueAsUnsigned(llvm::dwarf::DW_AT_const_value, 0);

      enumerators.push_back({enum_name, enum_value});
    }
  }

  return std::make_unique<OxCamlEnumType>(die.GetID(), ExtractTypeName(die), byte_size, std::move(enumerators));
}

lldb::TypeSP DWARFASTParserOxCaml::CreateLLDBType(const DWARFDIE &die, Reference<OxCamlType>* type_ref) {
  // Create CompilerType wrapping the Reference pointer
  CompilerType compiler_type(m_oxcaml_typesystem.weak_from_this(), type_ref);
  auto* oxcaml_type = type_ref->get();

  // Determine encoding type and other properties
  Type::EncodingDataType encoding_type = Type::eEncodingIsUID;
  user_id_t encoding_uid = LLDB_INVALID_UID;

  if (oxcaml_type->GetKind() == OxCamlType::Typedef) {
    encoding_type = Type::eEncodingIsTypedefUID;
    auto* typedef_type = static_cast<OxCamlTypedefType*>(oxcaml_type);
    if (typedef_type->GetUnderlyingType()) {
      encoding_uid = typedef_type->GetUnderlyingType()->GetDieId();
    }
  }

  // Create LLDB Type object
  SymbolFileDWARF *dwarf = die.GetDWARF();
  if (!dwarf)
    return TypeSP();

  // CR sspies: In the future, we need to update the naming scheme for types so that:
  // 1. Type names passed to LLDB are unique (e.g., include DIE ID or module info)
  // 2. The debugger uses display names when showing information to users
  // 3. This separation allows proper type identity while maintaining readable output

  std::string display_name = oxcaml_type->GetDisplayName();
  uint64_t byte_size = oxcaml_type->GetByteSize();
  Type::ResolveState resolve_state = Type::ResolveState::Full;

  TypeSP type_sp = dwarf->MakeType(
    die.GetID(),
    ConstString(display_name),
    byte_size,
    nullptr,  // context
    encoding_uid,
    encoding_type,
    nullptr,  // declaration
    compiler_type,
    resolve_state
  );

  return type_sp;
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
  Log *log = GetLog(OxCamlLog::Functions);

  if (die.Tag() != llvm::dwarf::DW_TAG_subprogram) {
    LLDB_LOG(log, "ParseFunctionFromDWARF: Not a subprogram DIE, tag={0}", die.Tag());
    return nullptr;
  }

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
    LLDB_LOG(log, "ParseFunctionFromDWARF: Failed to extract DIE names and ranges");
    return nullptr;
  }

  LLDB_LOG(log, "ParseFunctionFromDWARF: DIE 0x{0:x16} name=\"{1}\" mangled=\"{2}\"",
           die.GetID(), name ? name : "<null>", mangled ? mangled : "<null>");


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
    LLDB_LOG(log, "ParseFunctionFromDWARF: Empty address ranges, cannot create function");
    return nullptr;
  }

  LLDB_LOG(log, "ParseFunctionFromDWARF: Creating function with {0} address ranges",
           func_ranges.size());

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

    LLDB_LOG(log, "ParseFunctionFromDWARF: Successfully created and added function \"{0}\" to compile unit",
             func_sp->GetName().GetCString());
  } else {
    LLDB_LOG(log, "ParseFunctionFromDWARF: Failed to create Function object");
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

std::unique_ptr<OxCamlType> DWARFASTParserOxCaml::ParsePointerType(const SymbolContext &sc, const DWARFDIE &die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);
  user_id_t die_id = die.GetID();

  LLDB_LOG(log, "ParsePointerType: DIE 0x{0:x16}", die_id);

  // Get the pointed-to type via DW_AT_type
  DWARFDIE pointed_to_die = die.GetAttributeValueAsReferenceDIE(llvm::dwarf::DW_AT_type);
  if (!pointed_to_die.IsValid()) {
    LLDB_LOG(log, "ParsePointerType: No valid pointed-to type found");
    return nullptr;
  }

  // Extract optional name
  std::optional<std::string> name = ExtractTypeName(die);

  // Parse the pointed-to type (recursively)
  ParseTypeFromDWARF(sc, pointed_to_die, nullptr);

  // Get the Reference
  auto pointed_to_opt = m_oxcaml_typesystem.GetType(pointed_to_die.GetID());
  if (!pointed_to_opt.has_value()) {
    llvm::report_fatal_error("ParsePointerType: Failed to get reference for pointed-to type");
  }
  Reference<OxCamlType>* pointed_to_ref = pointed_to_opt.value();

  // CR sspies: Instead of going through the type system, consider using the
  // type returned from ParseTypeFromDWARF above. Old version:
  //
  // bool type_is_new = false;
  // TypeSP pointed_to_type_sp = ParseTypeFromDWARF(sc, pointed_to_die, &type_is_new);
  // if (!pointed_to_type_sp) {
  //   LLDB_LOG(log, "ParsePointerType: Failed to parse pointed-to type");
  //   return nullptr;
  // }
  //
  // // Get the OxCamlType from the pointed-to TypeSP
  // CompilerType pointed_to_compiler_type = pointed_to_type_sp->GetForwardCompilerType();
  // auto* pointed_to_oxcaml_type = static_cast<OxCamlType*>(pointed_to_compiler_type.GetOpaqueQualType());
  // if (!pointed_to_oxcaml_type) {
  //   LLDB_LOG(log, "ParsePointerType: Failed to get OxCamlType from pointed-to type");
  //   return nullptr;
  // }
  //

  LLDB_LOG(log, "ParsePointerType: Creating OxCamlPointerType pointing to {0}",
           pointed_to_ref->get()->GetDisplayName());

  return std::make_unique<OxCamlPointerType>(die_id, std::move(name), pointed_to_ref);
}

std::unique_ptr<OxCamlType> DWARFASTParserOxCaml::ParseReferenceType(const SymbolContext &sc, const DWARFDIE &die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);
  user_id_t die_id = die.GetID();

  LLDB_LOG(log, "ParseReferenceType: DIE 0x{0:x16} (treating as pointer)", die_id);

  // In OxCaml, references are essentially pointers, so we treat them the same way
  return ParsePointerType(sc, die);
}

std::unique_ptr<OxCamlType> DWARFASTParserOxCaml::ParseStructureType(const SymbolContext &sc, const DWARFDIE &die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);
  user_id_t die_id = die.GetID();

  LLDB_LOG(log, "ParseStructureType: DIE 0x{0:x16}", die_id);

  // Get structure size from DW_AT_byte_size
  uint64_t byte_size = die.GetAttributeValueAsUnsigned(llvm::dwarf::DW_AT_byte_size, 0);
  if (byte_size == 0) {
    LLDB_LOG(log, "ParseStructureType: No byte size found or zero size");
    return nullptr;
  }

  // Extract optional name
  std::optional<std::string> name = ExtractTypeName(die);

  // Check for custom DW_AT_ocaml_offset_record_from_pointer attribute
  auto ocaml_attr = static_cast<llvm::dwarf::Attribute>(DW_AT_ocaml_offset_record_from_pointer);
  uint64_t attr_value = die.GetAttributeValueAsUnsigned(ocaml_attr, 0);
  int64_t base_offset = static_cast<int64_t>(attr_value);

  if (base_offset != 0) {
    LLDB_LOG(log, "ParseStructureType: Found DW_AT_ocaml_offset_record_from_pointer: {0}", base_offset);
  }

  // Parse child DIEs - separate members and variant parts
  std::vector<OxCamlMember> members;
  std::vector<OxCamlVariantPart> variant_parts;

  DWARFDIE child_die = die.GetFirstChild();
  while (child_die.IsValid()) {
    if (child_die.Tag() == llvm::dwarf::DW_TAG_member) {
      // Parse regular member
      auto member = ParseMember(sc, child_die);
      if (member.has_value()) {
        members.push_back(std::move(*member));
        LLDB_LOG(log, "ParseStructureType: Added member {0} at offset {1}, type {2}",
                 member->name.value_or("<unnamed>"), member->data_member_location, member->GetType()->GetDisplayName());
      } else {
        LLDB_LOG(log, "ParseStructureType: Failed to parse member, skipping");
      }
    } else if (child_die.Tag() == llvm::dwarf::DW_TAG_variant_part) {
      // Parse variant part
      auto variant_part = ParseVariantPart(sc, child_die);
      if (variant_part.has_value()) {
        variant_parts.push_back(std::move(*variant_part));
        LLDB_LOG(log, "ParseStructureType: Added variant part with {0} variants",
                 variant_part->GetVariants().size());
      } else {
        LLDB_LOG(log, "ParseStructureType: Failed to parse variant part, skipping");
      }
    }
    child_die = child_die.GetSibling();
  }

  LLDB_LOG(log, "ParseStructureType: Creating OxCamlStructureType with {0} members, {1} variant parts, size {2}",
           members.size(), variant_parts.size(), byte_size);

  return std::make_unique<OxCamlStructureType>(die_id, std::move(name), byte_size, std::move(members), std::move(variant_parts), base_offset);
}

std::optional<OxCamlMember> DWARFASTParserOxCaml::ParseMember(const SymbolContext &sc, const DWARFDIE &member_die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);

  LLDB_LOG(log, "ParseMember: DIE 0x{0:x16}", member_die.GetID());

  // Get member name (optional for tuples)
  std::optional<std::string> member_name = ExtractTypeName(member_die);

  // Get member type
  DWARFDIE member_type_die = member_die.GetAttributeValueAsReferenceDIE(llvm::dwarf::DW_AT_type);
  if (!member_type_die.IsValid()) {
    LLDB_LOG(log, "ParseMember: Member has no valid type");
    return std::nullopt;
  }

  // Parse member type
  bool type_is_new = false;
  TypeSP member_type_sp = ParseTypeFromDWARF(sc, member_type_die, &type_is_new);
  if (!member_type_sp) {
    LLDB_LOG(log, "ParseMember: Failed to parse member type");
    return std::nullopt;
  }

  // Get the Reference
  auto member_type_opt = m_oxcaml_typesystem.GetType(member_type_die.GetID());
  if (!member_type_opt.has_value()) {
    llvm::report_fatal_error("ParseMember: Failed to get reference for member type");
  }
  Reference<OxCamlType>* member_type_ref = member_type_opt.value();

  // Get member offset
  uint64_t member_offset = member_die.GetAttributeValueAsUnsigned(llvm::dwarf::DW_AT_data_member_location, 0);

  // Get bit field attributes (if present)
  std::optional<uint64_t> bit_offset;
  std::optional<uint64_t> bit_size;

  if (member_die.GetAttributeValueAsUnsigned(llvm::dwarf::DW_AT_data_bit_offset, UINT64_MAX) != UINT64_MAX) {
    bit_offset = member_die.GetAttributeValueAsUnsigned(llvm::dwarf::DW_AT_data_bit_offset, 0);
  }

  if (member_die.GetAttributeValueAsUnsigned(llvm::dwarf::DW_AT_bit_size, UINT64_MAX) != UINT64_MAX) {
    bit_size = member_die.GetAttributeValueAsUnsigned(llvm::dwarf::DW_AT_bit_size, 0);
  }

  // Get artificial attribute
  bool is_artificial = member_die.GetAttributeValueAsUnsigned(llvm::dwarf::DW_AT_artificial, 0) != 0;

  LLDB_LOG(log, "ParseMember: Created member {0} at offset {1}, type {2}{3}",
           member_name.value_or("<unnamed>"), member_offset, member_type_ref->get()->GetDisplayName(),
           is_artificial ? " (artificial)" : "");

  if (bit_offset.has_value() || bit_size.has_value()) {
    LLDB_LOG(log, "ParseMember: Bit field detected - offset: {0}, size: {1}",
             bit_offset.value_or(0), bit_size.value_or(0));
  }

  return OxCamlMember{
    std::move(member_name),
    member_type_ref,
    member_offset,
    bit_offset,
    bit_size,
    is_artificial
  };
}

std::optional<OxCamlVariantPart> DWARFASTParserOxCaml::ParseVariantPart(const SymbolContext &sc, const DWARFDIE &variant_part_die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);
  user_id_t die_id = variant_part_die.GetID();

  LLDB_LOG(log, "ParseVariantPart: DIE 0x{0:x16}", die_id);

  // Get discriminator reference from DW_AT_discr
  DWARFDIE discriminator_die = variant_part_die.GetAttributeValueAsReferenceDIE(llvm::dwarf::DW_AT_discr);
  if (!discriminator_die.IsValid()) {
    LLDB_LOG(log, "ParseVariantPart: No valid discriminator reference");
    return std::nullopt;
  }

  LLDB_LOG(log, "ParseVariantPart: Found discriminator DIE 0x{0:x16}", discriminator_die.GetID());

  // Parse discriminator member
  auto discriminator_member = ParseMember(sc, discriminator_die);
  if (!discriminator_member.has_value()) {
    LLDB_LOG(log, "ParseVariantPart: Failed to parse discriminator member");
    return std::nullopt;
  }

  LLDB_LOG(log, "ParseVariantPart: Discriminator at offset {0}, type: {1}",
           discriminator_member->data_member_location,
           discriminator_member->GetType()->GetDisplayName());

  // Parse variant DIE children
  std::vector<OxCamlVariantPart::Variant> variants;
  DWARFDIE child_die = variant_part_die.GetFirstChild();

  while (child_die.IsValid()) {
    if (child_die.Tag() == llvm::dwarf::DW_TAG_variant) {
      LLDB_LOG(log, "ParseVariantPart: Parsing variant DIE 0x{0:x16}", child_die.GetID());

      // Get discriminator value from DW_AT_discr_value
      uint64_t discr_value = child_die.GetAttributeValueAsUnsigned(llvm::dwarf::DW_AT_discr_value, UINT64_MAX);
      if (discr_value == UINT64_MAX) {
        LLDB_LOG(log, "ParseVariantPart: Variant has no discriminator value, skipping");
        child_die = child_die.GetSibling();
        continue;
      }

      LLDB_LOG(log, "ParseVariantPart: Variant discriminator value: {0}", discr_value);

      // Parse variant members
      std::vector<OxCamlMember> variant_members;
      DWARFDIE variant_child_die = child_die.GetFirstChild();

      while (variant_child_die.IsValid()) {
        if (variant_child_die.Tag() == llvm::dwarf::DW_TAG_member) {
          auto member = ParseMember(sc, variant_child_die);
          if (member.has_value()) {
            variant_members.push_back(std::move(*member));
            LLDB_LOG(log, "ParseVariantPart: Added variant member {0}",
                     member->name.value_or("<unnamed>"));
          }
        }
        variant_child_die = variant_child_die.GetSibling();
      }

      // Add variant to list
      variants.push_back({discr_value, std::move(variant_members)});
      LLDB_LOG(log, "ParseVariantPart: Added variant with discriminator {0}, {1} members",
               discr_value, variants.back().members.size());
    }
    child_die = child_die.GetSibling();
  }

  LLDB_LOG(log, "ParseVariantPart: Created variant part with {0} variants", variants.size());

  return OxCamlVariantPart{std::move(*discriminator_member), std::move(variants)};
}

// Parse unknown/unsupported DWARF types
std::unique_ptr<OxCamlType> DWARFASTParserOxCaml::ParseUnknownType(const DWARFDIE &die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);
  lldb::user_id_t die_id = die.GetID();
  uint32_t dwarf_tag = die.Tag();

  LLDB_LOG(log, "ParseUnknownType: Creating unknown type for DIE 0x{0:x16}, DWARF tag 0x{1:x}",
           die_id, dwarf_tag);

  // Extract basic information if available
  std::optional<std::string> name = ExtractTypeName(die);
  uint64_t byte_size = die.GetAttributeValueAsUnsigned(llvm::dwarf::DW_AT_byte_size, 8);  // Default to 8 bytes

  LLDB_LOG(log, "ParseUnknownType: name=\"{0}\", byte_size={1}",
           name ? *name : "<anonymous>", byte_size);

  return std::make_unique<OxCamlUnknownType>(die_id, std::move(name), byte_size, dwarf_tag);
}
