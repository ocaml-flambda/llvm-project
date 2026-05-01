//===-- DWARFASTParserOxCaml.cpp --------------------------------*- C++ -*-===//
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

#include "../../Language/OxCaml/LogChannelOxCaml.h"
#include "../../Language/OxCaml/OxCamlAssert.h"
#include "Plugins/TypeSystem/OxCaml/TypeSystemOxCaml.h"
#include "lldb/Core/Module.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Target/Language.h"
#include "lldb/Utility/Log.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::plugin::dwarf;

// Custom OCaml DWARF attribute DW_AT_ocaml_offset_record_from_pointer
// This attribute indicates the offset to apply when dereferencing pointers to
// this type
static constexpr uint32_t DW_AT_ocaml_offset_record_from_pointer = 0x3106;
// CR sspies: Technically, this requires an extension of the DWARF
// standard. If we want to upstream this, we should think about whether the
// attribute should be on the structure or on the pointer itself (indicating a
// base offset). For now, we declare it as an ad-hoc attribute here.

// Default size for OCaml values when size attribute is missing
static constexpr uint64_t DEFAULT_OCAML_VALUE_SIZE = 8;

DWARFASTParserOxCaml::DWARFASTParserOxCaml(TypeSystemOxCaml &oxcaml_typesystem)
    : DWARFASTParser(Kind::DWARFASTParserOxCaml),
      m_oxcaml_typesystem(oxcaml_typesystem) {}

DWARFASTParserOxCaml::~DWARFASTParserOxCaml() = default;

lldb::TypeSP DWARFASTParserOxCaml::ParseTypeFromDWARF(const SymbolContext &sc,
                                                      const DWARFDIE &die,
                                                      bool *type_is_new_ptr) {
  Log *log = GetLog(OxCamlLog::TypeParsing);

  if (type_is_new_ptr)
    *type_is_new_ptr = false;

  if (!die.IsValid()) {
    LLDB_LOG(log, "ParseTypeFromDWARF: Invalid DIE");
    return TypeSP();
  }

  user_id_t die_id = die.GetID();
  const char *die_name = die.GetName();

  LLDB_LOG(log, "ParseTypeFromDWARF: DIE 0x{0:x16} tag={1} name=\"{2}\"",
           die_id, die.Tag(), die_name ? die_name : "<anonymous>");

  // Recursive Type Handling Strategy:
  // 1. Check if we already have a Reference for this DIE
  // 2. If not, create a placeholder and register it immediately
  // 3. Parse the type (recursive calls will find the placeholder)
  // 4. Replace the placeholder with the actual type
  // This ensures all References point to the same object and see updates

  // Check if Reference already exists
  auto existing_opt = m_oxcaml_typesystem.GetType(die_id);
  if (existing_opt.has_value()) {
    Reference<OxCamlType> *existing_ref = existing_opt.value();

    LLDB_LOG(log,
             "ParseTypeFromDWARF: Found existing reference for DIE 0x{0:x16}",
             die_id);
    return CreateLLDBType(die, existing_ref);
  }

  LLDB_LOG(log, "ParseTypeFromDWARF: Registering new type for DIE 0x{0:x16}",
           die_id);

  std::optional<std::string> dwarf_name = ExtractTypeName(die);
  auto dwarf_size_opt =
      die.GetAttributeValueAsOptionalUnsigned(llvm::dwarf::DW_AT_byte_size);

  auto placeholder = std::make_unique<OxCamlPlaceholderType>(
      die_id, dwarf_name, dwarf_size_opt.value_or(DEFAULT_OCAML_VALUE_SIZE));

  Reference<OxCamlType> *type_ref =
      m_oxcaml_typesystem.RegisterType(die_id, std::move(placeholder));

  std::unique_ptr<OxCamlType> new_type;

  switch (die.Tag()) {
  case llvm::dwarf::DW_TAG_base_type:
    new_type = ParseBaseType(die);
    break;
  case llvm::dwarf::DW_TAG_typedef:
    new_type = ParseTypedefType(sc, die);
    break;
  case llvm::dwarf::DW_TAG_enumeration_type:
    new_type = ParseEnumType(die);
    break;
  case llvm::dwarf::DW_TAG_pointer_type:
    new_type = ParsePointerType(sc, die);
    break;
  case llvm::dwarf::DW_TAG_reference_type:
    new_type = ParseReferenceType(sc, die);
    break;
  case llvm::dwarf::DW_TAG_structure_type:
    new_type = ParseStructureType(sc, die);
    break;
  case llvm::dwarf::DW_TAG_array_type:
    new_type = ParseArrayType(sc, die);
    break;
  default:
    LLDB_LOG(log, "ParseTypeFromDWARF: Unsupported DWARF tag: 0x{0:x}",
             die.Tag());
    new_type = ParseUnknownType(die);
    break;
  }

  if (!new_type) {
    LLDB_LOG(log,
             "ParseTypeFromDWARF: Failed to create type for DIE 0x{0:x16} - "
             "creating unknown type as fallback",
             die_id);
    // Create unknown type as fallback for failed parsing
    new_type = ParseUnknownType(die);
  }

  // Replace placeholder with actual type
  LLDB_LOG(log,
           "ParseTypeFromDWARF: Replacing placeholder with actual type for DIE "
           "0x{0:x16}: {1}",
           die_id, new_type->GetDisplayName());
  type_ref->set(std::move(new_type));

  if (type_is_new_ptr)
    *type_is_new_ptr = true;

  return CreateLLDBType(die, type_ref);
}

std::optional<std::string>
DWARFASTParserOxCaml::ExtractTypeName(const DWARFDIE &die) {
  if (const char *name = die.GetName())
    return std::string(name);
  return std::nullopt;
}

std::unique_ptr<lldb_private::OxCamlType>
DWARFASTParserOxCaml::ParseBaseType(const DWARFDIE &die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);
  auto name_opt = ExtractTypeName(die);

  // Fallback for generic values, which carry the literal name "ocaml_value"
  if (name_opt && name_opt.value() == "ocaml_value") {
    return std::make_unique<OxCamlValueType>(die.GetID(), std::move(name_opt));
  }

  std::optional<uint64_t> byte_size =
      die.GetAttributeValueAsOptionalUnsigned(llvm::dwarf::DW_AT_byte_size);
  if (!byte_size.has_value() || byte_size.value() == 0) {
    LLDB_LOG(log,
             "ParseBaseType: Missing or zero byte_size for DIE 0x{0:x16} - "
             "cannot parse",
             die.GetID());
    return nullptr;
  }

  std::optional<uint64_t> encoding =
      die.GetAttributeValueAsOptionalUnsigned(llvm::dwarf::DW_AT_encoding);
  if (!encoding.has_value()) {
    LLDB_LOG(log,
             "ParseBaseType: Missing encoding for DIE 0x{0:x16} - cannot parse",
             die.GetID());
    return nullptr;
  }

  OxCamlUnboxedBaseType::BaseKind base_kind;
  switch (encoding.value()) {
  case llvm::dwarf::DW_ATE_signed:
    base_kind = OxCamlUnboxedBaseType::Signed;
    break;
  case llvm::dwarf::DW_ATE_unsigned:
    base_kind = OxCamlUnboxedBaseType::Unsigned;
    break;
  case llvm::dwarf::DW_ATE_float:
    base_kind = OxCamlUnboxedBaseType::Float;
    break;
  default:
    return std::make_unique<OxCamlUnknownType>(die.GetID(), std::move(name_opt),
                                               byte_size.value(), die.Tag());
  }

  return std::make_unique<OxCamlUnboxedBaseType>(
      die.GetID(), std::move(name_opt), byte_size.value(), base_kind);
}

std::unique_ptr<lldb_private::OxCamlType>
DWARFASTParserOxCaml::ParseTypedefType(const SymbolContext &sc,
                                       const DWARFDIE &die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);

  DWARFDIE underlying_die = die.GetReferencedDIE(llvm::dwarf::DW_AT_type);
  if (!underlying_die) {
    LLDB_LOG(log,
             "ParseTypedefType: Missing or invalid DW_AT_type reference for "
             "DIE 0x{0:x16}",
             die.GetID());
    return nullptr;
  }

  ParseTypeFromDWARF(sc, underlying_die, nullptr);

  auto underlying_opt = m_oxcaml_typesystem.GetType(underlying_die.GetID());
  OX_ASSERT(underlying_opt.has_value(),
            "Failed to get reference for underlying type DIE 0x{0:x}",
            underlying_die.GetID());
  Reference<OxCamlType> *underlying_ref = underlying_opt.value();

  return std::make_unique<OxCamlTypedefType>(die.GetID(), ExtractTypeName(die),
                                             underlying_ref);
}

std::unique_ptr<lldb_private::OxCamlType>
DWARFASTParserOxCaml::ParseEnumType(const DWARFDIE &die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);

  std::optional<uint64_t> byte_size_opt =
      die.GetAttributeValueAsOptionalUnsigned(llvm::dwarf::DW_AT_byte_size);
  if (!byte_size_opt.has_value() || byte_size_opt.value() == 0) {
    LLDB_LOG(log,
             "ParseEnumType: Missing or zero byte_size for enum DIE 0x{0:x16} "
             "- cannot parse",
             die.GetID());
    return nullptr;
  }
  uint64_t byte_size = byte_size_opt.value();

  std::vector<OxCamlEnumType::Enumerator> enumerators;
  for (DWARFDIE child : die.children()) {
    if (child.Tag() == llvm::dwarf::DW_TAG_enumerator) {
      const char *enum_name = child.GetName();
      if (!enum_name)
        continue;

      std::optional<uint64_t> enum_value_opt =
          child.GetAttributeValueAsOptionalUnsigned(
              llvm::dwarf::DW_AT_const_value);
      if (!enum_value_opt.has_value()) {
        LLDB_LOG(log,
                 "ParseEnumType: Missing const_value for enumerator '{0}' in "
                 "DIE 0x{1:x16}",
                 enum_name, die.GetID());
        continue;
      }

      enumerators.push_back(
          {enum_name, static_cast<int64_t>(enum_value_opt.value())});
    }
  }

  return std::make_unique<OxCamlEnumType>(die.GetID(), ExtractTypeName(die),
                                          byte_size, std::move(enumerators));
}

lldb::TypeSP
DWARFASTParserOxCaml::CreateLLDBType(const DWARFDIE &die,
                                     Reference<OxCamlType> *type_ref) {
  CompilerType compiler_type(m_oxcaml_typesystem.weak_from_this(), type_ref);
  auto *oxcaml_type = type_ref->get();

  Type::EncodingDataType encoding_type = Type::eEncodingIsUID;
  user_id_t encoding_uid = LLDB_INVALID_UID;

  if (oxcaml_type->GetKind() == OxCamlType::Typedef) {
    encoding_type = Type::eEncodingIsTypedefUID;
    auto *typedef_type = static_cast<OxCamlTypedefType *>(oxcaml_type);
    if (typedef_type->GetUnderlyingType()) {
      encoding_uid = typedef_type->GetUnderlyingType()->GetDieId();
    }
  }

  SymbolFileDWARF *dwarf = die.GetDWARF();
  if (!dwarf)
    return TypeSP();

  // CR sspies: In the future, we need to update the naming scheme for types to
  // ensure that:
  // 1. Type names passed to LLDB are unique (e.g., include DIE ID
  // 2. The debugger uses display names when showing information to users
  // 3. This separation allows proper type identity and readability

  std::string display_name = oxcaml_type->GetDisplayName();
  uint64_t byte_size = oxcaml_type->GetByteSize();
  Type::ResolveState resolve_state = Type::ResolveState::Full;

  TypeSP type_sp =
      dwarf->MakeType(die.GetID(), ConstString(display_name), byte_size,
                      nullptr, // context
                      encoding_uid, encoding_type,
                      nullptr, // declaration
                      compiler_type, resolve_state);

  return type_sp;
}

ConstString
DWARFASTParserOxCaml::ConstructDemangledNameFromDWARF(const DWARFDIE &die) {
  // CR sspies: This function would be the right place to compute OCaml linkage
  // names from mangled symbol names if they weren't provided via
  // DW_AT_linkage_name.
  //
  // Currently, OxCaml emits both:
  // - DW_AT_name: mangled symbol (e.g., "camlModule__function_1_2_code")
  // - DW_AT_linkage_name: OCaml name (e.g., "Module.function")
  //
  // If OxCaml stopped emitting DW_AT_linkage_name, we would implement the
  // demangling logic here. We would also need to update ParseFunctionFromDWARF.

  return ConstString(); // Currently returns empty as linkage names are provided
}

Function *DWARFASTParserOxCaml::ParseFunctionFromDWARF(
    CompileUnit &comp_unit, const DWARFDIE &die, AddressRanges func_ranges) {
  Log *log = GetLog(OxCamlLog::FunctionParsing);

  if (die.Tag() != llvm::dwarf::DW_TAG_subprogram) {
    LLDB_LOG(log, "ParseFunctionFromDWARF: Not a subprogram DIE, tag={0}",
             die.Tag());
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
    LLDB_LOG(log,
             "ParseFunctionFromDWARF: Failed to extract DIE names and ranges");
    return nullptr;
  }

  LLDB_LOG(log,
           "ParseFunctionFromDWARF: DIE 0x{0:x16} name=\"{1}\" mangled=\"{2}\"",
           die.GetID(), name ? name : "<null>", mangled ? mangled : "<null>");

  // For OCaml, DW_AT_name is the source-level name (e.g., "Module.function")
  // and DW_AT_linkage_name is the mangled assembly symbol (e.g.,
  // "camlModule__function_1_2_code"). Mangled::SetValue would auto-detect via
  // GetManglingScheme(), which doesn't recognize OCaml mangling, so we set
  // both fields explicitly.
  // TODO(oxcaml): once lldb has an OCaml demangler registered with
  // Mangled::GetManglingScheme(), revert this to a single SetValue(mangled)
  // call so demangling happens lazily through the standard path.
  Mangled func_name;
  if (name && strlen(name) > 0)
    func_name.SetDemangledName(ConstString(name));
  if (mangled && strlen(mangled) > 0)
    func_name.SetMangledName(ConstString(mangled));
  if (!func_name)
    return nullptr; // Must have a name

  const user_id_t func_user_id = die.GetID();

  // Safety check - SymbolFileDWARF should never pass empty ranges
  // If it does, we can't create a valid function
  if (func_ranges.empty()) {
    LLDB_LOG(
        log,
        "ParseFunctionFromDWARF: Empty address ranges, cannot create function");
    return nullptr;
  }

  LLDB_LOG(log,
           "ParseFunctionFromDWARF: Creating function with {0} address ranges",
           func_ranges.size());

  Address func_addr = func_ranges[0].GetBaseAddress();
  // CR sspies: FunctionType could parse DW_TAG_formal_parameter children to
  // build parameter info, but LLDB already handles parameter display separately
  // via direct DWARF DIE inspection when stopped in the function.
  FunctionSP func_sp = std::make_shared<Function>(
      &comp_unit,            // CompileUnit
      func_user_id,          // UserID for function DIE
      func_user_id,          // UserID for function Type (same for now)
      func_name,             // Mangled function name
      nullptr,               // FunctionType
      std::move(func_addr),  // Base address
      std::move(func_ranges) // All address ranges
  );

  if (!func_sp) {
    LLDB_LOG(log, "ParseFunctionFromDWARF: Failed to create Function object");
    return nullptr;
  }

  // Set frame base expression if available
  if (frame_base.IsValid())
    func_sp->GetFrameBaseExpression() = frame_base;
  comp_unit.AddFunction(func_sp);
  LLDB_LOG(log,
           "ParseFunctionFromDWARF: Successfully created and added function "
           "\"{0}\" to compile unit",
           func_sp->GetName().GetCString());
  return func_sp.get();
}

bool DWARFASTParserOxCaml::CompleteTypeFromDWARF(
    const DWARFDIE &die, Type *type, const CompilerType &compiler_type) {
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

CompilerDeclContext DWARFASTParserOxCaml::GetDeclContextContainingUIDFromDWARF(
    const DWARFDIE &die) {
  // Not implemented yet - return invalid context
  return CompilerDeclContext();
}

std::string DWARFASTParserOxCaml::GetDIEClassTemplateParams(DWARFDIE die) {
  // OCaml doesn't have C++-style templates
  return {};
}

std::unique_ptr<OxCamlType>
DWARFASTParserOxCaml::ParsePointerType(const SymbolContext &sc,
                                       const DWARFDIE &die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);
  user_id_t die_id = die.GetID();

  LLDB_LOG(log, "ParsePointerType: DIE 0x{0:x16}", die_id);

  DWARFDIE pointed_to_die =
      die.GetAttributeValueAsReferenceDIE(llvm::dwarf::DW_AT_type);
  if (!pointed_to_die.IsValid()) {
    LLDB_LOG(log, "ParsePointerType: No valid pointed-to type found");
    return nullptr;
  }

  std::optional<std::string> name = ExtractTypeName(die);

  ParseTypeFromDWARF(sc, pointed_to_die, nullptr);

  auto pointed_to_opt = m_oxcaml_typesystem.GetType(pointed_to_die.GetID());
  OX_ASSERT(pointed_to_opt.has_value(),
            "Failed to get reference for pointed-to type DIE 0x{0:x}",
            pointed_to_die.GetID());
  Reference<OxCamlType> *pointed_to_ref = pointed_to_opt.value();

  LLDB_LOG(log, "ParsePointerType: Creating OxCamlPointerType pointing to {0}",
           pointed_to_ref->get()->GetDisplayName());

  return std::make_unique<OxCamlPointerType>(die_id, std::move(name),
                                             pointed_to_ref);
}

std::unique_ptr<OxCamlType>
DWARFASTParserOxCaml::ParseReferenceType(const SymbolContext &sc,
                                         const DWARFDIE &die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);
  user_id_t die_id = die.GetID();

  LLDB_LOG(log, "ParseReferenceType: DIE 0x{0:x16} (treating as pointer)",
           die_id);

  // In OxCaml, references are essentially pointers, so we treat them the same
  return ParsePointerType(sc, die);
}

std::unique_ptr<OxCamlType>
DWARFASTParserOxCaml::ParseStructureType(const SymbolContext &sc,
                                         const DWARFDIE &die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);
  user_id_t die_id = die.GetID();

  LLDB_LOG(log, "ParseStructureType: DIE 0x{0:x16}", die_id);

  std::optional<uint64_t> byte_size_opt =
      die.GetAttributeValueAsOptionalUnsigned(llvm::dwarf::DW_AT_byte_size);
  if (!byte_size_opt.has_value() || byte_size_opt.value() == 0) {
    LLDB_LOG(log,
             "ParseStructureType: Missing or zero byte_size for DIE 0x{0:x16}",
             die_id);
    return nullptr;
  }
  uint64_t byte_size = byte_size_opt.value();

  std::optional<std::string> name = ExtractTypeName(die);

  auto ocaml_attr = static_cast<llvm::dwarf::Attribute>(
      DW_AT_ocaml_offset_record_from_pointer);
  std::optional<uint64_t> attr_value_opt =
      die.GetAttributeValueAsOptionalUnsigned(ocaml_attr);

  int64_t base_offset = 0;
  if (attr_value_opt.has_value()) {
    base_offset = static_cast<int64_t>(attr_value_opt.value());
    LLDB_LOG(
        log,
        "ParseStructureType: Found DW_AT_ocaml_offset_record_from_pointer: {0}",
        base_offset);
  }

  std::vector<OxCamlMember> members;
  std::vector<OxCamlVariantPart> variant_parts;

  for (DWARFDIE child_die : die.children()) {
    switch (child_die.Tag()) {
    case llvm::dwarf::DW_TAG_member: {
      auto member = ParseMember(sc, child_die);
      if (member.has_value()) {
        members.push_back(std::move(*member));
        LLDB_LOG(
            log, "ParseStructureType: Added member {0} at offset {1}, type {2}",
            member->name.value_or("<unnamed>"), member->data_member_location,
            member->GetType()->GetDisplayName());
      } else {
        LLDB_LOG(log, "ParseStructureType: Failed to parse member, skipping");
      }
      break;
    }
    case llvm::dwarf::DW_TAG_variant_part: {
      auto variant_part = ParseVariantPart(sc, child_die);
      if (variant_part.has_value()) {
        variant_parts.push_back(std::move(*variant_part));
        LLDB_LOG(log,
                 "ParseStructureType: Added variant part with {0} variants",
                 variant_part->GetVariants().size());
      } else {
        LLDB_LOG(log,
                 "ParseStructureType: Failed to parse variant part, skipping");
      }
      break;
    }
    default:
      break;
    }
  }

  LLDB_LOG(log,
           "ParseStructureType: Creating OxCamlStructureType with {0} members, "
           "{1} variant parts, size {2}",
           members.size(), variant_parts.size(), byte_size);

  return std::make_unique<OxCamlStructureType>(
      die_id, std::move(name), byte_size, std::move(members),
      std::move(variant_parts), base_offset);
}

std::optional<OxCamlMember>
DWARFASTParserOxCaml::ParseMember(const SymbolContext &sc,
                                  const DWARFDIE &member_die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);

  LLDB_LOG(log, "ParseMember: DIE 0x{0:x16}", member_die.GetID());

  std::optional<std::string> member_name = ExtractTypeName(member_die);

  DWARFDIE member_type_die =
      member_die.GetAttributeValueAsReferenceDIE(llvm::dwarf::DW_AT_type);
  if (!member_type_die.IsValid()) {
    LLDB_LOG(log, "ParseMember: Member has no valid type");
    return std::nullopt;
  }

  bool type_is_new = false;
  TypeSP member_type_sp = ParseTypeFromDWARF(sc, member_type_die, &type_is_new);
  if (!member_type_sp) {
    LLDB_LOG(log, "ParseMember: Failed to parse member type");
    return std::nullopt;
  }

  auto member_type_opt = m_oxcaml_typesystem.GetType(member_type_die.GetID());
  OX_ASSERT(member_type_opt.has_value(),
            "Failed to get reference for member type DIE 0x{0:x}",
            member_type_die.GetID());
  Reference<OxCamlType> *member_type_ref = member_type_opt.value();

  std::optional<uint64_t> member_offset_opt =
      member_die.GetAttributeValueAsOptionalUnsigned(
          llvm::dwarf::DW_AT_data_member_location);
  if (!member_offset_opt.has_value()) {
    LLDB_LOG(log, "ParseMember: Missing data_member_location for DIE 0x{0:x16}",
             member_die.GetID());
    return std::nullopt;
  }
  uint64_t member_offset = member_offset_opt.value();

  std::optional<uint64_t> bit_offset =
      member_die.GetAttributeValueAsOptionalUnsigned(
          llvm::dwarf::DW_AT_data_bit_offset);
  std::optional<uint64_t> bit_size =
      member_die.GetAttributeValueAsOptionalUnsigned(
          llvm::dwarf::DW_AT_bit_size);

  std::optional<uint64_t> artificial_opt =
      member_die.GetAttributeValueAsOptionalUnsigned(
          llvm::dwarf::DW_AT_artificial);
  bool is_artificial =
      artificial_opt.has_value() && artificial_opt.value() != 0;

  LLDB_LOG(log, "ParseMember: Created member {0} at offset {1}, type {2}{3}",
           member_name.value_or("<unnamed>"), member_offset,
           member_type_ref->get()->GetDisplayName(),
           is_artificial ? " (artificial)" : "");

  if (bit_offset.has_value() || bit_size.has_value()) {
    LLDB_LOG(log, "ParseMember: Bit field detected - offset: {0}, size: {1}",
             bit_offset.value_or(0), bit_size.value_or(0));
  }

  return OxCamlMember{std::move(member_name),
                      member_type_ref,
                      member_offset,
                      bit_offset,
                      bit_size,
                      is_artificial};
}

std::optional<OxCamlVariantPart>
DWARFASTParserOxCaml::ParseVariantPart(const SymbolContext &sc,
                                       const DWARFDIE &variant_part_die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);
  user_id_t die_id = variant_part_die.GetID();

  LLDB_LOG(log, "ParseVariantPart: DIE 0x{0:x16}", die_id);

  DWARFDIE discriminator_die = variant_part_die.GetAttributeValueAsReferenceDIE(
      llvm::dwarf::DW_AT_discr);
  if (!discriminator_die.IsValid()) {
    LLDB_LOG(log, "ParseVariantPart: No valid discriminator reference");
    return std::nullopt;
  }

  LLDB_LOG(log, "ParseVariantPart: Found discriminator DIE 0x{0:x16}",
           discriminator_die.GetID());

  auto discriminator_member = ParseMember(sc, discriminator_die);
  if (!discriminator_member.has_value()) {
    LLDB_LOG(log, "ParseVariantPart: Failed to parse discriminator member");
    return std::nullopt;
  }

  LLDB_LOG(log, "ParseVariantPart: Discriminator at offset {0}, type: {1}",
           discriminator_member->data_member_location,
           discriminator_member->GetType()->GetDisplayName());

  std::vector<OxCamlVariantPart::Variant> variants;

  for (DWARFDIE child_die : variant_part_die.children()) {
    if (child_die.Tag() != llvm::dwarf::DW_TAG_variant)
      continue;

    LLDB_LOG(log, "ParseVariantPart: Parsing variant DIE 0x{0:x16}",
             child_die.GetID());

    auto discr_value_opt = child_die.GetAttributeValueAsOptionalUnsigned(
        llvm::dwarf::DW_AT_discr_value);
    if (!discr_value_opt.has_value()) {
      LLDB_LOG(
          log,
          "ParseVariantPart: Variant has no discriminator value, skipping");
      continue;
    }
    uint64_t discr_value = discr_value_opt.value();

    LLDB_LOG(log, "ParseVariantPart: Variant discriminator value: {0}",
             discr_value);

    std::vector<OxCamlMember> variant_members;
    for (DWARFDIE variant_child_die : child_die.children()) {
      if (variant_child_die.Tag() == llvm::dwarf::DW_TAG_member) {
        auto member = ParseMember(sc, variant_child_die);
        if (member.has_value()) {
          variant_members.push_back(std::move(*member));
          LLDB_LOG(log, "ParseVariantPart: Added variant member {0}",
                   member->name.value_or("<unnamed>"));
        }
      }
    }

    variants.push_back({discr_value, std::move(variant_members)});
    LLDB_LOG(
        log,
        "ParseVariantPart: Added variant with discriminator {0}, {1} members",
        discr_value, variants.back().members.size());
  }

  LLDB_LOG(log, "ParseVariantPart: Created variant part with {0} variants",
           variants.size());

  return OxCamlVariantPart{std::move(*discriminator_member),
                           std::move(variants)};
}

std::unique_ptr<OxCamlType>
DWARFASTParserOxCaml::ParseArrayType(const SymbolContext &sc,
                                     const DWARFDIE &die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);
  user_id_t die_id = die.GetID();

  LLDB_LOG(log, "ParseArrayType: DIE 0x{0:x16}", die_id);

  DWARFDIE element_type_die =
      die.GetAttributeValueAsReferenceDIE(llvm::dwarf::DW_AT_type);
  if (!element_type_die.IsValid()) {
    LLDB_LOG(log, "ParseArrayType: No valid element type found");
    return nullptr;
  }

  std::optional<std::string> name = ExtractTypeName(die);

  ParseTypeFromDWARF(sc, element_type_die, nullptr);

  auto element_type_opt = m_oxcaml_typesystem.GetType(element_type_die.GetID());
  OX_ASSERT(element_type_opt.has_value(),
            "Failed to get reference for element type DIE 0x{0:x}",
            element_type_die.GetID());
  Reference<OxCamlType> *element_type_ref = element_type_opt.value();

  std::optional<uint64_t> byte_stride_opt =
      die.GetAttributeValueAsOptionalUnsigned(llvm::dwarf::DW_AT_byte_stride);
  uint64_t byte_stride;
  if (byte_stride_opt.has_value()) {
    byte_stride = byte_stride_opt.value();
  } else {
    byte_stride = element_type_ref->get()->GetByteSize();
    LLDB_LOG(log,
             "ParseArrayType: No explicit byte_stride, using element size: {0}",
             byte_stride);
  }

  std::optional<uint64_t> count;
  for (DWARFDIE child_die : die.children()) {
    if (child_die.Tag() == llvm::dwarf::DW_TAG_subrange_type) {
      auto subrange_count_opt = child_die.GetAttributeValueAsOptionalUnsigned(
          llvm::dwarf::DW_AT_count);
      if (subrange_count_opt.has_value()) {
        count = subrange_count_opt.value();
        LLDB_LOG(log, "ParseArrayType: Found subrange count: {0}",
                 count.value());
      }
      break;
    }
  }

  LLDB_LOG(log,
           "ParseArrayType: Creating OxCamlArrayType with element type {0}, "
           "stride {1}, count {2}",
           element_type_ref->get()->GetDisplayName(), byte_stride,
           count.has_value() ? std::to_string(count.value()) : "unknown");

  return std::make_unique<OxCamlArrayType>(
      die_id, std::move(name), element_type_ref, count, byte_stride);
}

std::unique_ptr<OxCamlType>
DWARFASTParserOxCaml::ParseUnknownType(const DWARFDIE &die) {
  Log *log = GetLog(OxCamlLog::TypeParsing);
  lldb::user_id_t die_id = die.GetID();
  uint32_t dwarf_tag = die.Tag();

  LLDB_LOG(log,
           "ParseUnknownType: Creating unknown type for DIE 0x{0:x16}, DWARF "
           "tag 0x{1:x}",
           die_id, dwarf_tag);

  std::optional<std::string> name = ExtractTypeName(die);
  std::optional<uint64_t> byte_size_opt =
      die.GetAttributeValueAsOptionalUnsigned(llvm::dwarf::DW_AT_byte_size);
  uint64_t byte_size = byte_size_opt.value_or(DEFAULT_OCAML_VALUE_SIZE);

  LLDB_LOG(log, "ParseUnknownType: name=\"{0}\", byte_size={1}",
           name ? *name : "<anonymous>", byte_size);

  return std::make_unique<OxCamlUnknownType>(die_id, std::move(name), byte_size,
                                             dwarf_tag);
}
