//===-- TypeSummary.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/DataFormatters/TypeSummary.h"

#include "FormatterBytecode.h"
#include "lldb/lldb-enumerations.h"
#include "lldb/lldb-public.h"

#include "lldb/Core/Debugger.h"
#include "lldb/DataFormatters/ValueObjectPrinter.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Target/Language.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/StreamString.h"
#include "lldb/ValueObject/ValueObject.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

using namespace lldb;
using namespace lldb_private;

TypeSummaryOptions::TypeSummaryOptions() = default;

lldb::LanguageType TypeSummaryOptions::GetLanguage() const { return m_lang; }

lldb::TypeSummaryCapping TypeSummaryOptions::GetCapping() const {
  return m_capping;
}

TypeSummaryOptions &TypeSummaryOptions::SetLanguage(lldb::LanguageType lang) {
  m_lang = lang;
  return *this;
}

TypeSummaryOptions &
TypeSummaryOptions::SetCapping(lldb::TypeSummaryCapping cap) {
  m_capping = cap;
  return *this;
}

TypeSummaryImpl::TypeSummaryImpl(Kind kind, const TypeSummaryImpl::Flags &flags,
                                 uint32_t ptr_match_depth)
    : m_flags(flags), m_kind(kind), m_ptr_match_depth(ptr_match_depth) {}

std::string TypeSummaryImpl::GetSummaryKindName() {
  switch (m_kind) {
  case Kind::eSummaryString:
    return "string";
  case Kind::eCallback:
    return "callback";
  case Kind::eScript:
    return "python";
  case Kind::eInternal:
    return "c++";
  case Kind::eBytecode:
    return "bytecode";
  case Kind::eExternal:
    return "external";
  }
  llvm_unreachable("Unknown type kind name");
}

StringSummaryFormat::StringSummaryFormat(const TypeSummaryImpl::Flags &flags,
                                         const char *format_cstr,
                                         uint32_t ptr_match_depth)
    : TypeSummaryImpl(Kind::eSummaryString, flags, ptr_match_depth),
      m_format_str() {
  SetSummaryString(format_cstr);
}

void StringSummaryFormat::SetSummaryString(const char *format_cstr) {
  m_format.Clear();
  if (format_cstr && format_cstr[0]) {
    m_format_str = format_cstr;
    m_error = FormatEntity::Parse(format_cstr, m_format);
  } else {
    m_format_str.clear();
    m_error.Clear();
  }
}

bool StringSummaryFormat::FormatObject(ValueObject *valobj, std::string &retval,
                                       const TypeSummaryOptions &options) {
  if (!valobj) {
    retval.assign("NULL ValueObject");
    return false;
  }

  StreamString s;
  ExecutionContext exe_ctx(valobj->GetExecutionContextRef());
  SymbolContext sc;
  StackFrame *frame = exe_ctx.GetFramePtr();
  if (frame)
    sc = frame->GetSymbolContext(lldb::eSymbolContextEverything);

  if (IsOneLiner()) {
    // We've already checked the case of a NULL valobj above.  Let's put in an
    // assert here to make sure someone doesn't take that out:
    assert(valobj && "Must have a valid ValueObject to summarize");
    ValueObjectPrinter printer(*valobj, &s, DumpValueObjectOptions());
    printer.PrintChildrenOneLiner(HideNames(valobj));
    retval = std::string(s.GetString());
    return true;
  } else {
    if (FormatEntity::Format(m_format, s, &sc, &exe_ctx,
                             &sc.line_entry.range.GetBaseAddress(), valobj,
                             false, false)) {
      retval.assign(std::string(s.GetString()));
      return true;
    } else {
      retval.assign("error: summary string parsing error");
      return false;
    }
  }
}

std::string StringSummaryFormat::GetDescription() {
  StreamString sstr;

  sstr.Printf("`%s`%s%s%s%s%s%s%s%s%s ptr-match-depth=%u", m_format_str.c_str(),
              m_error.Fail() ? " error: " : "",
              m_error.Fail() ? m_error.AsCString() : "",
              Cascades() ? "" : " (not cascading)",
              !DoesPrintChildren(nullptr) ? "" : " (show children)",
              !DoesPrintValue(nullptr) ? " (hide value)" : "",
              IsOneLiner() ? " (one-line printout)" : "",
              SkipsPointers() ? " (skip pointers)" : "",
              SkipsReferences() ? " (skip references)" : "",
              HideNames(nullptr) ? " (hide member names)" : "",
              GetPtrMatchDepth());
  return std::string(sstr.GetString());
}

std::string StringSummaryFormat::GetName() { return m_format_str; }

CXXFunctionSummaryFormat::CXXFunctionSummaryFormat(
    const TypeSummaryImpl::Flags &flags, Callback impl, const char *description,
    uint32_t ptr_match_depth)
    : TypeSummaryImpl(Kind::eCallback, flags, ptr_match_depth), m_impl(impl),
      m_description(description ? description : "") {}

bool CXXFunctionSummaryFormat::FormatObject(ValueObject *valobj,
                                            std::string &dest,
                                            const TypeSummaryOptions &options) {
  dest.clear();
  StreamString stream;
  if (!m_impl || !m_impl(*valobj, stream, options))
    return false;
  dest = std::string(stream.GetString());
  return true;
}

std::string CXXFunctionSummaryFormat::GetDescription() {
  StreamString sstr;
  sstr.Printf("%s%s%s%s%s%s%s ptr-match-depth=%u %s",
              Cascades() ? "" : " (not cascading)",
              !DoesPrintChildren(nullptr) ? "" : " (show children)",
              !DoesPrintValue(nullptr) ? " (hide value)" : "",
              IsOneLiner() ? " (one-line printout)" : "",
              SkipsPointers() ? " (skip pointers)" : "",
              SkipsReferences() ? " (skip references)" : "",
              HideNames(nullptr) ? " (hide member names)" : "",
              GetPtrMatchDepth(), m_description.c_str());
  return std::string(sstr.GetString());
}

std::string CXXFunctionSummaryFormat::GetName() { return m_description; }

ScriptSummaryFormat::ScriptSummaryFormat(const TypeSummaryImpl::Flags &flags,
                                         const char *function_name,
                                         const char *python_script,
                                         uint32_t ptr_match_depth)
    : TypeSummaryImpl(Kind::eScript, flags, ptr_match_depth), m_function_name(),
      m_python_script(), m_script_function_sp() {
  // Take preference in the python script name over the function name.
  if (function_name) {
    m_function_name.assign(function_name);
    m_script_formatter_name = function_name;
  }
  if (python_script) {
    m_python_script.assign(python_script);
    m_script_formatter_name = python_script;
  }

  // Python scripts include the tabbing of the function def so we remove the
  // leading spaces.
  m_script_formatter_name = m_script_formatter_name.erase(
      0, m_script_formatter_name.find_first_not_of(' '));
}

bool ScriptSummaryFormat::FormatObject(ValueObject *valobj, std::string &retval,
                                       const TypeSummaryOptions &options) {
  if (!valobj)
    return false;

  TargetSP target_sp(valobj->GetTargetSP());

  if (!target_sp) {
    retval.assign("error: no target");
    return false;
  }

  ScriptInterpreter *script_interpreter =
      target_sp->GetDebugger().GetScriptInterpreter();

  if (!script_interpreter) {
    retval.assign("error: no ScriptInterpreter");
    return false;
  }

  return script_interpreter->GetScriptedSummary(
      m_function_name.c_str(), valobj->GetSP(), m_script_function_sp, options,
      retval);
}

std::string ScriptSummaryFormat::GetDescription() {
  StreamString sstr;
  sstr.Printf("%s%s%s%s%s%s%s ptr-match-depth=%u\n  ",
              Cascades() ? "" : " (not cascading)",
              !DoesPrintChildren(nullptr) ? "" : " (show children)",
              !DoesPrintValue(nullptr) ? " (hide value)" : "",
              IsOneLiner() ? " (one-line printout)" : "",
              SkipsPointers() ? " (skip pointers)" : "",
              SkipsReferences() ? " (skip references)" : "",
              HideNames(nullptr) ? " (hide member names)" : "",
              GetPtrMatchDepth());
  if (m_python_script.empty()) {
    if (m_function_name.empty()) {
      sstr.PutCString("no backing script");
    } else {
      sstr.PutCString(m_function_name);
    }
  } else {
    sstr.PutCString(m_python_script);
  }
  return std::string(sstr.GetString());
}

std::string ScriptSummaryFormat::GetName() { return m_script_formatter_name; }

ExternalSummaryFormat::ExternalSummaryFormat(
    const TypeSummaryImpl::Flags &flags, llvm::StringRef executable_path)
    : TypeSummaryImpl(Kind::eExternal, flags),
      m_executable_path(executable_path.str()) {}

llvm::Expected<std::string>
ExternalSummaryFormat::RunSummaryExecutable(llvm::StringRef executable_path,
                                            llvm::StringRef type_name,
                                            llvm::ArrayRef<uint8_t> input) {
  // The input reaches the program through a temporary file redirected to its
  // standard input; from the program's point of view this is the same as a
  // pipe, but avoids any risk of deadlock on large inputs.
  llvm::SmallString<128> input_path;
  int input_fd;
  if (std::error_code ec = llvm::sys::fs::createTemporaryFile(
          "lldb-external-summary-in", "bin", input_fd, input_path))
    return llvm::createStringError(
        ec, "cannot create temporary file for external summary input");
  llvm::FileRemover input_remover(input_path);
  {
    llvm::raw_fd_ostream input_stream(input_fd, /*shouldClose=*/true);
    input_stream.write(reinterpret_cast<const char *>(input.data()),
                       input.size());
    input_stream.flush();
    if (input_stream.has_error())
      return llvm::createStringError(
          input_stream.error(),
          "cannot write temporary file for external summary input");
  }

  llvm::SmallString<128> output_path;
  if (std::error_code ec = llvm::sys::fs::createTemporaryFile(
          "lldb-external-summary-out", "txt", output_path))
    return llvm::createStringError(
        ec, "cannot create temporary file for external summary output");
  llvm::FileRemover output_remover(output_path);

  // An empty redirect sends the program's stderr to /dev/null so that its
  // diagnostics cannot corrupt LLDB's terminal.
  std::optional<llvm::StringRef> redirects[3] = {
      input_path.str(), output_path.str(), llvm::StringRef("")};

  // Guard against a wedged program: a summary is produced interactively, so
  // a few seconds is already generous.
  constexpr unsigned timeout_in_seconds = 10;

  std::string error_msg;
  bool execution_failed = false;
  int exit_code = llvm::sys::ExecuteAndWait(
      executable_path, {executable_path, type_name}, /*Env=*/std::nullopt,
      redirects, timeout_in_seconds, /*MemoryLimit=*/0, &error_msg,
      &execution_failed);
  if (execution_failed)
    return llvm::createStringError("cannot run '%s': %s",
                                   executable_path.str().c_str(),
                                   error_msg.c_str());
  if (exit_code != 0)
    return llvm::createStringError(
        "'%s' did not recognize type '%s' (exit code %d)",
        executable_path.str().c_str(), type_name.str().c_str(), exit_code);

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> output_buffer =
      llvm::MemoryBuffer::getFile(output_path);
  if (!output_buffer)
    return llvm::createStringError(
        output_buffer.getError(), "cannot read external summary output file");

  llvm::StringRef output = (*output_buffer)->getBuffer().rtrim("\r\n");
  return output.str();
}

bool ExternalSummaryFormat::FormatObject(ValueObject *valobj,
                                         std::string &dest,
                                         const TypeSummaryOptions &options) {
  dest.clear();
  if (!valobj || m_executable_path.empty())
    return false;

  Log *log = GetLog(LLDBLog::DataFormatters);

  // Ask the value's language plugin for the serialized form to send to the
  // program (and the type name to pass it); without a language plugin, send
  // the value's raw bytes.
  Language *language = Language::FindPlugin(options.GetLanguage());
  if (!language)
    language = Language::FindPlugin(valobj->GetObjectRuntimeLanguage());

  std::vector<uint8_t> data;
  llvm::Expected<std::string> type_name =
      language ? language->GetExternalFormatterInput(*valobj, data)
               : Language::GetDefaultExternalFormatterInput(*valobj, data);
  if (!type_name) {
    LLDB_LOG_ERROR(log, type_name.takeError(),
                   "external summary: cannot serialize value: {0}");
    return false;
  }

  llvm::Expected<std::string> output =
      RunSummaryExecutable(m_executable_path, *type_name, data);
  if (!output) {
    LLDB_LOG_ERROR(log, output.takeError(), "external summary: {0}");
    return false;
  }

  dest = std::move(*output);
  return true;
}

std::string ExternalSummaryFormat::GetDescription() {
  StreamString sstr;
  sstr.Printf("`%s`%s%s%s%s%s%s ptr-match-depth=%u",
              m_executable_path.c_str(), Cascades() ? "" : " (not cascading)",
              !DoesPrintChildren(nullptr) ? "" : " (show children)",
              !DoesPrintValue(nullptr) ? " (hide value)" : "",
              SkipsPointers() ? " (skip pointers)" : "",
              SkipsReferences() ? " (skip references)" : "",
              HideNames(nullptr) ? " (hide member names)" : "",
              GetPtrMatchDepth());
  return std::string(sstr.GetString());
}

std::string ExternalSummaryFormat::GetName() { return m_executable_path; }

BytecodeSummaryFormat::BytecodeSummaryFormat(
    const TypeSummaryImpl::Flags &flags,
    std::unique_ptr<llvm::MemoryBuffer> bytecode)
    : TypeSummaryImpl(Kind::eBytecode, flags), m_bytecode(std::move(bytecode)) {
}

bool BytecodeSummaryFormat::FormatObject(ValueObject *valobj,
                                         std::string &retval,
                                         const TypeSummaryOptions &options) {
  if (!valobj)
    return false;

  TargetSP target_sp(valobj->GetTargetSP());

  if (!target_sp) {
    retval.assign("error: no target");
    return false;
  }

  std::vector<FormatterBytecode::ControlStackElement> control(
      {m_bytecode->getBuffer()});
  FormatterBytecode::DataStack data({valobj->GetSP()});
  llvm::Error error = FormatterBytecode::Interpret(
      control, data, FormatterBytecode::sel_summary);
  if (error) {
    retval = llvm::toString(std::move(error));
    return false;
  }
  if (!data.size()) {
    retval = "empty stack";
    return false;
  }
  auto &top = data.back();
  retval = "";
  llvm::raw_string_ostream os(retval);
  if (auto s = std::get_if<std::string>(&top))
    os << *s;
  else if (auto u = std::get_if<uint64_t>(&top))
    os << *u;
  else if (auto i = std::get_if<int64_t>(&top))
    os << *i;
  else if (auto valobj = std::get_if<ValueObjectSP>(&top)) {
    if (!valobj->get())
      os << "empty object";
    else
      os << valobj->get()->GetValueAsCString();
  } else if (auto type = std::get_if<CompilerType>(&top)) {
    os << type->TypeDescription();
  } else if (auto sel = std::get_if<FormatterBytecode::Selectors>(&top)) {
    os << toString(*sel);
  }
  return true;
}

std::string BytecodeSummaryFormat::GetDescription() {
  StreamString sstr;
  sstr.Printf("%s%s%s%s%s%s%s\n  ", Cascades() ? "" : " (not cascading)",
              !DoesPrintChildren(nullptr) ? "" : " (show children)",
              !DoesPrintValue(nullptr) ? " (hide value)" : "",
              IsOneLiner() ? " (one-line printout)" : "",
              SkipsPointers() ? " (skip pointers)" : "",
              SkipsReferences() ? " (skip references)" : "",
              HideNames(nullptr) ? " (hide member names)" : "");
  // FIXME: sstr.PutCString(disassembly);
  return std::string(sstr.GetString());
}

std::string BytecodeSummaryFormat::GetName() {
  return "LLDB bytecode formatter";
}
