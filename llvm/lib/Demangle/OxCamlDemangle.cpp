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

#include "llvm/Demangle/Demangle.h"
#include "llvm/Demangle/StringView.h"
#include "llvm/Demangle/Utility.h"

using llvm::itanium_demangle::OutputBuffer;
using llvm::itanium_demangle::StringView;

#define ERROR (~((unsigned)0))

static unsigned ConsumeUnsignedDecimal(StringView& sv) {
  unsigned res = 0, i = 0;
  while(sv[i] >= '0' && sv[i] <= '9') {
    res = res * 10 + (sv[i] - '0');
    i++;
  }
  sv = sv.dropFront(i);
  if(i == 0)
    return ERROR;
  return res;
}

static unsigned ConsumeUnsigned26(StringView& sv) {
  unsigned res = 0, i = 0;
  while(sv[i] >= 'A' && sv[i] <= 'Z') {
    res = res * 26 + (sv[i] - 'A');
    i++;
  }
  sv = sv.dropFront(i);
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
static bool DecodeEscaped(StringView& Mangled, OutputBuffer& Demangled) {
  unsigned len = ConsumeUnsignedDecimal(Mangled);
  if(len == ERROR || len <= 0 || len > Mangled.size())
    return false;

  size_t split = Mangled.find('_');
  if(split >= len)
    return false;

  StringView coded = Mangled.substr(0, split);
  StringView raw = Mangled.substr(split+1, len-split-1);

  while(!coded.empty()) {
    unsigned chunklen = ConsumeUnsigned26(coded);
    if(chunklen == ERROR || chunklen > raw.size())
      return false;
    Demangled << raw.substr(0, chunklen);
    raw = raw.dropFront(chunklen);

    unsigned i;
    for(i = 0; i+1 < coded.size() && islowerhex(coded[i]); i+=2) {
      if(!islowerhex(coded[i+1]))
        return false;
      char c = (char)(lowerhex(coded[i]) << 4 | lowerhex(coded[i+1]));
      Demangled << c;
    }
    coded = coded.dropFront(i);
  }

  if(!raw.empty())
    Demangled << raw;

  Mangled = Mangled.dropFront(len);
  return true;
}

// Decode identifier (either plain or escaped)
// Handles: <len><text> or u<len><coded>_<raw>
// Returns true on success, false on error
static bool DecodeIdentifier(StringView& Mangled, OutputBuffer& Demangled) {
  if(Mangled.consumeFront('u')) {
    // Escaped identifier
    return DecodeEscaped(Mangled, Demangled);
  } else {
    // Plain identifier with length prefix
    unsigned len = ConsumeUnsignedDecimal(Mangled);
    if(len == ERROR || len <= 0 || len > Mangled.size())
      return false;
    Demangled << Mangled.substr(0, len);
    Mangled = Mangled.dropFront(len);
    return true;
  }
}

// Decode anonymous location (format: filename_line_col)
// Anonymous functions, modules and partial functions are encoded as:
//   type(filename:line:col)
// where <type> can be fn, mod or partial
// Returns true on success, false on error
static bool DecodeAnonymousLocation(StringView& Mangled, OutputBuffer& Demangled, char typ) {
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
    std::free(temp_buf);
    return false;
  }

  size_t temp_len = TempDemangled.getCurrentPosition();

  // Parse filename_line_col format by finding the last two underscores
  size_t first_underscore = 0, second_underscore = 0;
  int underscore_count = 0;

  for(size_t j = temp_len; j > 0; j--) {
    if(temp_buf[j-1] == '_') {
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
      Demangled << temp_buf[j];
    Demangled << ':';
    for(size_t j = first_underscore + 1; j < second_underscore; j++)
      Demangled << temp_buf[j];
    Demangled << ':';
    for(size_t j = second_underscore + 1; j < temp_len; j++)
      Demangled << temp_buf[j];
    Demangled << ')';
  } else {
    // Fallback: just output the identifier as-is
    for(size_t j = 0; j < temp_len; j++)
      Demangled << temp_buf[j];
  }

  std::free(temp_buf);
  return true;
}

static void TrimStamp(OutputBuffer& Demangled) {
  size_t pos, end;
  char *buf = Demangled.getBuffer();
  pos = end = Demangled.getCurrentPosition() - 1;

  /* Trim the end only if it matches _[0-9]+ */
  while(pos > 0 && buf[pos] >= '0' && buf[pos] <= '9')
    pos--;
  if(pos > 0 && buf[pos] == '_' && pos < end)
    Demangled.setCurrentPosition(pos);
}

char *llvm::oxcamlDemangle(const char *MangledName) {
  StringView Mangled(MangledName);
  if(!Mangled.consumeFront("_Caml") && !Mangled.consumeFront("__Caml"))
    return nullptr;

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
              Mangled = Mangled.dropFront(1);
              if(!DecodeIdentifier(Mangled, Demangled))
                  ENDONERROR();
              break;

          case 'S':  // anonymous Struct
          case 'L':  // anonymous function (L for lambda)
          case 'P':  // Partial application
              if(!Demangled.empty())
                  Demangled << '.';
              Mangled = Mangled.dropFront(1);
              if(!DecodeAnonymousLocation(Mangled, Demangled, Mangled[0]))
                  ENDONERROR();
              break;


          case 'I':  // Inlining
              if(!Demangled.empty())
                  Demangled << '.';
              Mangled = Mangled.dropFront(1);
              Demangled << "<inlining>";
              break;

          default:
              ENDONERROR();
      }
  }

  TrimStamp(Demangled);
  Demangled << '\0';

  return Demangled.getBuffer();
}
