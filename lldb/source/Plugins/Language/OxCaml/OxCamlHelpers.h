//===-- OxCamlHelpers.h -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// \file
/// This file provides constants and helper functions for OCaml memory layout
/// and value representation.
///
/// OCaml uses a tagged pointer system where:
/// - LSB = 1: Immediate value (tagged integer, unit, etc.)
/// - LSB = 0: Pointer to heap block
///
/// Heap blocks have an 8-byte header before the data containing:
/// - bits 0-7:   tag (identifies block type)
/// - bits 8-9:   color (GC color bits)
/// - bits 10-55: wosize (size in words)
/// - bits 56-63: reserved (for mixed blocks: scannable_wosize + 1)

#ifndef LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLHELPERS_H
#define LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLHELPERS_H

#include <cstdint>
#include <string>

namespace lldb_private {
namespace formatters {
namespace oxcaml {
namespace helpers {

/// OCaml memory layout constants
namespace constants {
  /// Size of an OCaml word in bytes (always 8 bytes on 64-bit platforms)
  constexpr uint64_t WORD_SIZE = 8;

  /// Offset from data pointer to block header (header is 8 bytes before data)
  constexpr int64_t HEADER_OFFSET = -8;

  /// Bit mask for extracting tag from header (bits 0-7)
  constexpr uint64_t HEADER_TAG_MASK = 0xFF;

  /// Bit shift for extracting wosize from header
  constexpr uint64_t HEADER_WOSIZE_SHIFT = 10;

  /// Bit shift for extracting reserved field from header
  constexpr uint64_t HEADER_RESERVED_SHIFT = 56;

  /// Number of bits used for wosize field
  constexpr uint64_t HEADER_WOSIZE_BITS = 46;

  /// Bit mask for extracting wosize from header
  /// Computed as: ((1 << HEADER_WOSIZE_BITS) - 1) << HEADER_WOSIZE_SHIFT
  constexpr uint64_t HEADER_WOSIZE_MASK = (((1ULL << HEADER_WOSIZE_BITS) - 1ULL) << HEADER_WOSIZE_SHIFT);

  /// Bit mask for checking LSB (least significant bit) to discriminate immediate vs pointer
  constexpr uint64_t VALUE_LSB_MASK = 0x1;

  /// Bit shift for untagging immediate values (right shift by 1)
  constexpr int VALUE_UNTAG_SHIFT = 1;

  /// OCaml special block tag for float arrays (Double_array_tag)
  /// Float arrays store unboxed 8-byte IEEE 754 doubles
  constexpr uint8_t DOUBLE_ARRAY_TAG = 254;

  /// OCaml special block tags (tags >= 246)
  enum class SpecialTag : uint8_t {
    Lazy_tag = 246,           // Lazy values
    Closure_tag = 247,        // Function closures
    Object_tag = 248,         // Object instances (0xf8), exception descriptors
    Infix_tag = 249,          // Infix closures
    Forward_tag = 250,        // Forwarding pointers (GC)
    Abstract_tag = 251,       // Abstract values
    String_tag = 252,         // String values
    Double_tag = 253,         // Boxed float values
    Double_array_tag = 254,   // Float arrays
    Custom_tag = 255          // Custom blocks
  };

  /// Regular block tag for exceptions with arguments
  /// (not a "special" tag, but specific to exception representation)
  constexpr uint8_t EXCEPTION_BLOCK_TAG = 0;

  /// Size of IEEE 754 double precision float (8 bytes)
  /// Used for boxed floats (Double_tag) and float arrays (Double_array_tag)
  constexpr uint64_t DOUBLE_SIZE = 8;

  /// Floating-point type sizes in bytes
  constexpr uint64_t FLOAT16_SIZE = 2;  // float16# (half precision)
  constexpr uint64_t FLOAT32_SIZE = 4;  // float32# (single precision)
  constexpr uint64_t FLOAT64_SIZE = 8;  // float# (double precision)

  /// Integer type sizes in bytes (for Int32.t, Int64.t custom blocks)
  constexpr uint64_t INT32_SIZE = 4;
  constexpr uint64_t INT64_SIZE = 8;

  /// OCaml closure layout constants
  /// Closures store arity in the top 8 bits of the closinfo word
  constexpr int CLOSURE_ARITY_SHIFT = 56;

  /// Code pointer offset based on arity
  /// - Low arity (0 or 1): code pointer at offset 0 words
  /// - High arity (>= 2): code pointer at offset 2 words
  constexpr uint64_t CLOSURE_CODE_PTR_OFFSET_LOW_ARITY = 0;
  constexpr uint64_t CLOSURE_CODE_PTR_OFFSET_HIGH_ARITY = 2;

  /// Verify OCaml runtime invariant: 1 word = 1 double (both 8 bytes on 64-bit)
  static_assert(WORD_SIZE == DOUBLE_SIZE,
                "OCaml assumes 1 word equals 1 double (8 bytes on 64-bit platforms)");
  static_assert(DOUBLE_SIZE == FLOAT64_SIZE,
                "DOUBLE_SIZE should equal FLOAT64_SIZE");
}

/// OCaml type suffix constants
namespace suffixes {
  /// Signed integer type suffixes (unboxed types)
  constexpr const char* INT8_SUFFIX = "s";    // int8# -> #42s
  constexpr const char* INT16_SUFFIX = "S";   // int16# -> #42S
  constexpr const char* INT32_SUFFIX = "l";   // int32# -> #42l
  constexpr const char* INT64_SUFFIX = "L";   // int64# -> #42L

  /// Unsigned integer type suffixes (debugger extension, not standard OCaml)
  constexpr const char* UINT8_SUFFIX = "us";  // uint8# -> #42us
  constexpr const char* UINT16_SUFFIX = "uS"; // uint16# -> #42uS
  constexpr const char* UINT32_SUFFIX = "ul"; // uint32# -> #42ul
  constexpr const char* UINT64_SUFFIX = "uL"; // uint64# -> #42uL

  /// Other OCaml type suffixes
  constexpr const char* TAGGED_INT_SUFFIX = "";   // Tagged OCaml int -> 42
  constexpr const char* NATIVEINT_SUFFIX = "n";   // Nativeint.t -> #42n
  constexpr const char* FLOAT32_SUFFIX = "s";     // float32# -> #3.14s

  /// Get OCaml suffix for signed integer types
  /// \param byte_size Size of the integer type in bytes (1, 2, 4, 8)
  /// \return OCaml suffix string
  inline std::string GetSignedIntegerSuffix(uint64_t byte_size) {
    switch (byte_size) {
      case 1: return INT8_SUFFIX;
      case 2: return INT16_SUFFIX;
      case 4: return INT32_SUFFIX;
      case 8: return INT64_SUFFIX;
      default: return "";     // fallback
    }
  }

  /// Get OCaml suffix for unsigned integer types
  /// \param byte_size Size of the integer type in bytes (1, 2, 4, 8)
  /// \return OCaml-style suffix string
  inline std::string GetUnsignedIntegerSuffix(uint64_t byte_size) {
    switch (byte_size) {
      case 1: return UINT8_SUFFIX;
      case 2: return UINT16_SUFFIX;
      case 4: return UINT32_SUFFIX;
      case 8: return UINT64_SUFFIX;
      default: return "";     // fallback
    }
  }
}

/// Helper functions for OCaml value manipulation
namespace value {
  /// Check if an OCaml value is an immediate (LSB = 1)
  /// \param value The OCaml value to check
  /// \return true if value is an immediate (tagged integer, etc.), false if pointer
  inline bool IsImmediate(uint64_t value) {
    return (value & constants::VALUE_LSB_MASK) == 1;
  }

  /// Untag an OCaml immediate value by right-shifting
  /// \param value The tagged immediate value
  /// \return The untagged value as a signed integer
  inline int64_t UntagImmediate(uint64_t value) {
    int64_t signed_value = static_cast<int64_t>(value);
    return signed_value >> constants::VALUE_UNTAG_SHIFT;
  }
}

/// Helper functions for OCaml block header manipulation
namespace header {
  /// Get the address of the block header from a block pointer
  /// OCaml block pointers point to the data, with the header 8 bytes before
  /// \param block_ptr Address of the block data (LSB must be 0, not an immediate)
  /// \return Address where the block header is located (block_ptr - 8)
  inline lldb::addr_t GetHeaderAddress(lldb::addr_t block_ptr) {
    return block_ptr + constants::HEADER_OFFSET;
  }

  /// Extract tag field from block header (bits 0-7)
  /// \param header The 64-bit block header value
  /// \return The 8-bit tag value identifying the block type
  inline uint8_t ExtractTag(uint64_t header) {
    return header & constants::HEADER_TAG_MASK;
  }

  /// Extract wosize field from block header (bits 10-55)
  /// \param header The 64-bit block header value
  /// \return The wosize (size in words) of the block
  inline uint64_t ExtractWosize(uint64_t header) {
    return (header & constants::HEADER_WOSIZE_MASK) >> constants::HEADER_WOSIZE_SHIFT;
  }

  /// Extract reserved field from block header (bits 56-63)
  /// For mixed blocks, this encodes scannable_wosize as (reserved - 1)
  /// \param header The 64-bit block header value
  /// \return The 8-bit reserved field value
  inline uint8_t ExtractReserved(uint64_t header) {
    return header >> constants::HEADER_RESERVED_SHIFT;
  }

  /// Calculate scannable wosize for mixed blocks
  /// Mixed blocks have unboxed fields that should not be scanned by GC.
  /// The reserved field encodes: reserved = scannable_wosize + 1
  /// \param reserved The reserved field from the header
  /// \param total_wosize The total wosize from the header
  /// \return The number of words that contain scannable OCaml values
  inline uint64_t ExtractScannableWosize(uint8_t reserved, uint64_t total_wosize) {
    if (reserved > 0) {
      return reserved - 1;  // Mixed block: reserved encodes scannable_wosize + 1
    }
    return total_wosize;    // Regular block: all words are scannable
  }

  /// Calculate non-scannable wosize for mixed blocks
  /// \param reserved The reserved field from the header
  /// \param total_wosize The total wosize from the header
  /// \return The number of words that contain unboxed data (not OCaml values)
  inline uint64_t ExtractNonScannableWosize(uint8_t reserved, uint64_t total_wosize) {
    uint64_t scannable = ExtractScannableWosize(reserved, total_wosize);
    return total_wosize - scannable;
  }

  /// Parse complete header information in one call
  /// This is the preferred way to extract header fields as it ensures
  /// all fields are extracted consistently using the same bit operations.
  /// \param header The 64-bit block header value
  /// \param[out] tag The block tag (bits 0-7)
  /// \param[out] wosize The total size in words (bits 10-55)
  /// \param[out] reserved The reserved field (bits 56-63)
  inline void ParseHeader(uint64_t header, uint8_t& tag, uint64_t& wosize, uint8_t& reserved) {
    tag = ExtractTag(header);
    wosize = ExtractWosize(header);
    reserved = ExtractReserved(header);
  }

  /// Parse complete header with scannable/non-scannable wosize calculation
  /// \param header The 64-bit block header value
  /// \param[out] tag The block tag (bits 0-7)
  /// \param[out] wosize The total size in words (bits 10-55)
  /// \param[out] scannable_wosize Number of words containing OCaml values
  /// \param[out] non_scannable_wosize Number of words containing unboxed data
  inline void ParseHeaderWithMixedBlocks(uint64_t header, uint8_t& tag, uint64_t& wosize,
                                         uint64_t& scannable_wosize, uint64_t& non_scannable_wosize) {
    tag = ExtractTag(header);
    wosize = ExtractWosize(header);
    uint8_t reserved = ExtractReserved(header);
    scannable_wosize = ExtractScannableWosize(reserved, wosize);
    non_scannable_wosize = ExtractNonScannableWosize(reserved, wosize);
  }

  /// Get the address of the last word in an OCaml block
  /// \param block_ptr Address of the block data
  /// \param wosize Size of the block in words
  /// \return Address of the last word in the block
  inline lldb::addr_t GetLastWordAddress(lldb::addr_t block_ptr, uint64_t wosize) {
    return block_ptr + (wosize - 1) * constants::WORD_SIZE;
  }
}

/// Helper functions for OCaml string encoding
namespace string {
  /// Bit shift for extracting padding byte from last word (bits 56-63)
  constexpr int PADDING_BYTE_SHIFT = 56;

  /// Extract the padding byte from the last word of an OCaml string
  /// OCaml strings store padding information in bits 56-63 of the last word
  /// \param last_word The last word of the string block
  /// \return The padding byte value (0-7)
  inline uint8_t ExtractPaddingByte(uint64_t last_word) {
    return last_word >> PADDING_BYTE_SHIFT;
  }

  /// Calculate the actual length of an OCaml string
  /// OCaml string length = (wosize * WORD_SIZE) - padding - 1
  /// The -1 accounts for the null terminator
  /// \param wosize Size of the string block in words
  /// \param padding_byte Padding value from the last word
  /// \return The actual string length in bytes
  inline uint64_t CalculateStringLength(uint64_t wosize, uint8_t padding_byte) {
    return wosize * constants::WORD_SIZE - padding_byte - 1;
  }
}

/// Helper functions for OCaml closure handling
namespace closure {
  /// Extract arity from closinfo word
  /// The arity is stored in the top 8 bits (bits 56-63) of the closinfo word
  /// \param closinfo The closinfo word from the closure
  /// \return The arity value (0-255)
  inline uint8_t ExtractArity(uint64_t closinfo) {
    return closinfo >> constants::CLOSURE_ARITY_SHIFT;
  }

  /// Calculate code pointer offset based on closure arity
  /// OCaml closures store their code pointer at different offsets:
  /// - Arity 0 or 1: code pointer at offset 0 (first word of closure data)
  /// - Arity >= 2: code pointer at offset 2 (third word of closure data)
  /// \param closinfo The closinfo word containing arity information
  /// \return Offset in words from closure base to code pointer
  inline uint64_t GetCodePtrOffset(uint64_t closinfo) {
    uint8_t arity = ExtractArity(closinfo);
    return (arity == 0 || arity == 1)
        ? constants::CLOSURE_CODE_PTR_OFFSET_LOW_ARITY
        : constants::CLOSURE_CODE_PTR_OFFSET_HIGH_ARITY;
  }
}

} // namespace helpers
} // namespace oxcaml
} // namespace formatters
} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_LANGUAGE_OXCAML_OXCAMLHELPERS_H
