//===-- OxCamlTypes.cpp -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OxCamlTypes.h"
#include "../../Language/OxCaml/LogChannelOxCaml.h"
#include "../../Language/OxCaml/OxCamlAssert.h"
#include "llvm/Support/FormatVariadic.h"

using namespace lldb;
using namespace lldb_private;

// =============================================================================
// OxCamlValueType implementations
// =============================================================================

std::string OxCamlValueType::GetDefaultDisplayName() const {
  return "ocaml_value";
}

// =============================================================================
// OxCamlUnboxedBaseType implementations
// =============================================================================

std::string OxCamlUnboxedBaseType::GetDefaultDisplayName() const {
  uint64_t bits = m_byte_size * 8;
  switch (m_base_kind) {
  case Signed:
    return "int" + std::to_string(bits) + "#";
  case Unsigned:
    return "uint" + std::to_string(bits) + "#";
  case Float:
    return "float" + std::to_string(bits) + "#";
  }
}

// =============================================================================
// OxCamlPlaceholderType implementations
// =============================================================================

uint64_t OxCamlPlaceholderType::GetByteSize() const {
  Log *log = GetLog(OxCamlLog::TypeRegistry);
  LLDB_LOG(log,
           "OxCamlPlaceholderType::GetByteSize() called for DIE 0x{0:x16}, "
           "returning {1}",
           GetDieId(), m_byte_size);
  return m_byte_size;
}

std::string OxCamlPlaceholderType::GetDefaultDisplayName() const {
  Log *log = GetLog(OxCamlLog::TypeRegistry);
  LLDB_LOG(
      log,
      "OxCamlPlaceholderType::GetDefaultDisplayName() called for DIE 0x{0:x16}",
      GetDieId());
  return "<placeholder>";
}

// =============================================================================
// OxCamlUnknownType implementations
// =============================================================================

OxCamlUnknownType::OxCamlUnknownType(lldb::user_id_t die_id,
                                     std::optional<std::string> name,
                                     uint64_t byte_size, uint32_t dwarf_tag)
    : OxCamlType(Unknown, die_id, std::move(name)), m_byte_size(byte_size),
      m_dwarf_tag(dwarf_tag) {}

uint64_t OxCamlUnknownType::GetByteSize() const {
  Log *log = GetLog(OxCamlLog::TypeRegistry);
  LLDB_LOG(log,
           "OxCamlUnknownType::GetByteSize() called for DIE 0x{0:x16}, DWARF "
           "tag 0x{1:x}, returning {2}",
           GetDieId(), m_dwarf_tag, m_byte_size);
  return m_byte_size;
}

uint32_t OxCamlUnknownType::GetDwarfTag() const { return m_dwarf_tag; }

std::string OxCamlUnknownType::GetDefaultDisplayName() const {
  Log *log = GetLog(OxCamlLog::TypeRegistry);
  LLDB_LOG(log,
           "OxCamlUnknownType::GetDefaultDisplayName() called for DIE "
           "0x{0:x16}, DWARF tag 0x{1:x}",
           GetDieId(), m_dwarf_tag);
  return llvm::formatv("<unknown DWARF tag 0x{0:x}>", m_dwarf_tag);
}

// =============================================================================
// OxCamlTypedefType implementations
// =============================================================================

OxCamlType *OxCamlTypedefType::GetUnderlyingType() const {
  return m_underlying_ref->get();
}

uint64_t OxCamlTypedefType::GetByteSize() const {
  return GetUnderlyingType()->GetByteSize();
}

std::string OxCamlTypedefType::GetDefaultDisplayName() const {
  return GetUnderlyingType()->GetDisplayName();
}

// =============================================================================
// OxCamlEnumType implementations
// =============================================================================

std::optional<std::string>
OxCamlEnumType::GetEnumeratorName(int64_t value) const {
  for (const auto &e : m_enumerators) {
    if (e.value == value)
      return e.name;
  }
  return std::nullopt;
}

std::optional<int64_t>
OxCamlEnumType::GetEnumeratorValue(const std::string &name) const {
  for (const auto &e : m_enumerators) {
    if (e.name == name)
      return e.value;
  }
  return std::nullopt;
}

std::string OxCamlEnumType::GetDefaultDisplayName() const { return "enum"; }

// =============================================================================
// OxCamlPointerType implementations
// =============================================================================

OxCamlType *OxCamlPointerType::GetPointedToType() const {
  return m_pointed_to_ref->get();
}

std::string OxCamlPointerType::GetDefaultDisplayName() const {
  return GetPointedToType()->GetDisplayName() + " ptr";
}

// =============================================================================
// OxCamlArrayType implementations
// =============================================================================

OxCamlType *OxCamlArrayType::GetElementType() const {
  return m_element_type_ref->get();
}

std::string OxCamlArrayType::GetDefaultDisplayName() const {
  return GetElementType()->GetDisplayName() + " array";
}

// =============================================================================
// OxCamlMember implementations
// =============================================================================

OxCamlType *OxCamlMember::GetType() const {
  OX_ASSERT(type_ref, "Member type reference is null: {0}", (void *)this);
  return type_ref->get();
}

// =============================================================================
// OxCamlVariantPart implementations
// =============================================================================

std::optional<const OxCamlVariantPart::Variant *>
OxCamlVariantPart::GetActiveVariant(uint64_t discr_value) const {
  for (const auto &variant : m_variants) {
    if (variant.discriminator_value == discr_value)
      return &variant;
  }
  return std::nullopt;
}

// =============================================================================
// OxCamlStructureType implementations
// =============================================================================

bool OxCamlStructureType::IsOCamlVariant() const {
  return m_variant_parts.size() == 1 && m_members.empty();
}

bool OxCamlStructureType::IsTuple() const {
  if (m_members.empty() || !m_variant_parts.empty())
    return false;

  for (const auto &member : m_members) {
    if (member.name.has_value())
      return false;
  }
  return true;
}

std::optional<OxCamlMember>
OxCamlStructureType::GetMemberByName(const std::string &name) const {
  for (const auto &m : m_members) {
    if (m.name && m.name.value() == name)
      return m;
  }
  return std::nullopt;
}

std::optional<OxCamlMember>
OxCamlStructureType::GetMemberByIndex(size_t index) const {
  if (index < m_members.size())
    return m_members[index];
  return std::nullopt;
}

std::string OxCamlStructureType::GetDefaultDisplayName() const {
  if (IsOCamlVariant())
    return "variant";
  return IsTuple() ? "tuple" : "record";
}
