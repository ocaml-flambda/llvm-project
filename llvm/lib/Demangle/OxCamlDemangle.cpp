//===--- OxCamlDemangle.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines a demangler for the new mangling scheme devised for OxCaml
//
//===----------------------------------------------------------------------===//

#include <cassert>
#include <string_view>

#include "llvm/Demangle/StringViewExtras.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Demangle/Utility.h"

using llvm::itanium_demangle::starts_with;
using llvm::itanium_demangle::OutputBuffer;

#define ERROR (~((unsigned)0))

static unsigned ConsumeUnsignedDecimal(std::string_view& sv) {
  unsigned res = 0, i = 0;
  while(sv[i] >= '0' && sv[i] <= '9') {
    res = res * 10 + (sv[i] - '0');
    i++;
  }
  sv.remove_prefix(i);
  if(i == 0)
    return ERROR;
  return res;
}

static unsigned ConsumeUnsigned26(std::string_view& sv) {
  unsigned res = 0, i = 0;
  while(sv[i] >= 'A' && sv[i] <= 'Z') {
    res = res * 26 + (sv[i] - 'A');
    i++;
  }
  sv.remove_prefix(i);
  if(i == 0)
    return ERROR;
  return res;
}

static bool islowerhex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static unsigned lowerhex(char c) {
  if(c >= '0' && c <= '9')
    return c - '0';
  else {
    assert(c >= 'a' && c <= 'f');
    return c - 'a' + 10;
  }
}

// Decode escaped identifier (format: u<len><coded>_<raw>)
// Returns true on success, false on error
static bool DecodeEscaped(std::string_view& Mangled, OutputBuffer& Demangled) {
  unsigned len = ConsumeUnsignedDecimal(Mangled);
  if(len == ERROR || len <= 0 || len > Mangled.size())
    return false;

  size_t split = Mangled.find('_');
  if(split >= len)
    return false;

  std::string_view coded = Mangled.substr(0, split);
  std::string_view raw = Mangled.substr(split+1, len-split-1);

  while(!coded.empty()) {
    unsigned chunklen = ConsumeUnsigned26(coded);
    if(chunklen == ERROR || chunklen > raw.size())
      return false;
    Demangled << raw.substr(0, chunklen);
    raw.remove_prefix(chunklen);

    unsigned i;
    for(i = 0; i+1 < coded.size() && islowerhex(coded[i]); i+=2) {
      if(!islowerhex(coded[i+1]))
        return false;
      char c = (char)(lowerhex(coded[i]) << 4 | lowerhex(coded[i+1]));
      Demangled << c;
    }
    coded.remove_prefix(i);
  }

  if(!raw.empty())
    Demangled << raw;

  Mangled.remove_prefix(len);
  return true;
}

// Decode identifier (either plain or escaped)
// Handles: <len><text> or u<len><coded>_<raw>
// Returns true on success, false on error
static bool DecodeIdentifier(std::string_view& Mangled, OutputBuffer& Demangled) {
  if(starts_with(Mangled, 'u')) {
    // Escaped identifier
    Mangled.remove_prefix(1);
    return DecodeEscaped(Mangled, Demangled);
  } else {
    // Plain identifier with length prefix
    unsigned len = ConsumeUnsignedDecimal(Mangled);
    if(len == ERROR || len <= 0 || len > Mangled.size())
      return false;
    Demangled << Mangled.substr(0, len);
    Mangled.remove_prefix(len);
    return true;
  }
}

// Decode anonymous location (format: filename_line_col)
// Anonymous functions, modules and partial functions are encoded as:
//   type(filename:line:col)
// where <type> can be fn, mod or partial
// Returns true on success, false on error
static bool DecodeAnonymousLocation(std::string_view& Mangled, OutputBuffer& Demangled, char typ) {
  // Allocate temporary buffer based on remaining mangled string size
  // The decoded identifier will be at most the size of the remaining mangled string
  size_t buffer_size = Mangled.size();
  if(buffer_size == 0)
    return false;

  char *temp_buf = static_cast<char *>(std::malloc(buffer_size));
  if(temp_buf == nullptr)
    std::terminate();

  OutputBuffer TempDemangled(temp_buf, buffer_size);

  if(!DecodeIdentifier(Mangled, TempDemangled)) {
    std::free(TempDemangled.getBuffer());
    return false;
  }

  size_t temp_len = TempDemangled.getCurrentPosition();
  char *buf = TempDemangled.getBuffer();

  // Parse filename_line_col format by finding the last two underscores
  size_t first_underscore = 0, second_underscore = 0;
  int underscore_count = 0;

  for(size_t j = temp_len; j > 0; j--) {
    if(buf[j-1] == '_') {
      underscore_count++;
      if(underscore_count == 1)
        second_underscore = j - 1;
      else if(underscore_count == 2) {
        first_underscore = j - 1;
        break;
      }
    }
  }

  // Output in format type(filename:line:col)
  if(underscore_count >= 2) {
    switch(typ) {
      case 'S':
        Demangled << "mod";
        break;
      case 'L':
        Demangled << "fn";
        break;
      case 'P':
        Demangled << "partial";
        break;
      default:
        assert(0);
    }
    Demangled << '(';
    for(size_t j = 0; j < first_underscore; j++)
      Demangled << buf[j];
    Demangled << ':';
    for(size_t j = first_underscore + 1; j < second_underscore; j++)
      Demangled << buf[j];
    Demangled << ':';
    for(size_t j = second_underscore + 1; j < temp_len; j++)
      Demangled << buf[j];
    Demangled << ')';
  } else {
    // Fallback: just output the identifier as-is
    for(size_t j = 0; j < temp_len; j++)
      Demangled << buf[j];
  }

  std::free(buf);
  return true;
}

// Demangle a symbol in the structured scheme (prefix "_Caml").
static char *demangleStructured(std::string_view Mangled) {
  if(!starts_with(Mangled, "_Caml"))
    return nullptr;
  Mangled.remove_prefix(5);

  // Allocate the buffer at a reasonable size, as OutputBuffer allocates 992
  // bytes when starting from an empty buffer
  char *DemangledBuffer;
  DemangledBuffer = static_cast<char *>(std::malloc(Mangled.size()));
  if (DemangledBuffer == nullptr)
    std::terminate();
  OutputBuffer Demangled(DemangledBuffer, Mangled.size());

#define ENDONERROR() do {           \
  std::free(Demangled.getBuffer()); \
  return nullptr;                   \
} while(0)

  // Parse path items
  while(!Mangled.empty()) {
      // Check for terminating underscore
      if(Mangled[0] == '_') {
          // End of symbol path, rest is unique id
          break;
      }

      // Handle each path_item type
      switch(Mangled[0]) {
          case 'U':  // Compilation Unit
          case 'M':  // Module
          case 'O':  // class (O for object)
          case 'F':  // Function
              if(!Demangled.empty())
                  Demangled << '.';
              Mangled.remove_prefix(1);
              if(!DecodeIdentifier(Mangled, Demangled))
                  ENDONERROR();
              break;

          case 'S':  // anonymous Struct
          case 'L':  // anonymous function (L for lambda)
          case 'P': { // Partial application
              if(!Demangled.empty())
                  Demangled << '.';
              char typ = Mangled[0];
              Mangled.remove_prefix(1);
              if(!DecodeAnonymousLocation(Mangled, Demangled, typ))
                  ENDONERROR();
              break;
          }
          case 'I':  // Inline marker (specialization)
              if(!Demangled.empty())
                  Demangled << '.';
              Mangled.remove_prefix(1);
              // The body was specialized (copied) into the current compilation
              // unit, not inlined at a particular call site, so render
              // <specialization_of> to match the ocamlfilt reference demangler.
              Demangled << "<specialization_of>";
              break;

          default:
              ENDONERROR();
      }
  }

  // A valid symbol must contain at least one path item; reject names like
  // "_Caml" or "_Caml_123" that decoded to nothing.
  if(Demangled.getCurrentPosition() == 0)
      ENDONERROR();

  // Append the trailing suffix (compiler-appended unique ids such as "_5_code")
  // verbatim. They keep otherwise-identical symbols distinct, so dropping them
  // would risk name clashes.
  Demangled << Mangled;

  Demangled << '\0';

  return Demangled.getBuffer();
}

// ===========================================================================
// Flat scheme (legacy "caml"/"_caml" prefixes)
//
// Ports the flat0 (OCaml <= 5.2) and flat1 (OCaml >= 5.3) demanglers from the
// ocamlfilt reference tool. flat0 uses "__" as the module separator and "$xx"
// hex escapes. flat1 additionally supports the macOS assembler flavour, which
// uses "$" as the separator and "$$xx"/"$$$xx" escapes; the flavour is
// auto-detected per symbol. As with the structured scheme, the trailing
// compiler suffix (e.g. "_42") is preserved verbatim.
// ===========================================================================

static bool IsUpperAZ(char c) { return c >= 'A' && c <= 'Z'; }

static bool IsFlatXDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

static unsigned FlatHex(char c) {
  if(c >= '0' && c <= '9')
    return c - '0';
  if(c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return c - 'A' + 10;
}

// If Mangled starts with a recognised flat prefix ("caml", or the
// underscore-prefixed "_caml" that the macOS assembler emits) followed by an
// uppercase letter (a syntactically valid OCaml module name), return the
// matched prefix length; otherwise return 0.
static size_t MatchedFlatPrefixLen(std::string_view Mangled) {
  if(Mangled.size() > 5 && starts_with(Mangled, "_caml") && IsUpperAZ(Mangled[5]))
    return 5;
  if(Mangled.size() > 4 && starts_with(Mangled, "caml") && IsUpperAZ(Mangled[4]))
    return 4;
  return 0;
}

enum class FlatStyle { Linux, Macos };

// flat1 emits one of two flavours per binary. A bare '.' or "__" is a positive
// Linux marker; "$$" or a '$' not followed by two hex digits is a positive
// macOS marker. A '$' followed by two hex digits is ambiguous in isolation, so
// if no Linux marker appears anywhere the only coherent reading is macOS.
static FlatStyle DetectFlatStyle(std::string_view S, size_t PrefixLen) {
  size_t len = S.size();
  bool saw_dollar_hex_pair = false;
  for(size_t i = PrefixLen; i < len;) {
    if(S[i] == '.')
      return FlatStyle::Linux;
    if(S[i] == '_' && i + 1 < len && S[i + 1] == '_')
      return FlatStyle::Linux;
    if(S[i] == '$') {
      if(i + 1 < len && S[i + 1] == '$')
        return FlatStyle::Macos;
      if(i + 2 < len && IsFlatXDigit(S[i + 1]) && IsFlatXDigit(S[i + 2])) {
        saw_dollar_hex_pair = true;
        i += 3;
        continue;
      }
      return FlatStyle::Macos;
    }
    i++;
  }
  return saw_dollar_hex_pair ? FlatStyle::Macos : FlatStyle::Linux;
}

// flat0 (OCaml <= 5.2): "__" -> '.', "$xx" -> hex char, else literal. Returns a
// malloc'd C string.
static char *demangleFlat0(std::string_view S, size_t PrefixLen) {
  size_t len = S.size();
  char *Out = static_cast<char *>(std::malloc(len + 1));
  if(Out == nullptr)
    std::terminate();

  size_t j = 0;
  for(size_t i = PrefixLen; i < len;) {
    if(S[i] == '_') {
      if(i + 1 >= len)
        break; // dangling '_' -> stop
      if(S[i + 1] == '_') {
        Out[j++] = '.';
        i += 2;
        continue;
      }
      Out[j++] = '_';
      i++;
      continue;
    }
    if(S[i] == '$') {
      if(i + 2 < len && IsFlatXDigit(S[i + 1]) && IsFlatXDigit(S[i + 2])) {
        Out[j++] = (char)((FlatHex(S[i + 1]) << 4) | FlatHex(S[i + 2]));
        i += 3;
        continue;
      }
      Out[j++] = '$'; // not a valid escape -> literal '$'
      i++;
      continue;
    }
    Out[j++] = S[i++];
  }
  Out[j] = '\0';
  return Out;
}

// flat1 (OCaml >= 5.3): Linux flavour matches flat0; the macOS flavour uses '$'
// as the separator with "$$xx" / "$$$xx" escapes. Returns a malloc'd C string.
static char *demangleFlat1(std::string_view S, size_t PrefixLen) {
  size_t len = S.size();
  FlatStyle style = DetectFlatStyle(S, PrefixLen);
  char *Out = static_cast<char *>(std::malloc(len + 1));
  if(Out == nullptr)
    std::terminate();

  size_t j = 0;
  for(size_t i = PrefixLen; i < len;) {
    if(style == FlatStyle::Macos) {
      if(S[i] != '$') {
        Out[j++] = S[i++];
        continue;
      }
      if(i + 4 < len && S[i + 1] == '$' && S[i + 2] == '$' &&
         IsFlatXDigit(S[i + 3]) && IsFlatXDigit(S[i + 4])) {
        // "$$$xx" -> separator + hex char
        Out[j++] = '.';
        Out[j++] = (char)((FlatHex(S[i + 3]) << 4) | FlatHex(S[i + 4]));
        i += 5;
        continue;
      }
      if(i + 3 < len && S[i + 1] == '$' && IsFlatXDigit(S[i + 2]) &&
         IsFlatXDigit(S[i + 3])) {
        // "$$xx" -> hex char
        Out[j++] = (char)((FlatHex(S[i + 2]) << 4) | FlatHex(S[i + 3]));
        i += 4;
        continue;
      }
      // bare '$' -> separator
      Out[j++] = '.';
      i++;
      continue;
    }

    // Linux flavour
    if(S[i] == '$') {
      if(i + 2 < len && IsFlatXDigit(S[i + 1]) && IsFlatXDigit(S[i + 2])) {
        Out[j++] = (char)((FlatHex(S[i + 1]) << 4) | FlatHex(S[i + 2]));
        i += 3;
        continue;
      }
      Out[j++] = '$';
      i++;
      continue;
    }
    if(S[i] == '_' && i + 1 < len && S[i + 1] == '_') {
      Out[j++] = '.';
      i += 2;
      continue;
    }
    Out[j++] = S[i++];
  }
  Out[j] = '\0';
  return Out;
}

bool llvm::isOxCamlMangledName(std::string_view MangledName) {
  return starts_with(MangledName, "_Caml") ||
         MatchedFlatPrefixLen(MangledName) != 0;
}

char *llvm::oxcamlDemangle(std::string_view Mangled) {
  if(starts_with(Mangled, "_Caml"))
    return demangleStructured(Mangled);

  if(size_t PrefixLen = MatchedFlatPrefixLen(Mangled)) {
    // flat1 (>= 5.3) is a superset of flat0; only fall back to flat0 (<= 5.2)
    // if flat1 rejects the symbol.
    if(char *Result = demangleFlat1(Mangled, PrefixLen))
      return Result;
    return demangleFlat0(Mangled, PrefixLen);
  }

  return nullptr;
}

// Length of the demangled name S[0..len) with any trailing OCaml compiler stamp
// removed. The stamp is a non-deterministic counter (plus an optional "_code"
// marker for code symbols) the compiler appends to keep linker symbols unique;
// it is unstable across builds and carries no source meaning. Recognised
// trailing forms: "_<digits>_<digits>_code", "_<digits>_code", "_<digits>".
// Source spans ("[...]"/"(...)") and operator/identifier characters are kept,
// since they do not end in "_<digits>".
static size_t lengthWithoutStamp(const char *S, size_t len) {
  // Strip one trailing "_<digits>" group ending at End; return the new end, or
  // End unchanged when there is no such group.
  auto stripGroup = [&](size_t End) -> size_t {
    size_t p = End;
    while(p > 0 && S[p - 1] >= '0' && S[p - 1] <= '9')
      p--;
    if(p < End && p > 0 && S[p - 1] == '_')
      return p - 1;
    return End;
  };

  if(len >= 5 && S[len - 5] == '_' && S[len - 4] == 'c' && S[len - 3] == 'o' &&
     S[len - 2] == 'd' && S[len - 1] == 'e') {
    // "..._code": only a stamp if preceded by at least one "_<digits>" group,
    // otherwise "_code" is part of a name (e.g. "process_code").
    size_t after_code = len - 5;
    size_t e1 = stripGroup(after_code);
    if(e1 == after_code)
      return len;
    return stripGroup(e1); // optionally a second group ("_<digits>_<digits>_code")
  }
  return stripGroup(len);
}

// Like oxcamlDemangle, but strips the trailing compiler stamp for display.
char *llvm::oxcamlDemangleNoStamp(std::string_view Mangled) {
  char *Result = oxcamlDemangle(Mangled);
  if(Result == nullptr)
    return nullptr;

  size_t len = 0;
  while(Result[len] != '\0')
    len++;
  Result[lengthWithoutStamp(Result, len)] = '\0';
  return Result;
}
