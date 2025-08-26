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
#include "llvm/Support/ErrorHandling.h"

#include "../../SymbolFile/DWARF/DWARFASTParserOxCaml.h"

using namespace lldb;
using namespace lldb_private;

LLDB_PLUGIN_DEFINE(TypeSystemOxCaml)

char TypeSystemOxCaml::ID;

TypeSystemOxCaml::TypeSystemOxCaml() : TypeSystem() {}

TypeSystemOxCaml::~TypeSystemOxCaml() = default;

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



llvm::Expected<uint64_t> TypeSystemOxCaml::GetBitSize(lldb::opaque_compiler_type_t type, ExecutionContextScope *exe_scope) {
  return 64; // Default OCaml value size
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
  return CompilerType(weak_from_this(), type);
}

// Methods with default implementations in base class are not overridden









CompilerType TypeSystemOxCaml::GetArrayElementType(lldb::opaque_compiler_type_t type, ExecutionContextScope *exe_scope) {
  llvm_unreachable("GetArrayElementType not implemented for OCaml");
}

CompilerType TypeSystemOxCaml::GetEnumerationIntegerType(lldb::opaque_compiler_type_t type) {
  llvm_unreachable("GetEnumerationIntegerType not implemented for OCaml");
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
  llvm_unreachable("DumpTypeDescription not implemented for OCaml");
}

void TypeSystemOxCaml::DumpTypeDescription(lldb::opaque_compiler_type_t type, Stream &s, lldb::DescriptionLevel level) {
  llvm_unreachable("DumpTypeDescription not implemented for OCaml");
}

void TypeSystemOxCaml::Dump(llvm::raw_ostream &output, llvm::StringRef filter) {
  // Nothing to dump
}









CompilerType TypeSystemOxCaml::GetFullyUnqualifiedType(lldb::opaque_compiler_type_t type) {
  llvm_unreachable("GetFullyUnqualifiedType not implemented for OCaml");
}