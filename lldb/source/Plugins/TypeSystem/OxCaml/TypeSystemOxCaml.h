//===-- TypeSystemOxCaml.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_TYPESYSTEM_OXCAML_TYPESYSTEMOXCAML_H
#define LLDB_SOURCE_PLUGINS_TYPESYSTEM_OXCAML_TYPESYSTEMOXCAML_H

#include "lldb/Symbol/TypeSystem.h"
#include "lldb/Core/PluginInterface.h"
#include "lldb/lldb-private.h"
#include "lldb/Core/PluginManager.h"
#include "../../SymbolFile/DWARF/DWARFDIE.h"

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lldb_private {

namespace plugin {
namespace dwarf {
class DWARFASTParser;
} // namespace dwarf
} // namespace plugin

using namespace lldb_private::plugin::dwarf;

// Forward declaration
class OxCamlType;

class TypeSystemOxCaml : public TypeSystem {
  /// LLVM RTTI support.
  static char ID;

public:
  /// LLVM RTTI support.
  /// \{
  bool isA(const void *ClassID) const override { return ClassID == &ID; }
  static bool classof(const TypeSystem *ts) { return ts->isA(&ID); }
  /// \}

  TypeSystemOxCaml();
  ~TypeSystemOxCaml() override;

  static lldb::TypeSystemSP CreateInstance(lldb::LanguageType language, Module *module, Target *target);
  static void Initialize();
  static void Terminate();
  static llvm::StringRef GetPluginNameStatic() { return "oxcaml"; }

  static LanguageSet GetSupportedLanguagesForTypes() {
    LanguageSet languages;
    languages.Insert(lldb::eLanguageTypeOCaml);
    return languages;
  }
  static LanguageSet GetSupportedLanguagesForExpressions() {
    return LanguageSet(); // No expression support yet
  }

  // Core plugin interface methods
  llvm::StringRef GetPluginName() override { return GetPluginNameStatic(); }
  plugin::dwarf::DWARFASTParser *GetDWARFParser() override;
  bool SupportsLanguage(lldb::LanguageType language) override { return language == lldb::eLanguageTypeOCaml; }

  CompilerType GetTypeFromMangledTypename(ConstString mangled_typename);

  // Essential pure virtual methods that must be implemented
  ConstString DeclGetName(void *opaque_decl) override { return ConstString(); }
  CompilerType GetTypeForDecl(void *opaque_decl) override { return CompilerType(); }

  ConstString DeclContextGetName(void *opaque_decl_ctx) override { return ConstString(); }
  ConstString DeclContextGetScopeQualifiedName(void *opaque_decl_ctx) override { return ConstString(); }
  bool DeclContextIsClassMethod(void *opaque_decl_ctx) override { return false; }
  bool DeclContextIsContainedInLookup(void *opaque_decl_ctx, void *other_opaque_decl_ctx) override { return false; }
  lldb::LanguageType DeclContextGetLanguage(void *opaque_decl_ctx) override { return lldb::eLanguageTypeOCaml; }

#ifndef NDEBUG
  bool Verify(lldb::opaque_compiler_type_t type) override { 
    // For now, accept any non-null type as valid
    return type != nullptr;
  }
#endif

  // Type test functions
  bool IsArrayType(lldb::opaque_compiler_type_t type, CompilerType *element_type, uint64_t *size, bool *is_incomplete) override { return false; }
  bool IsAggregateType(lldb::opaque_compiler_type_t type) override { return false; }
  bool IsCharType(lldb::opaque_compiler_type_t type) override { return false; }
  bool IsCompleteType(lldb::opaque_compiler_type_t type) override { return false; }
  bool IsDefined(lldb::opaque_compiler_type_t type) override { return false; }
  bool IsFloatingPointType(lldb::opaque_compiler_type_t type, uint32_t &count, bool &is_complex) override { return false; }
  bool IsFunctionType(lldb::opaque_compiler_type_t type) override { return false; }
  size_t GetNumberOfFunctionArguments(lldb::opaque_compiler_type_t type) override { return 0; }
  CompilerType GetFunctionArgumentAtIndex(lldb::opaque_compiler_type_t type, const size_t index) override { return CompilerType(); }
  bool IsFunctionPointerType(lldb::opaque_compiler_type_t type) override { return false; }
  bool IsMemberFunctionPointerType(lldb::opaque_compiler_type_t type) override { return false; }
  bool IsBlockPointerType(lldb::opaque_compiler_type_t type, CompilerType *function_pointer_type_ptr) override { return false; }
  bool IsIntegerType(lldb::opaque_compiler_type_t type, bool &is_signed) override { 
    is_signed = false;  // OCaml values are unsigned tagged pointers
    return true; 
  }
  bool IsScopedEnumerationType(lldb::opaque_compiler_type_t type) override { return false; }
  bool IsPossibleDynamicType(lldb::opaque_compiler_type_t type, CompilerType *target_type, bool check_cplusplus, bool check_objc) override { return false; }
  bool IsPointerType(lldb::opaque_compiler_type_t type, CompilerType *pointee_type) override { return false; }
  bool IsReferenceType(lldb::opaque_compiler_type_t type, CompilerType *pointee_type, bool *is_rvalue) override { return false; }
  bool IsPointerOrReferenceType(lldb::opaque_compiler_type_t type, CompilerType *pointee_type) override { return false; }
  bool IsScalarType(lldb::opaque_compiler_type_t type) override { return true; }  // OCaml values are scalars
  bool IsVoidType(lldb::opaque_compiler_type_t type) override { return false; }
  bool CanPassInRegisters(const CompilerType &type) override { return true; }  // OCaml passes values in registers

  // Essential system info
  uint32_t GetPointerByteSize() override { return 8; }

  // Type names and info - essential methods
  ConstString GetTypeName(lldb::opaque_compiler_type_t type, bool BaseOnly) override;
  unsigned GetPtrAuthKey(lldb::opaque_compiler_type_t type) override { return 0; }
  unsigned GetPtrAuthDiscriminator(lldb::opaque_compiler_type_t type) override { return 0; }
  bool GetPtrAuthAddressDiversity(lldb::opaque_compiler_type_t type) override { return false; }
  ConstString GetDisplayTypeName(lldb::opaque_compiler_type_t type) override;
  const llvm::fltSemantics &GetFloatTypeSemantics(size_t byte_size) override { return llvm::APFloat::IEEEdouble(); }
  lldb::BasicType GetBasicTypeEnumeration(lldb::opaque_compiler_type_t type) override { return lldb::eBasicTypeInvalid; }
  uint32_t GetTypeInfo(lldb::opaque_compiler_type_t type, CompilerType *pointee_or_element_clang_type) override { return 0; }
  lldb::TypeClass GetTypeClass(lldb::opaque_compiler_type_t type) override { return lldb::eTypeClassBuiltin; }  // OCaml values are built-in types

  // Type completion and introspection
  bool GetCompleteType(lldb::opaque_compiler_type_t type) override { return false; }
  lldb::LanguageType GetMinimumLanguage(lldb::opaque_compiler_type_t type) override { return lldb::eLanguageTypeOCaml; }
  llvm::Expected<uint64_t> GetBitSize(lldb::opaque_compiler_type_t type, ExecutionContextScope *exe_scope) override;

  // Creating related types
  CompilerType GetCanonicalType(lldb::opaque_compiler_type_t type) override { return CompilerType(); }
  int GetFunctionArgumentCount(lldb::opaque_compiler_type_t type) override { return -1; }
  CompilerType GetFunctionArgumentTypeAtIndex(lldb::opaque_compiler_type_t type, size_t idx) override { return CompilerType(); }
  CompilerType GetFunctionReturnType(lldb::opaque_compiler_type_t type) override { return CompilerType(); }
  size_t GetNumMemberFunctions(lldb::opaque_compiler_type_t type) override { return 0; }
  TypeMemberFunctionImpl GetMemberFunctionAtIndex(lldb::opaque_compiler_type_t type, size_t idx) override { return TypeMemberFunctionImpl(); }
  CompilerType GetPointeeType(lldb::opaque_compiler_type_t type) override { return CompilerType(); }
  CompilerType GetPointerType(lldb::opaque_compiler_type_t type) override { return CompilerType(); }

  CompilerType GetArrayElementType(lldb::opaque_compiler_type_t type, ExecutionContextScope *exe_scope) override;
  CompilerType GetEnumerationIntegerType(lldb::opaque_compiler_type_t type) override;
  CompilerType GetNonReferenceType(lldb::opaque_compiler_type_t type) override;
  CompilerType GetLValueReferenceType(lldb::opaque_compiler_type_t type) override;
  CompilerType GetRValueReferenceType(lldb::opaque_compiler_type_t type) override;
  CompilerType GetAtomicType(lldb::opaque_compiler_type_t type) override;
  CompilerType AddConstModifier(lldb::opaque_compiler_type_t type) override;
  CompilerType AddVolatileModifier(lldb::opaque_compiler_type_t type) override;
  CompilerType AddRestrictModifier(lldb::opaque_compiler_type_t type) override;
  CompilerType CreateTypedef(lldb::opaque_compiler_type_t type, const char *name, const CompilerDeclContext &decl_ctx, uint32_t opaque_payload) override;

  // Exploring the type - essential methods
  lldb::Encoding GetEncoding(lldb::opaque_compiler_type_t type, uint64_t &count) override { 
    count = 1;
    return lldb::eEncodingUint;  // OCaml values are essentially tagged pointers/integers
  }
  llvm::Expected<uint32_t> GetNumChildren(lldb::opaque_compiler_type_t type, bool omit_empty_base_classes, const ExecutionContext *exe_ctx) override;
  uint32_t GetNumFields(lldb::opaque_compiler_type_t type) override { return 0; }
  CompilerType GetFieldAtIndex(lldb::opaque_compiler_type_t type, size_t idx, std::string &name, uint64_t *bit_offset_ptr, uint32_t *bitfield_bit_size_ptr, bool *is_bitfield_ptr) override { return CompilerType(); }
  llvm::Expected<CompilerType> GetChildCompilerTypeAtIndex(lldb::opaque_compiler_type_t type, ExecutionContext *exe_ctx, size_t idx, bool transparent_pointers, bool omit_empty_base_classes, bool ignore_array_bounds, std::string &child_name, uint32_t &child_byte_size, int32_t &child_byte_offset, uint32_t &child_bitfield_bit_size, uint32_t &child_bitfield_bit_offset, bool &child_is_base_class, bool &child_is_deref_of_parent, ValueObject *valobj, uint64_t &language_flags) override;
  size_t GetIndexOfChildMemberWithName(lldb::opaque_compiler_type_t type, llvm::StringRef name, bool omit_empty_base_classes, std::vector<uint32_t> &child_indexes) override;

  lldb::Format GetFormat(lldb::opaque_compiler_type_t type) override { 
    return lldb::eFormatHex;  // OCaml values are best displayed in hex to see tags
  }
  uint32_t GetNumDirectBaseClasses(lldb::opaque_compiler_type_t type) override;
  uint32_t GetNumVirtualBaseClasses(lldb::opaque_compiler_type_t type) override;
  CompilerType GetDirectBaseClassAtIndex(lldb::opaque_compiler_type_t type, size_t idx, uint32_t *bit_offset_ptr) override;
  CompilerType GetVirtualBaseClassAtIndex(lldb::opaque_compiler_type_t type, size_t idx, uint32_t *bit_offset_ptr) override;
  llvm::Expected<CompilerType> GetDereferencedType(lldb::opaque_compiler_type_t type, ExecutionContext *exe_ctx, std::string &deref_name, uint32_t &deref_byte_size, int32_t &deref_byte_offset, ValueObject *valobj, uint64_t &language_flags) override;
  llvm::Expected<uint32_t> GetIndexOfChildWithName(lldb::opaque_compiler_type_t type, llvm::StringRef name, bool omit_empty_base_classes) override;

  void dump(lldb::opaque_compiler_type_t type) const override;
  bool DumpTypeValue(lldb::opaque_compiler_type_t type, Stream &s, lldb::Format format, const DataExtractor &data, lldb::offset_t data_offset, size_t data_byte_size, uint32_t bitfield_bit_size, uint32_t bitfield_bit_offset, ExecutionContextScope *exe_scope) override { return false; }
  void DumpTypeDescription(lldb::opaque_compiler_type_t type, lldb::DescriptionLevel level) override;
  void DumpTypeDescription(lldb::opaque_compiler_type_t type, Stream &s, lldb::DescriptionLevel level) override;
  void Dump(llvm::raw_ostream &output, llvm::StringRef filter) override;

  bool IsRuntimeGeneratedType(lldb::opaque_compiler_type_t type) override { return false; }
  unsigned GetTypeQualifiers(lldb::opaque_compiler_type_t type) override { return 0; }
  std::optional<size_t> GetTypeBitAlign(lldb::opaque_compiler_type_t type, ExecutionContextScope *exe_scope) override { return std::nullopt; }
  CompilerType GetBasicTypeFromAST(lldb::BasicType basic_type) override { return CompilerType(); }
  CompilerType GetBuiltinTypeForEncodingAndBitSize(lldb::Encoding encoding, size_t bit_size) override { return CompilerType(); }
  bool IsBeingDefined(lldb::opaque_compiler_type_t type) override { return false; }
  bool IsConst(lldb::opaque_compiler_type_t type) override { return false; }
  uint32_t IsHomogeneousAggregate(lldb::opaque_compiler_type_t type, CompilerType *base_type_ptr) override { return 0; }
  bool IsPolymorphicClass(lldb::opaque_compiler_type_t type) override { return false; }
  bool IsTypedefType(lldb::opaque_compiler_type_t type) override;
  CompilerType GetTypedefedType(lldb::opaque_compiler_type_t type) override;
  bool IsVectorType(lldb::opaque_compiler_type_t type, CompilerType *element_type, uint64_t *size) override { return false; }
  CompilerType GetFullyUnqualifiedType(lldb::opaque_compiler_type_t type) override;

  // GetTypeForFormatters - Prepares a type for the formatter system.
  // This method is called when LLDB needs to match formatters to a type.
  // The returned CompilerType is used to get the type name for formatter matching.
  // Currently returns the type wrapped in a CompilerType without modification.
  CompilerType GetTypeForFormatters(void *type) override;

  // Type registry methods
  std::optional<OxCamlType*> GetType(lldb::user_id_t die_id);
  void RegisterType(lldb::user_id_t die_id, std::unique_ptr<OxCamlType> type);

private:
  std::unique_ptr<plugin::dwarf::DWARFASTParser> m_dwarf_parser;
  
  // Type registry: maps DIE ID to created OxCamlType
  std::unordered_map<lldb::user_id_t, std::unique_ptr<OxCamlType>> m_type_registry;
};

// OxCamlType class hierarchy
class OxCamlType {
public:
  enum Kind { Base, Typedef, Enum, Pointer, Structure };
  
  OxCamlType(Kind k, lldb::user_id_t die_id, std::optional<std::string> name) 
    : m_kind(k), m_die_id(die_id), m_name(std::move(name)) {}
  virtual ~OxCamlType() = default;
  
  Kind GetKind() const { return m_kind; }
  lldb::user_id_t GetDieId() const { return m_die_id; }
  
  // Non-virtual - uses DWARF name if present, otherwise falls back to default
  std::string GetDisplayName() const { 
    return m_name.value_or(GetDefaultDisplayName()); 
  }
  
  virtual uint64_t GetByteSize() const = 0;
  
protected:
  // Derived classes provide their fallback name
  virtual std::string GetDefaultDisplayName() const = 0;
  
  Kind m_kind;
  lldb::user_id_t m_die_id;
  std::optional<std::string> m_name;  // From DW_AT_name if present
};

class OxCamlBaseType : public OxCamlType {
public:
  OxCamlBaseType(lldb::user_id_t die_id, std::optional<std::string> name)
    : OxCamlType(Base, die_id, std::move(name)) {}
  
  uint64_t GetByteSize() const override { return 8; }
  
protected:
  std::string GetDefaultDisplayName() const override { 
    return "ocaml_value";  // Default for base types without names
  }
};

class OxCamlTypedefType : public OxCamlType {
  OxCamlType* m_underlying;  // Non-owning pointer to registry-owned type
  
public:
  OxCamlTypedefType(lldb::user_id_t die_id, std::optional<std::string> name, 
                    OxCamlType* underlying)
    : OxCamlType(Typedef, die_id, std::move(name)), 
      m_underlying(underlying) {}
  
  OxCamlType* GetUnderlyingType() const { return m_underlying; }
  uint64_t GetByteSize() const override { return m_underlying->GetByteSize(); }
  
protected:
  std::string GetDefaultDisplayName() const override {
    // Anonymous typedef uses underlying type's name
    return m_underlying->GetDisplayName();
  }
};

class OxCamlEnumType : public OxCamlType {
public:
  struct Enumerator {
    std::string name;
    int64_t value;
  };
  
private:
  uint64_t m_byte_size;
  std::vector<Enumerator> m_enumerators;
  
public:
  OxCamlEnumType(lldb::user_id_t die_id, std::optional<std::string> name,
                 uint64_t byte_size, std::vector<Enumerator> enumerators)
    : OxCamlType(Enum, die_id, std::move(name)), 
      m_byte_size(byte_size), m_enumerators(std::move(enumerators)) {}
  
  uint64_t GetByteSize() const override { return m_byte_size; }
  const std::vector<Enumerator>& GetEnumerators() const { return m_enumerators; }
  
  // Find enumerator by value
  std::optional<std::string> GetEnumeratorName(int64_t value) const {
    for (const auto& e : m_enumerators) {
      if (e.value == value)
        return e.name;
    }
    return std::nullopt;
  }
  
  // Find enumerator by name
  std::optional<int64_t> GetEnumeratorValue(const std::string& name) const {
    for (const auto& e : m_enumerators) {
      if (e.name == name)
        return e.value;
    }
    return std::nullopt;
  }
  
protected:
  std::string GetDefaultDisplayName() const override { 
    return "enum";  // Generic name for anonymous enums
  }
};

class OxCamlPointerType : public OxCamlType {
  OxCamlType* m_pointed_to;  // Non-owning pointer to registry-owned type
  
public:
  OxCamlPointerType(lldb::user_id_t die_id, std::optional<std::string> name,
                    OxCamlType* pointed_to)
    : OxCamlType(Pointer, die_id, std::move(name)), 
      m_pointed_to(pointed_to) {}
  
  OxCamlType* GetPointedToType() const { return m_pointed_to; }
  uint64_t GetByteSize() const override { return 8; }  // Pointer size is always 8 bytes
  
protected:
  std::string GetDefaultDisplayName() const override {
    return m_pointed_to ? m_pointed_to->GetDisplayName() + " *" : "void *";
  }
};

class OxCamlStructureType : public OxCamlType {
public:
  struct Member {
    std::optional<std::string> name;  // Optional for tuples
    OxCamlType* type;                 // Non-owning pointer to member type
    uint64_t offset;                  // Byte offset within structure
  };
  
private:
  uint64_t m_byte_size;
  std::vector<Member> m_members;
  
public:
  OxCamlStructureType(lldb::user_id_t die_id, std::optional<std::string> name,
                      uint64_t byte_size, std::vector<Member> members)
    : OxCamlType(Structure, die_id, std::move(name)), 
      m_byte_size(byte_size), m_members(std::move(members)) {}
  
  uint64_t GetByteSize() const override { return m_byte_size; }
  const std::vector<Member>& GetMembers() const { return m_members; }
  
  // Check if this is likely a tuple (no member names)
  bool IsTuple() const {
    if (m_members.empty())
      return false;
    
    for (const auto& member : m_members) {
      if (member.name.has_value())
        return false;  // If any member has a name, it's not a tuple
    }
    return true;  // All members have no names, it's a tuple
  }
  
  // Get member by name (for records)
  std::optional<Member> GetMemberByName(const std::string& name) const {
    for (const auto& m : m_members) {
      if (m.name && m.name.value() == name)
        return m;
    }
    return std::nullopt;
  }
  
  // Get member by index (for tuples)
  std::optional<Member> GetMemberByIndex(size_t index) const {
    if (index < m_members.size())
      return m_members[index];
    return std::nullopt;
  }
  
protected:
  std::string GetDefaultDisplayName() const override {
    return IsTuple() ? "tuple" : "record";
  }
};

} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_TYPESYSTEM_OXCAML_TYPESYSTEMOXCAML_H
