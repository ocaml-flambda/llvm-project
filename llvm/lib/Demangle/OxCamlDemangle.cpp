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

// Sentinel returned by the Consume* helpers when no digits could be parsed, or
// the value would overflow. All callers must treat it as a parse failure.
static constexpr unsigned kParseError = ~0u;

static unsigned ConsumeUnsignedDecimal(std::string_view &sv) {
  unsigned res = 0, i = 0;
  while (i < sv.size() && sv[i] >= '0' && sv[i] <= '9') {
    unsigned digit = (unsigned)(sv[i] - '0');
    // Reject values that would overflow, mirroring the compiler's
    // int_of_string_opt, rather than wrapping silently.
    if (res > (kParseError - digit) / 10)
      return kParseError;
    res = res * 10 + digit;
    i++;
  }
  sv.remove_prefix(i);
  if (i == 0)
    return kParseError;
  return res;
}

static unsigned ConsumeUnsigned26(std::string_view &sv) {
  unsigned res = 0, i = 0;
  while (i < sv.size() && sv[i] >= 'A' && sv[i] <= 'Z') {
    unsigned digit = (unsigned)(sv[i] - 'A');
    if (res > (kParseError - digit) / 26)
      return kParseError;
    res = res * 26 + digit;
    i++;
  }
  sv.remove_prefix(i);
  if (i == 0)
    return kParseError;
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
  if(len == kParseError || len <= 0 || len > Mangled.size())
    return false;

  size_t split = Mangled.find('_');
  if(split >= len)
    return false;

  std::string_view coded = Mangled.substr(0, split);
  std::string_view raw = Mangled.substr(split+1, len-split-1);

  while(!coded.empty()) {
    unsigned chunklen = ConsumeUnsigned26(coded);
    if(chunklen == kParseError || chunklen > raw.size())
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
    if(len == kParseError || len <= 0 || len > Mangled.size())
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

  // The compiler parser (structured_mangling.ml, parse_location) requires the
  // exact "filename_line_col" shape with numeric line and column fields, and
  // rejects the whole symbol otherwise. Mirror that: reject here so the symbol
  // is passed through unchanged rather than emitting a malformed location.
  auto all_digits = [&](size_t begin, size_t end) {
    if(begin >= end)
      return false;
    for(size_t j = begin; j < end; j++)
      if(buf[j] < '0' || buf[j] > '9')
        return false;
    return true;
  };
  if(underscore_count < 2 ||
     !all_digits(first_underscore + 1, second_underscore) ||
     !all_digits(second_underscore + 1, temp_len)) {
    std::free(buf);
    return false;
  }

  // Output in format type(filename:line:col)
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

  std::free(buf);
  return true;
}

// Length of the structured-scheme prefix at the start of Mangled, or 0 if there
// is none. The bare prefix is "_Caml"; the macOS assembler prepends a leading
// underscore, giving "__Caml". Both are accepted, matching the compiler parser
// (structured_mangling.ml) and mirroring the flat path's "_caml" handling.
static size_t MatchedStructuredPrefixLen(std::string_view Mangled) {
  if(starts_with(Mangled, "__Caml"))
    return 6;
  if(starts_with(Mangled, "_Caml"))
    return 5;
  return 0;
}

// Demangle a symbol in the structured scheme (prefix "_Caml"/"__Caml").
static char *demangleStructured(std::string_view Mangled) {
  size_t prefix_len = MatchedStructuredPrefixLen(Mangled);
  if(prefix_len == 0)
    return nullptr;
  Mangled.remove_prefix(prefix_len);

  // Allocate the buffer at a reasonable size, as OutputBuffer allocates 992
  // bytes when starting from an empty buffer
  char *DemangledBuffer;
  DemangledBuffer = static_cast<char *>(std::malloc(Mangled.size()));
  if (DemangledBuffer == nullptr)
    std::terminate();
  OutputBuffer Demangled(DemangledBuffer, Mangled.size());

  // Free the output buffer and report failure; used on every error path below.
  auto fail = [&]() -> char * {
    std::free(Demangled.getBuffer());
    return nullptr;
  };

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
                  return fail();
              break;

          case 'S':  // anonymous Struct
          case 'L':  // anonymous function (L for lambda)
          case 'P': { // Partial application
              if(!Demangled.empty())
                  Demangled << '.';
              char typ = Mangled[0];
              Mangled.remove_prefix(1);
              if(!DecodeAnonymousLocation(Mangled, Demangled, typ))
                  return fail();
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
              return fail();
      }
  }

  // A valid symbol must contain at least one path item; reject names like
  // "_Caml" or "_Caml_123" that decoded to nothing.
  if(Demangled.getCurrentPosition() == 0)
      return fail();

  // Anything left in Mangled is the compiler-appended suffix (unique ids such as
  // "_5_code") that begins at the terminating underscore. Because the path was
  // decoded from exact length-prefixed identifiers, this boundary is precise, so
  // we drop the suffix here rather than re-deriving it heuristically over the
  // whole demangled string (which would also strip digits from real identifiers
  // such as "add_2").
  Demangled << '\0';

  return Demangled.getBuffer();
}

// ===========================================================================
// Flat scheme (legacy "caml"/"_caml" prefixes)
//
// Ports the flat0 (OCaml <= 5.2) demangler from the ocamlfilt reference tool:
// "__" is the module separator and "$xx" a hex escape. The trailing compiler
// stamp is stripped heuristically (flat names are not length-delimited, so the
// boundary cannot be known exactly as it can for the structured scheme).
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

bool llvm::isOxCamlMangledName(std::string_view MangledName) {
  return MatchedStructuredPrefixLen(MangledName) != 0 ||
         MatchedFlatPrefixLen(MangledName) != 0;
}

// Length of the demangled name S[0..len) with any trailing OCaml compiler stamp
// removed. The stamp is a non-deterministic counter (plus an optional "_code"
// marker for code symbols) the compiler appends to keep linker symbols unique;
// it is unstable across builds and carries no source meaning. Recognised
// trailing forms: "_<digits>_<digits>_code", "_<digits>_code", "_<digits>".
// Source spans ("[...]"/"(...)") and operator/identifier characters are kept,
// since they do not end in "_<digits>".
static size_t lengthWithoutStamp(const char *S, size_t len) {
  size_t end = len;

  // Drop a trailing "_code" marker, but only when it follows a "_<digits>"
  // group (otherwise "_code" is part of an identifier, e.g. "process_code").
  if(end >= 5 && S[end - 5] == '_' && S[end - 4] == 'c' && S[end - 3] == 'o' &&
     S[end - 2] == 'd' && S[end - 1] == 'e') {
    size_t p = end - 5;
    while(p > 0 && S[p - 1] >= '0' && S[p - 1] <= '9')
      p--;
    if(p < end - 5 && p > 0 && S[p - 1] == '_')
      end -= 5;
  }

  // Drop all trailing "_<digits>" groups. Non-numeric identifier characters
  // stop the scan, so "add_string" and "bar_baz" survive while a numeric stamp
  // like "..._4_9" does not.
  for(;;) {
    size_t p = end;
    while(p > 0 && S[p - 1] >= '0' && S[p - 1] <= '9')
      p--;
    if(p < end && p > 0 && S[p - 1] == '_')
      end = p - 1;
    else
      break;
  }
  return end;
}

// Truncate Result in place at the end of its source-meaningful content, i.e.
// drop the trailing compiler stamp. Used for the flat scheme only.
static void stripTrailingStamp(char *Result) {
  size_t len = 0;
  while(Result[len] != '\0')
    len++;
  Result[lengthWithoutStamp(Result, len)] = '\0';
}

char *llvm::oxcamlDemangle(std::string_view Mangled) {
  // Structured scheme: the demangler knows the exact path/suffix boundary and
  // drops the stamp itself, so its result needs no post-processing.
  if(MatchedStructuredPrefixLen(Mangled))
    return demangleStructured(Mangled);

  // Flat scheme: there are no length delimiters, so the trailing compiler stamp
  // can only be removed heuristically after demangling.
  if(size_t PrefixLen = MatchedFlatPrefixLen(Mangled)) {
    char *Result = demangleFlat0(Mangled, PrefixLen);
    if(Result == nullptr)
      return nullptr;
    stripTrailingStamp(Result);
    return Result;
  }

  return nullptr;
}
