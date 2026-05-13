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

char *llvm::oxcamlDemangle(std::string_view Mangled) {
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
