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
#include "lldb/Utility/Reference.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lldb_private {

// Forward declarations
class OxCamlType;
class OxCamlEnumType;

// OxCamlType class hierarchy
class OxCamlType {
public:
  enum Kind { Base, Typedef, Enum, Pointer, Structure, Placeholder, Unknown };

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

  // Virtual method to get pointer adjustment offset for this type
  // Returns the offset to apply when dereferencing a pointer to this type
  // Most types return 0, but some (like structures with custom base offset) may return non-zero
  virtual int64_t GetPointerAdjustmentOffset() const { return 0; }

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

class OxCamlPlaceholderType : public OxCamlType {
  uint64_t m_byte_size;

public:
  OxCamlPlaceholderType(lldb::user_id_t die_id,
                        std::optional<std::string> name,
                        uint64_t byte_size)
    : OxCamlType(Placeholder, die_id, std::move(name)),
      m_byte_size(byte_size) {}

  uint64_t GetByteSize() const override;

protected:
  std::string GetDefaultDisplayName() const override;
};

class OxCamlUnknownType : public OxCamlType {
  uint64_t m_byte_size;
  uint32_t m_dwarf_tag;  // Store the DWARF tag that wasn't recognized

public:
  OxCamlUnknownType(lldb::user_id_t die_id,
                    std::optional<std::string> name,
                    uint64_t byte_size,
                    uint32_t dwarf_tag);

  uint64_t GetByteSize() const override;
  uint32_t GetDwarfTag() const;

protected:
  std::string GetDefaultDisplayName() const override;
};

class OxCamlTypedefType : public OxCamlType {
  Reference<OxCamlType>* m_underlying_ref;  // Weak pointer to registry-owned Reference

public:
  OxCamlTypedefType(lldb::user_id_t die_id, std::optional<std::string> name,
                    Reference<OxCamlType>* underlying_ref)
    : OxCamlType(Typedef, die_id, std::move(name)),
      m_underlying_ref(underlying_ref) {}

  OxCamlType* GetUnderlyingType() const { return m_underlying_ref->get(); }
  Reference<OxCamlType>* GetUnderlyingReference() const { return m_underlying_ref; }
  uint64_t GetByteSize() const override { return GetUnderlyingType()->GetByteSize(); }

protected:
  std::string GetDefaultDisplayName() const override {
    return GetUnderlyingType()->GetDisplayName();
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
  Reference<OxCamlType>* m_pointed_to_ref;  // Weak pointer to registry-owned Reference

public:
  OxCamlPointerType(lldb::user_id_t die_id, std::optional<std::string> name,
                    Reference<OxCamlType>* pointed_to_ref)
    : OxCamlType(Pointer, die_id, std::move(name)),
      m_pointed_to_ref(pointed_to_ref) {}

  OxCamlType* GetPointedToType() const { return m_pointed_to_ref->get(); }
  uint64_t GetByteSize() const override { return 8; }

protected:
  std::string GetDefaultDisplayName() const override {
    return GetPointedToType()->GetDisplayName() + " *";
  }
};

// Unified member structure for both regular members and variant members
struct OxCamlMember {
  std::optional<std::string> name;     // DW_AT_name (if present)
  Reference<OxCamlType>* type_ref;     // Weak pointer to registry-owned Reference
  uint64_t data_member_location;       // DW_AT_data_member_location

  // For bit fields
  std::optional<uint64_t> bit_offset;  // DW_AT_data_bit_offset
  std::optional<uint64_t> bit_size;    // DW_AT_bit_size

  // DWARF artificial attribute
  bool is_artificial = false;          // DW_AT_artificial

  bool IsBitField() const { return bit_offset.has_value() && bit_size.has_value(); }
  OxCamlType* GetType() const { 
    if (!type_ref) {
      llvm::report_fatal_error("Member type reference is null - this should never happen");
    }
    return type_ref->get(); 
  }
};

// Represents DW_TAG_variant_part with discriminator and variant cases
class OxCamlVariantPart {
public:
  struct Variant {
    uint64_t discriminator_value;         // DW_AT_discr_value
    std::vector<OxCamlMember> members;    // DW_TAG_member children
  };

private:
  OxCamlMember m_discriminator;
  std::vector<Variant> m_variants;

public:
  OxCamlVariantPart(OxCamlMember discriminator, std::vector<Variant> variants)
    : m_discriminator(std::move(discriminator)), m_variants(std::move(variants)) {}

  const OxCamlMember& GetDiscriminator() const { return m_discriminator; }
  const std::vector<Variant>& GetVariants() const { return m_variants; }

  // Check if discriminator is artificial (compiler-generated)
  // Note: Only artificial discriminators with exactly one member in the active variant
  // should be handled transparently. All other cases use regular variant formatting.
  bool HasArtificialDiscriminator() const { return m_discriminator.is_artificial; }

  // Find active variant by discriminator value
  std::optional<const Variant*> GetActiveVariant(uint64_t discr_value) const {
    for (const auto& variant : m_variants) {
      if (variant.discriminator_value == discr_value)
        return &variant;
    }
    return std::nullopt;
  }
};

class OxCamlStructureType : public OxCamlType {
private:
  uint64_t m_byte_size;
  std::vector<OxCamlMember> m_members;            // Regular members
  std::vector<OxCamlVariantPart> m_variant_parts; // Variant parts

  // Custom base offset from DW_AT_ocaml_offset_record_from_pointer. This is
  // specific to the DWARF encoding used by the OxCaml compiler. It supports an
  // ad-hoc attribute (see DW_AT_ocaml_offset_record_from_pointer in
  // DWARFASTParserOxCaml.cpp) that specifies a byte offset to apply when
  // dealing with a pointer to a structure. This allows one to factor in the
  // header field of a block when the actual pointer points to the first entry
  // in the block.
  int64_t m_base_offset;

public:
  // Constructor for structures with both regular members and variant parts
  OxCamlStructureType(lldb::user_id_t die_id, std::optional<std::string> name,
                      uint64_t byte_size, std::vector<OxCamlMember> members,
                      std::vector<OxCamlVariantPart> variant_parts = {},
                      int64_t base_offset = 0)
    : OxCamlType(Structure, die_id, std::move(name)),
      m_byte_size(byte_size), m_members(std::move(members)),
      m_variant_parts(std::move(variant_parts)), m_base_offset(base_offset) {}

  uint64_t GetByteSize() const override { return m_byte_size; }
  const std::vector<OxCamlMember>& GetMembers() const { return m_members; }
  const std::vector<OxCamlVariantPart>& GetVariantParts() const { return m_variant_parts; }

  // Override to return custom base offset from DW_AT_ocaml_offset_record_from_pointer
  int64_t GetPointerAdjustmentOffset() const override {
    return m_base_offset;
  }

  // Check if this is an OCaml variant (one variant part, no regular members)
  bool IsOCamlVariant() const {
    return m_variant_parts.size() == 1 && m_members.empty();
  }

  // Check if this is likely a tuple (no member names, no variant parts)
  bool IsTuple() const {
    if (m_members.empty() || !m_variant_parts.empty())
      return false;

    for (const auto& member : m_members) {
      if (member.name.has_value())
        return false;  // If any member has a name, it's not a tuple
    }
    return true;  // All members have no names, it's a tuple
  }

  // Get member by name (for records)
  std::optional<OxCamlMember> GetMemberByName(const std::string& name) const {
    for (const auto& m : m_members) {
      if (m.name && m.name.value() == name)
        return m;
    }
    return std::nullopt;
  }

  // Get member by index (for tuples)
  std::optional<OxCamlMember> GetMemberByIndex(size_t index) const {
    if (index < m_members.size())
      return m_members[index];
    return std::nullopt;
  }

protected:
  std::string GetDefaultDisplayName() const override {
    if (IsOCamlVariant())
      return "variant";
    return IsTuple() ? "tuple" : "record";
  }
};

} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_TYPESYSTEM_OXCAML_OXCAMLTYPES_H
