//===-- OxCamlTypes.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_TYPESYSTEM_OXCAML_OXCAMLTYPES_H
#define LLDB_SOURCE_PLUGINS_TYPESYSTEM_OXCAML_OXCAMLTYPES_H

#include "lldb/lldb-private.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lldb_private {

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

#endif // LLDB_SOURCE_PLUGINS_TYPESYSTEM_OXCAML_OXCAMLTYPES_H
