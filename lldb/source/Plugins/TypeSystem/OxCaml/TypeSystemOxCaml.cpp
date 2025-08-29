//===-- TypeSystemOxCaml.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TypeSystemOxCaml.h"

#include "lldb/Core/PluginManager.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Target/Language.h"
#include "lldb/Utility/Log.h"
#include "lldb/Host/StreamFile.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"

#include "../../SymbolFile/DWARF/DWARFASTParserOxCaml.h"

using namespace lldb;
using namespace lldb_private;

LLDB_PLUGIN_DEFINE(TypeSystemOxCaml)

char TypeSystemOxCaml::ID;

TypeSystemOxCaml::TypeSystemOxCaml() : TypeSystem() {
  // Registry starts empty - types created on demand
}

TypeSystemOxCaml::~TypeSystemOxCaml() {
  // Clear registry on destruction
  m_type_registry.clear();
}

plugin::dwarf::DWARFASTParser *TypeSystemOxCaml::GetDWARFParser() {
  if (!m_dwarf_parser)
    m_dwarf_parser = std::make_unique<DWARFASTParserOxCaml>(*this);
  return m_dwarf_parser.get();
}

lldb::TypeSystemSP TypeSystemOxCaml::CreateInstance(lldb::LanguageType language,
                                                    Module *module, Target *target) {
  if (language != eLanguageTypeOCaml)
    return TypeSystemSP();

  return std::make_shared<TypeSystemOxCaml>();
}

void TypeSystemOxCaml::Initialize() {
  PluginManager::RegisterPlugin(
      GetPluginNameStatic(), "OCaml type system", CreateInstance,
      GetSupportedLanguagesForTypes(), GetSupportedLanguagesForExpressions());
}

void TypeSystemOxCaml::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}

CompilerType TypeSystemOxCaml::GetTypeFromMangledTypename(ConstString mangled_typename) {
  return CompilerType();
}



ConstString TypeSystemOxCaml::GetTypeName(lldb::opaque_compiler_type_t type, bool BaseOnly) {
  if (static_cast<OxCamlType*>(type))
    return ConstString("ocaml_value");  // Universal format specifier for all OCaml types
  return ConstString();
}

ConstString TypeSystemOxCaml::GetDisplayTypeName(lldb::opaque_compiler_type_t type) {
  // Return the actual DWARF type name for display purposes
  // (while GetTypeName returns "ocaml_value" for formatter matching)
  if (auto* ocaml_type = static_cast<OxCamlType*>(type))
    return ConstString(ocaml_type->GetName());
  return ConstString();
}

llvm::Expected<uint64_t> TypeSystemOxCaml::GetBitSize(lldb::opaque_compiler_type_t type, ExecutionContextScope *exe_scope) {
  if (auto* ocaml_type = static_cast<OxCamlType*>(type))
    return ocaml_type->GetByteSize() * 8;  // Convert bytes to bits
  return llvm::createStringError(llvm::inconvertibleErrorCode(), "Invalid OxCaml type");
}

llvm::Expected<uint32_t> TypeSystemOxCaml::GetNumChildren(lldb::opaque_compiler_type_t type, bool omit_empty_base_classes, const ExecutionContext *exe_ctx) {
  return 0;
}

llvm::Expected<CompilerType> TypeSystemOxCaml::GetChildCompilerTypeAtIndex(lldb::opaque_compiler_type_t type, ExecutionContext *exe_ctx, size_t idx, bool transparent_pointers, bool omit_empty_base_classes, bool ignore_array_bounds, std::string &child_name, uint32_t &child_byte_size, int32_t &child_byte_offset, uint32_t &child_bitfield_bit_size, uint32_t &child_bitfield_bit_offset, bool &child_is_base_class, bool &child_is_deref_of_parent, ValueObject *valobj, uint64_t &language_flags) {
  return CompilerType();
}

size_t TypeSystemOxCaml::GetIndexOfChildMemberWithName(lldb::opaque_compiler_type_t type, llvm::StringRef name, bool omit_empty_base_classes, std::vector<uint32_t> &child_indexes) {
  return UINT32_MAX;
}

CompilerType TypeSystemOxCaml::GetTypeForFormatters(void *type) {
  // Use the provided type pointer directly
  return CompilerType(weak_from_this(), type);
}

// Type registry methods
std::optional<OxCamlType*> TypeSystemOxCaml::GetType(lldb::user_id_t die_id) {
  auto it = m_type_registry.find(die_id);
  if (it != m_type_registry.end()) {
    return it->second.get();
  }
  return std::nullopt;
}

void TypeSystemOxCaml::RegisterType(lldb::user_id_t die_id, std::unique_ptr<OxCamlType> type) {
  m_type_registry[die_id] = std::move(type);
}

bool TypeSystemOxCaml::IsTypedefType(lldb::opaque_compiler_type_t type) {
  if (auto* ocaml_type = static_cast<OxCamlType*>(type))
    return ocaml_type->GetKind() == OxCamlType::Typedef;
  return false;
}

CompilerType TypeSystemOxCaml::GetTypedefedType(lldb::opaque_compiler_type_t type) {
  if (auto* ocaml_type = static_cast<OxCamlType*>(type)) {
    if (ocaml_type->GetKind() == OxCamlType::Typedef) {
      auto* typedef_type = static_cast<OxCamlTypedefType*>(ocaml_type);
      return CompilerType(weak_from_this(), typedef_type->GetUnderlyingType());
    }
  }
  return CompilerType();
}


CompilerType TypeSystemOxCaml::GetArrayElementType(lldb::opaque_compiler_type_t type, ExecutionContextScope *exe_scope) {
  llvm_unreachable("GetArrayElementType not implemented for OCaml");
}

CompilerType TypeSystemOxCaml::GetEnumerationIntegerType(lldb::opaque_compiler_type_t type) {
  // For OCaml enums, return the same type since they're already integer-like
  if (auto* ocaml_type = static_cast<OxCamlType*>(type)) {
    if (ocaml_type->GetKind() == OxCamlType::Enum) {
      // OCaml enums are essentially tagged integers, return self
      return CompilerType(weak_from_this(), type);
    }
  }
  return CompilerType();
}

CompilerType TypeSystemOxCaml::GetNonReferenceType(lldb::opaque_compiler_type_t type) {
  llvm_unreachable("GetNonReferenceType not implemented for OCaml");
}

CompilerType TypeSystemOxCaml::GetLValueReferenceType(lldb::opaque_compiler_type_t type) {
  llvm_unreachable("GetLValueReferenceType not implemented for OCaml");
}

CompilerType TypeSystemOxCaml::GetRValueReferenceType(lldb::opaque_compiler_type_t type) {
  llvm_unreachable("GetRValueReferenceType not implemented for OCaml");
}

CompilerType TypeSystemOxCaml::GetAtomicType(lldb::opaque_compiler_type_t type) {
  llvm_unreachable("GetAtomicType not implemented for OCaml");
}

CompilerType TypeSystemOxCaml::AddConstModifier(lldb::opaque_compiler_type_t type) {
  llvm_unreachable("AddConstModifier not implemented for OCaml");
}

CompilerType TypeSystemOxCaml::AddVolatileModifier(lldb::opaque_compiler_type_t type) {
  llvm_unreachable("AddVolatileModifier not implemented for OCaml");
}

CompilerType TypeSystemOxCaml::AddRestrictModifier(lldb::opaque_compiler_type_t type) {
  llvm_unreachable("AddRestrictModifier not implemented for OCaml");
}

CompilerType TypeSystemOxCaml::CreateTypedef(lldb::opaque_compiler_type_t type, const char *name, const CompilerDeclContext &decl_ctx, uint32_t opaque_payload) {
  llvm_unreachable("CreateTypedef not implemented for OCaml");
}


uint32_t TypeSystemOxCaml::GetNumDirectBaseClasses(lldb::opaque_compiler_type_t type) {
  llvm_unreachable("GetNumDirectBaseClasses not implemented for OCaml");
}

uint32_t TypeSystemOxCaml::GetNumVirtualBaseClasses(lldb::opaque_compiler_type_t type) {
  llvm_unreachable("GetNumVirtualBaseClasses not implemented for OCaml");
}

CompilerType TypeSystemOxCaml::GetDirectBaseClassAtIndex(lldb::opaque_compiler_type_t type, size_t idx, uint32_t *bit_offset_ptr) {
  llvm_unreachable("GetDirectBaseClassAtIndex not implemented for OCaml");
}

CompilerType TypeSystemOxCaml::GetVirtualBaseClassAtIndex(lldb::opaque_compiler_type_t type, size_t idx, uint32_t *bit_offset_ptr) {
  llvm_unreachable("GetVirtualBaseClassAtIndex not implemented for OCaml");
}

llvm::Expected<CompilerType> TypeSystemOxCaml::GetDereferencedType(lldb::opaque_compiler_type_t type, ExecutionContext *exe_ctx, std::string &deref_name, uint32_t &deref_byte_size, int32_t &deref_byte_offset, ValueObject *valobj, uint64_t &language_flags) {
  llvm_unreachable("GetDereferencedType not implemented for OCaml");
}

llvm::Expected<uint32_t> TypeSystemOxCaml::GetIndexOfChildWithName(lldb::opaque_compiler_type_t type, llvm::StringRef name, bool omit_empty_base_classes) {
  llvm_unreachable("GetIndexOfChildWithName not implemented for OCaml");
}


void TypeSystemOxCaml::dump(lldb::opaque_compiler_type_t type) const {
  llvm_unreachable("dump not implemented for OCaml");
}


void TypeSystemOxCaml::DumpTypeDescription(lldb::opaque_compiler_type_t type, lldb::DescriptionLevel level) {
  // Call the Stream version with stdout
  StreamFile s(stdout, false);
  DumpTypeDescription(type, s, level);
}

void TypeSystemOxCaml::DumpTypeDescription(lldb::opaque_compiler_type_t type, Stream &s, lldb::DescriptionLevel level) {
  if (auto* ocaml_type = static_cast<OxCamlType*>(type))
    s.PutCString(ocaml_type->GetName());
}

void TypeSystemOxCaml::Dump(llvm::raw_ostream &output, llvm::StringRef filter) {
  output << "OxCaml TypeSystem - Type Registry:\n";
  output << "===================================\n";
  
  if (m_type_registry.empty()) {
    output << "(empty)\n";
    return;
  }
  
  for (const auto& [die_id, type] : m_type_registry) {
    std::string type_name = type->GetName();
    
    // Apply filter if provided
    if (!filter.empty() && type_name.find(filter.str()) == std::string::npos)
      continue;
      
    output << "DIE 0x" << llvm::format_hex(die_id, 0) 
           << ": " << type_name;
    
    // Show additional info based on type kind
    switch (type->GetKind()) {
      case OxCamlType::Base:
        output << " (base type, " << type->GetByteSize() << " bytes)";
        break;
      case OxCamlType::Typedef:
        {
          auto* td = static_cast<OxCamlTypedefType*>(type.get());
          output << " (typedef -> " << td->GetUnderlyingType()->GetName() << ")";
        }
        break;
      case OxCamlType::Enum:
        {
          auto* et = static_cast<OxCamlEnumType*>(type.get());
          output << " (enum type, " << et->GetEnumerators().size() << " enumerators, " << type->GetByteSize() << " bytes)";
        }
        break;
    }
    output << "\n";
  }
}


CompilerType TypeSystemOxCaml::GetFullyUnqualifiedType(lldb::opaque_compiler_type_t type) {
  // OCaml types don't have qualifiers like const/volatile, so return as-is
  return CompilerType(weak_from_this(), type);
}
