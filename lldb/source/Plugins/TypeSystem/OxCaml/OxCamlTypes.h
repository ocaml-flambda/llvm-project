//===-- OxCamlTypes.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_TYPESYSTEM_OXCAML_OXCAMLTYPES_H
#define LLDB_SOURCE_PLUGINS_TYPESYSTEM_OXCAML_OXCAMLTYPES_H

#include "../../Language/OxCaml/OxCamlHelpers.h"
#include "OxCamlDynamicLayoutValue.h"
#include "lldb/Utility/Reference.h"
#include "lldb/lldb-private.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lldb_private {

class OxCamlType;
class OxCamlEnumType;

class OxCamlType {
public:
  enum Kind {
    Value,
    UnboxedBase,
    Typedef,
    Enum,
    Pointer,
    Structure,
    Array,
    Placeholder,
    Unknown
  };

  OxCamlType(Kind k, lldb::user_id_t die_id, std::optional<std::string> name)
      : m_kind(k), m_die_id(die_id), m_name(std::move(name)) {}
  virtual ~OxCamlType() = default;

  Kind GetKind() const { return m_kind; }
  lldb::user_id_t GetDieId() const { return m_die_id; }

  std::string GetDisplayName() const {
    return m_name.value_or(GetDefaultDisplayName());
  }

  virtual uint64_t GetByteSize() const = 0;

  virtual std::string GetDefaultDisplayName() const = 0;

protected:
  Kind m_kind;
  lldb::user_id_t m_die_id;
  std::optional<std::string> m_name;
};

class OxCamlValueType : public OxCamlType {
public:
  OxCamlValueType(lldb::user_id_t die_id, std::optional<std::string> name)
      : OxCamlType(Value, die_id, std::move(name)) {}

  uint64_t GetByteSize() const override {
    return formatters::oxcaml::helpers::constants::WORD_SIZE;
  }

protected:
  std::string GetDefaultDisplayName() const override;
};

class OxCamlUnboxedBaseType : public OxCamlType {
public:
  enum BaseKind { Signed, Unsigned, Float };

private:
  uint64_t m_byte_size;
  BaseKind m_base_kind;

public:
  OxCamlUnboxedBaseType(lldb::user_id_t die_id, std::optional<std::string> name,
                        uint64_t byte_size, BaseKind base_kind)
      : OxCamlType(UnboxedBase, die_id, std::move(name)),
        m_byte_size(byte_size), m_base_kind(base_kind) {}

  uint64_t GetByteSize() const override { return m_byte_size; }
  BaseKind GetBaseKind() const { return m_base_kind; }

protected:
  std::string GetDefaultDisplayName() const override;
};

class OxCamlPlaceholderType : public OxCamlType {
  uint64_t m_byte_size;

public:
  OxCamlPlaceholderType(lldb::user_id_t die_id, std::optional<std::string> name,
                        uint64_t byte_size)
      : OxCamlType(Placeholder, die_id, std::move(name)),
        m_byte_size(byte_size) {}

  uint64_t GetByteSize() const override;

protected:
  std::string GetDefaultDisplayName() const override;
};

class OxCamlUnknownType : public OxCamlType {
  uint64_t m_byte_size;
  uint32_t m_dwarf_tag;

public:
  OxCamlUnknownType(lldb::user_id_t die_id, std::optional<std::string> name,
                    uint64_t byte_size, uint32_t dwarf_tag);

  uint64_t GetByteSize() const override;
  uint32_t GetDwarfTag() const;

protected:
  std::string GetDefaultDisplayName() const override;
};

class OxCamlTypedefType : public OxCamlType {
  Reference<OxCamlType> *m_underlying_ref;

public:
  OxCamlTypedefType(lldb::user_id_t die_id, std::optional<std::string> name,
                    Reference<OxCamlType> *underlying_ref)
      : OxCamlType(Typedef, die_id, std::move(name)),
        m_underlying_ref(underlying_ref) {}

  OxCamlType *GetUnderlyingType() const;
  uint64_t GetByteSize() const override;

protected:
  std::string GetDefaultDisplayName() const override;
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
      : OxCamlType(Enum, die_id, std::move(name)), m_byte_size(byte_size),
        m_enumerators(std::move(enumerators)) {}

  uint64_t GetByteSize() const override { return m_byte_size; }
  const std::vector<Enumerator> &GetEnumerators() const {
    return m_enumerators;
  }

  std::optional<std::string> GetEnumeratorName(int64_t value) const;
  std::optional<int64_t> GetEnumeratorValue(const std::string &name) const;

protected:
  std::string GetDefaultDisplayName() const override;
};

class OxCamlPointerType : public OxCamlType {
  Reference<OxCamlType> *m_pointed_to_ref;

public:
  OxCamlPointerType(lldb::user_id_t die_id, std::optional<std::string> name,
                    Reference<OxCamlType> *pointed_to_ref)
      : OxCamlType(Pointer, die_id, std::move(name)),
        m_pointed_to_ref(pointed_to_ref) {}

  OxCamlType *GetPointedToType() const;
  uint64_t GetByteSize() const override {
    return formatters::oxcaml::helpers::constants::WORD_SIZE;
  }

protected:
  std::string GetDefaultDisplayName() const override;
};

class OxCamlArrayType : public OxCamlType {
  Reference<OxCamlType> *m_element_type_ref;
  /// DW_AT_count, in elements. The storage is optional because DWARF permits
  /// unbounded arrays, but OxCaml compiler-emitted array DIEs must have a
  /// subrange count expression that reads the runtime block wosize.
  std::optional<OxCamlDynamicLayoutValue> m_count;
  /// DW_AT_byte_stride, in bytes. OxCaml compiler output emits this explicitly.
  OxCamlDynamicLayoutValue m_stride;

public:
  OxCamlArrayType(lldb::user_id_t die_id, std::optional<std::string> name,
                  Reference<OxCamlType> *element_type_ref,
                  std::optional<OxCamlDynamicLayoutValue> count,
                  OxCamlDynamicLayoutValue stride)
      : OxCamlType(Array, die_id, std::move(name)),
        m_element_type_ref(element_type_ref), m_count(std::move(count)),
        m_stride(std::move(stride)) {}

  OxCamlType *GetElementType() const;
  const std::optional<OxCamlDynamicLayoutValue> &GetCount() const {
    return m_count;
  }
  const OxCamlDynamicLayoutValue &GetStride() const { return m_stride; }
  uint64_t GetByteSize() const override {
    return formatters::oxcaml::helpers::constants::WORD_SIZE;
  }

protected:
  std::string GetDefaultDisplayName() const override;
};

struct OxCamlMember {
  std::optional<std::string> name;
  Reference<OxCamlType> *type_ref;
  /// Object-address-relative location of this member. Constants are byte
  /// offsets from the raw OCaml object pointer; exprlocs evaluate to a load
  /// address using the pre-pushed pointer convention.
  OxCamlDynamicLayoutValue location;
  /// Optional bit-field offset and size. Both are scalar quantities in bits
  /// (constants or empty-stack exprlocs).
  std::optional<OxCamlDynamicLayoutValue> bit_offset;
  std::optional<OxCamlDynamicLayoutValue> bit_size;
  bool is_artificial = false;

  bool IsBitField() const {
    return bit_offset.has_value() && bit_size.has_value();
  }
  OxCamlType *GetType() const;
};

class OxCamlVariantPart {
public:
  struct Variant {
    uint64_t discriminator_value;
    std::vector<OxCamlMember> members;
  };

private:
  OxCamlMember m_discriminator;
  std::vector<Variant> m_variants;

public:
  OxCamlVariantPart(OxCamlMember discriminator, std::vector<Variant> variants)
      : m_discriminator(std::move(discriminator)),
        m_variants(std::move(variants)) {}

  const OxCamlMember &GetDiscriminator() const { return m_discriminator; }
  const std::vector<Variant> &GetVariants() const { return m_variants; }
  bool HasArtificialDiscriminator() const {
    return m_discriminator.is_artificial;
  }
  std::optional<const Variant *> GetActiveVariant(uint64_t discr_value) const;
};

class OxCamlStructureType : public OxCamlType {
private:
  /// Dynamic size (ScalarBytes or ScalarBits, constant or expression). The
  /// unit is recorded by [m_size.GetKind()]; callers that need bytes must
  /// convert. [m_static_size_fallback] (always in bytes) is used by LLDB
  /// APIs that need a static size without an execution context.
  OxCamlDynamicLayoutValue m_size;
  uint64_t m_static_size_fallback;
  std::vector<OxCamlMember> m_members;
  std::vector<OxCamlVariantPart> m_variant_parts;

public:
  OxCamlStructureType(lldb::user_id_t die_id, std::optional<std::string> name,
                      OxCamlDynamicLayoutValue size,
                      uint64_t static_size_fallback,
                      std::vector<OxCamlMember> members,
                      std::vector<OxCamlVariantPart> variant_parts = {})
      : OxCamlType(Structure, die_id, std::move(name)),
        m_size(std::move(size)),
        m_static_size_fallback(static_size_fallback),
        m_members(std::move(members)),
        m_variant_parts(std::move(variant_parts)) {}

  uint64_t GetByteSize() const override { return m_static_size_fallback; }
  const OxCamlDynamicLayoutValue &GetDynamicSize() const { return m_size; }
  const std::vector<OxCamlMember> &GetMembers() const { return m_members; }
  const std::vector<OxCamlVariantPart> &GetVariantParts() const {
    return m_variant_parts;
  }

  bool IsOCamlVariant() const;
  bool IsTuple() const;
  std::optional<OxCamlMember> GetMemberByName(const std::string &name) const;
  std::optional<OxCamlMember> GetMemberByIndex(size_t index) const;

protected:
  std::string GetDefaultDisplayName() const override;
};

} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_TYPESYSTEM_OXCAML_OXCAMLTYPES_H
