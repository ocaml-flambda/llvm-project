//===-- OxCamlTypes.cpp ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OxCamlTypes.h"
#include "../../Language/OxCaml/LogChannelOxCaml.h"

using namespace lldb;
using namespace lldb_private;

// TODO: Move more method implementations from the header file to this implementation file
// to reduce header dependencies and compilation times.

// OxCamlPlaceholderType implementation
uint64_t OxCamlPlaceholderType::GetByteSize() const {
  Log *log = GetLog(OxCamlLog::TypeRegistry);
  LLDB_LOG(log, "OxCamlPlaceholderType::GetByteSize() called for DIE 0x{0:x16}, returning {1}",
           GetDieId(), m_byte_size);
  return m_byte_size;
}

std::string OxCamlPlaceholderType::GetDefaultDisplayName() const {
  Log *log = GetLog(OxCamlLog::TypeRegistry);
  LLDB_LOG(log, "OxCamlPlaceholderType::GetDefaultDisplayName() called for DIE 0x{0:x16}", GetDieId());
  return "<placeholder>";
}

// OxCamlUnknownType implementation
OxCamlUnknownType::OxCamlUnknownType(lldb::user_id_t die_id,
                                     std::optional<std::string> name,
                                     uint64_t byte_size,
                                     uint32_t dwarf_tag)
  : OxCamlType(Unknown, die_id, std::move(name)),
    m_byte_size(byte_size),
    m_dwarf_tag(dwarf_tag) {}

uint64_t OxCamlUnknownType::GetByteSize() const {
  Log *log = GetLog(OxCamlLog::TypeRegistry);
  LLDB_LOG(log, "OxCamlUnknownType::GetByteSize() called for DIE 0x{0:x16}, DWARF tag 0x{1:x}, returning {2}", 
           GetDieId(), m_dwarf_tag, m_byte_size);
  return m_byte_size;
}

uint32_t OxCamlUnknownType::GetDwarfTag() const {
  return m_dwarf_tag;
}

std::string OxCamlUnknownType::GetDefaultDisplayName() const {
  Log *log = GetLog(OxCamlLog::TypeRegistry);
  LLDB_LOG(log, "OxCamlUnknownType::GetDefaultDisplayName() called for DIE 0x{0:x16}, DWARF tag 0x{1:x}", 
           GetDieId(), m_dwarf_tag);
  return llvm::formatv("<unknown DWARF tag 0x{0:x}>", m_dwarf_tag);
}
