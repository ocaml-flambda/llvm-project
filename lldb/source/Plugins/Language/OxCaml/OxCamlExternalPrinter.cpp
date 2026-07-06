//===-- OxCamlExternalPrinter.cpp -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OxCamlExternalPrinter.h"
#include "LogChannelOxCaml.h"
#include "OxCamlLanguage.h"
#include "OxCamlMarshal.h"
#include "Plugins/TypeSystem/OxCaml/TypeSystemOxCaml.h"
#include "lldb/Core/Value.h"
#include "lldb/DataFormatters/TypeSummary.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Target/Process.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/FileSpec.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Reference.h"
#include "lldb/Utility/Status.h"
#include "lldb/Utility/Stream.h"
#include "lldb/ValueObject/ValueObject.h"
#include "llvm/ADT/StringRef.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters::oxcaml;

static OxCamlType *ResolveTypedefs(OxCamlType *type) {
  while (type && type->GetKind() == OxCamlType::Typedef)
    type = static_cast<OxCamlTypedefType *>(type)->GetUnderlyingType();
  return type;
}

llvm::Expected<std::string>
lldb_private::formatters::oxcaml::GetOCamlExternalFormatterInput(
    ValueObject &valobj, std::vector<uint8_t> &data) {
  CompilerType compiler_type = valobj.GetCompilerType();
  auto *type_ref =
      static_cast<Reference<OxCamlType> *>(compiler_type.GetOpaqueQualType());
  if (!type_ref || !type_ref->get())
    return llvm::createStringError("value has no OxCaml type information");

  // Only values using the tagged ocaml_value representation can be
  // marshalled; unboxed primitives (float#, int64#, ...) are raw bits whose
  // low bit means nothing, and unresolved types tell us nothing at all.
  OxCamlType *type = ResolveTypedefs(type_ref->get());
  if (!type)
    return llvm::createStringError("value has an unresolved typedef chain");
  switch (type->GetKind()) {
  case OxCamlType::UnboxedBase:
  case OxCamlType::Placeholder:
  case OxCamlType::Unknown:
    return llvm::createStringError(
        "type '%s' is not represented as a tagged OCaml value",
        type->GetDisplayName().c_str());
  default:
    break;
  }

  // Values described by DWARF implicit pointers (in whole or in part) have
  // no bytes of their own in the debuggee, so the marshaller cannot walk
  // them yet; a MarshalMemoryReader materializing them from the DWARF
  // description would lift this restriction.
  if (valobj.GetValue().GetValueType() == Value::ValueType::ImplicitPointer ||
      !valobj.GetValue().GetImplicitPointerPieces().empty())
    return llvm::createStringError(
        "value is described by an implicit pointer");

  lldb::ProcessSP process_sp = valobj.GetProcessSP();
  if (!process_sp)
    return llvm::createStringError("value has no process");

  DataExtractor extractor;
  Status status;
  valobj.GetData(extractor, status);
  if (status.Fail())
    return llvm::createStringError("cannot get value data: %s",
                                   status.AsCString());
  if (extractor.GetByteSize() < sizeof(uint64_t))
    return llvm::createStringError(
        "value has %zu byte(s) of data, expected at least 8",
        static_cast<size_t>(extractor.GetByteSize()));

  lldb::offset_t offset = 0;
  const uint64_t root = extractor.GetU64(&offset);

  ProcessMemoryReader reader(process_sp);
  llvm::Expected<std::vector<uint8_t>> marshalled =
      MarshalOCamlValue(root, reader);
  if (!marshalled)
    return marshalled.takeError();
  data = std::move(*marshalled);

  // "Env.t @ value" -> "Env.t": the layout annotation is an artifact of the
  // DWARF encoding, not part of the OCaml type name the printer knows.
  llvm::StringRef name = valobj.GetDisplayTypeName().GetStringRef();
  size_t layout_pos = name.rfind(" @ ");
  if (layout_pos != llvm::StringRef::npos)
    name = name.take_front(layout_pos);
  return name.str();
}

bool lldb_private::formatters::oxcaml::TryExternalPrettyPrinter(
    ValueObject &valobj, Stream &stream) {
  FileSpec executable = OxCamlLanguage::GetExternalSummaryExecutable();
  if (!executable)
    return false;

  Log *log = GetLog(OxCamlLog::Formatting);

  std::vector<uint8_t> data;
  llvm::Expected<std::string> type_name =
      GetOCamlExternalFormatterInput(valobj, data);
  if (!type_name) {
    LLDB_LOG_ERROR(log, type_name.takeError(),
                   "external printer: cannot marshal value: {0}");
    return false;
  }

  LLDB_LOG(log, "external printer: running '{0}' for type '{1}' with {2} "
                "marshalled byte(s)",
           executable.GetPath(), *type_name, data.size());

  llvm::Expected<std::string> output =
      ExternalSummaryFormat::RunSummaryExecutable(executable.GetPath(),
                                                  *type_name, data);
  if (!output) {
    LLDB_LOG_ERROR(log, output.takeError(), "external printer: {0}");
    return false;
  }

  stream.PutCString(*output);
  return true;
}
