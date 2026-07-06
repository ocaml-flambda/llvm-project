//===-- OxCamlMarshal.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Marshaller for OCaml values living in a debugged process, producing the
// standard Marshal wire format.  Adapted from the OxCaml runtime's
// runtime/extern.c by way of extern_standalone.cpp (in the extern_standalone
// directory of this repository); the runtime code is deliberately followed
// closely, so much of this file is C-style rather than LLVM-style C++.
//
// Differences from extern_standalone.cpp:
// - Every access to the OCaml heap (block headers, fields, string and
//   float-array contents) goes through a MarshalMemoryReader instead of
//   dereferencing host pointers.  The "extern stack" of pending fields
//   stores debuggee addresses, not host pointers.  A failed read aborts
//   marshalling with an error rather than crashing LLDB.
// - The state is allocated per call instead of being a global, making the
//   marshaller safe to use from multiple debugger sessions.
// - The total output size is capped (see MarshalOCamlValue); a debugger can
//   be pointed at arbitrary words, and a corrupt "block header" could
//   otherwise describe a near-infinite object graph.
// - Errors are reported as llvm::Error.  Internally setjmp/longjmp is kept,
//   mirroring the runtime's raise-through behaviour; the code inside the
//   setjmp region is careful to hold no live C++ objects with destructors.
//
// Inherited from extern_standalone.cpp: 64-bit targets only; sharing always
// preserved; no compression; no custom-block serializers (so Int32.t,
// Int64.t, Nativeint.t, Bigarray values are rejected); closures,
// continuations, abstract and mixed blocks are rejected.
//
//===----------------------------------------------------------------------===//

#include "OxCamlMarshal.h"

#include "lldb/Target/Process.h"
#include "lldb/Utility/Status.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace lldb_private;
using namespace lldb_private::formatters::oxcaml;

// ---------------------------------------------------------------------------
// Memory readers

bool MarshalMemoryReader::ReadWord(uint64_t addr, uint64_t &word) {
  // All supported OxCaml targets share the host's byte order.
  return ReadBytes(addr, &word, sizeof(word));
}

bool ProcessMemoryReader::ReadBytes(uint64_t addr, void *buf, uint64_t len) {
  if (!m_process_sp)
    return false;
  if (len == 0)
    return true;
  Status error;
  const size_t bytes_read =
      m_process_sp->ReadMemory(addr, buf, static_cast<size_t>(len), error);
  return error.Success() && bytes_read == len;
}

// ---------------------------------------------------------------------------
// The marshaller

namespace {

typedef int64_t intnat;
typedef uint64_t uintnat;
typedef size_t asize_t;

static inline int umul_overflow(uintnat a, uintnat b, uintnat *res) {
  return __builtin_mul_overflow(a, b, res);
}

/* Value representation (from caml/mlvalues.h).  A [value] here is a word
   read from the debuggee; "pointers" are debuggee addresses and are never
   dereferenced directly. */

typedef intnat value;
typedef uintnat header_t;
typedef uintnat mlsize_t;
typedef unsigned int tag_t;

#define Is_null(v) ((v) == 0)

static inline int Is_long(value x) { return ((x & 1) != 0 || x == 0); }

static inline int Is_block(value x) { return ((x & 1) == 0 && x != 0); }

#define Long_val(x) ((x) >> 1)

/* Headers: 8 reserved bits (nonzero only for mixed blocks), then 46 size
   bits, 2 colour bits and 8 tag bits.  Header colour bits are ignored.
   Make_header uses the colour for out-of-heap blocks (NOT_MARKABLE, i.e. 3)
   and zero reserved bits. */

#define Tag_hd(hd) ((tag_t)((hd) & 0xFF))
#define Wosize_hd(hd) ((mlsize_t)(((hd) >> 10) & (((header_t)1 << 46) - 1)))
#define Reserved_hd(hd) ((hd) >> 56)

#define Make_header(wosize, tag) (((header_t)(wosize) << 10) + (3 << 8) + (tag))

#define Forcing_tag 244
#define Cont_tag 245
#define Lazy_tag 246
#define Closure_tag 247
#define Infix_tag 249
#define Forward_tag 250
#define Abstract_tag 251
#define String_tag 252
#define Double_tag 253
#define Double_array_tag 254
#define Custom_tag 255

/* The marshal format (from caml/intext.h) */

#define Intext_magic_number_small 0x8495A6BE
#define Intext_magic_number_big 0x8495A6BF

/* Header format for the "small" model: 20 bytes
       0   "small" magic number
       4   length of marshaled data, in bytes
       8   number of shared blocks
      12   size in words when read on a 32-bit platform
           (always 0 here: 32-bit readers are not supported)
      16   size in words when read on a 64-bit platform
   The 4 numbers are 32 bits each, in big endian.

   Header format for the "big" model: 32 bytes
       0   "big" magic number
       4   four reserved bytes, currently set to 0
       8   length of marshaled data, in bytes
      16   number of shared blocks
      24   size in words when read on a 64-bit platform
   The 3 numbers are 64 bits each, in big endian.
*/

#define MAX_INTEXT_HEADER_SIZE 32

/* Codes for the compact format */

#define PREFIX_SMALL_BLOCK 0x80
#define PREFIX_SMALL_INT 0x40
#define PREFIX_SMALL_STRING 0x20
#define CODE_INT8 0x0
#define CODE_INT16 0x1
#define CODE_INT32 0x2
#define CODE_INT64 0x3
#define CODE_SHARED8 0x4
#define CODE_SHARED16 0x5
#define CODE_SHARED32 0x6
#define CODE_SHARED64 0x14
#define CODE_BLOCK32 0x8
#define CODE_BLOCK64 0x13
#define CODE_STRING8 0x9
#define CODE_STRING32 0xA
#define CODE_STRING64 0x15
#define CODE_DOUBLE_BIG 0xB
#define CODE_DOUBLE_LITTLE 0xC
#define CODE_DOUBLE_ARRAY8_BIG 0xD
#define CODE_DOUBLE_ARRAY8_LITTLE 0xE
#define CODE_DOUBLE_ARRAY32_BIG 0xF
#define CODE_DOUBLE_ARRAY32_LITTLE 0x7
#define CODE_DOUBLE_ARRAY64_BIG 0x16
#define CODE_DOUBLE_ARRAY64_LITTLE 0x17
#define CODE_NULL 0x1f

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define CODE_DOUBLE_NATIVE CODE_DOUBLE_BIG
#define CODE_DOUBLE_ARRAY8_NATIVE CODE_DOUBLE_ARRAY8_BIG
#define CODE_DOUBLE_ARRAY32_NATIVE CODE_DOUBLE_ARRAY32_BIG
#define CODE_DOUBLE_ARRAY64_NATIVE CODE_DOUBLE_ARRAY64_BIG
#define Htobe16(x) ((uint16_t)(x))
#define Htobe32(x) ((uint32_t)(x))
#define Htobe64(x) ((uint64_t)(x))
#else
#define CODE_DOUBLE_NATIVE CODE_DOUBLE_LITTLE
#define CODE_DOUBLE_ARRAY8_NATIVE CODE_DOUBLE_ARRAY8_LITTLE
#define CODE_DOUBLE_ARRAY32_NATIVE CODE_DOUBLE_ARRAY32_LITTLE
#define CODE_DOUBLE_ARRAY64_NATIVE CODE_DOUBLE_ARRAY64_LITTLE
#define Htobe16(x) __builtin_bswap16((uint16_t)(x))
#define Htobe32(x) __builtin_bswap32((uint32_t)(x))
#define Htobe64(x) __builtin_bswap64((uint64_t)(x))
#endif

/* Size-ing data structures for extern.  Chosen so that
   sizeof(struct output_block) is slightly below 8Kb. */

#define SIZE_EXTERN_OUTPUT_BLOCK 8100

struct output_block {
  struct output_block *next;
  char *end;
  char data[SIZE_EXTERN_OUTPUT_BLOCK];
};

/* Error codes.  The message carried alongside is what reaches the user. */

typedef enum {
  EXTERN_OK = 0,
  EXTERN_ERROR_OUT_OF_MEMORY = 1,
  EXTERN_ERROR_INVALID_ARGUMENT = 2,
  EXTERN_ERROR_READ_FAILURE = 3,
  EXTERN_ERROR_OUTPUT_TOO_LARGE = 4
} extern_error_code_t;

/* Stack for pending values to marshal.  Each item is a run of block fields
   still to be serialized, identified by the debuggee address of the next
   field (the runtime version stores a host pointer instead). */

#define EXTERN_STACK_INIT_SIZE 256
#define EXTERN_STACK_MAX_SIZE (1024 * 1024 * 100)

struct extern_item {
  uint64_t field_addr;
  mlsize_t count;
};

/* Hash table to record already-marshaled objects and their positions */

struct object_position {
  value obj;
  uintnat pos;
};

/* The hash table uses open addressing, linear probing, and a redundant
   representation:
   - a bitvector [present] records which entries of the table are occupied;
   - an array [entries] records (object, position) pairs for the entries
     that are occupied.
   The bitvector is much smaller than the array (1/128th on 64-bit
   platforms), so it has better locality, making it faster to determine
   that an object is not in the table.  Also, it makes it faster to empty
   or initialize a table: only the [present] bitvector needs to be filled
   with zeros, the [entries] array can be left uninitialized. */

struct position_table {
  int shift;
  mlsize_t size;      /* size == 1 << (wordsize - shift) */
  mlsize_t mask;      /* mask == size - 1 */
  mlsize_t threshold; /* threshold == a fixed fraction of size */
  uintnat *present;   /* [Bitvect_size(size)] */
  struct object_position *entries; /* [size]  */
};

#define Bits_word (8 * sizeof(uintnat))
#define Bitvect_size(n) (((n) + Bits_word - 1) / Bits_word)

#define POS_TABLE_INIT_SIZE_LOG2 8
#define POS_TABLE_INIT_SIZE (1 << POS_TABLE_INIT_SIZE_LOG2)

struct extern_state {

  /* How to read the debuggee's memory */
  MarshalMemoryReader *reader;

  uintnat obj_counter; /* Number of objects emitted so far */
  uintnat size_64;     /* Size in words of 64-bit block for struct. */

  /* Return point for error reporting */
  jmp_buf error_return;
  extern_error_code_t error_code;
  const char *error_msg; /* either a literal or error_buf */
  char error_buf[160];

  /* Output size cap: bytes written into closed output blocks so far, and
     the limit above which marshalling is abandoned */
  uintnat output_written;
  uintnat max_output;

  /* Stack for pending values to marshal */
  struct extern_item extern_stack_init[EXTERN_STACK_INIT_SIZE];
  struct extern_item *extern_stack;
  struct extern_item *extern_stack_limit;

  /* Hash table to record already marshalled objects */
  uintnat pos_table_present_init[Bitvect_size(POS_TABLE_INIT_SIZE)];
  struct object_position pos_table_entries_init[POS_TABLE_INIT_SIZE];
  struct position_table pos_table;

  /* To buffer the output */

  char *extern_ptr;
  char *extern_limit;

  struct output_block *extern_output_first;
  struct output_block *extern_output_block;
};

[[noreturn]] static void extern_raise(struct extern_state *s,
                                      extern_error_code_t code,
                                      const char *msg) {
  s->error_code = code;
  s->error_msg = msg;
  longjmp(s->error_return, 1);
}

static void init_extern_stack(struct extern_state *s) {
  /* (Re)initialize the globals for next time around */
  s->extern_stack = s->extern_stack_init;
  s->extern_stack_limit = s->extern_stack + EXTERN_STACK_INIT_SIZE;
}

/* Forward declarations */

[[noreturn]] static void extern_out_of_memory(struct extern_state *s);

[[noreturn]] static void extern_invalid_argument(struct extern_state *s,
                                                 const char *msg);

[[noreturn]] static void extern_stack_overflow(struct extern_state *s);

[[noreturn]] static void extern_read_failure(struct extern_state *s,
                                             uint64_t addr, uint64_t len);

static void free_extern_output(struct extern_state *s);

/* Reading the debuggee's memory.  Both helpers raise (after freeing the
   output) if the read fails, so callers can assume success. */

static void read_bytes(struct extern_state *s, uint64_t addr, void *buf,
                       uint64_t len) {
  if (!s->reader->ReadBytes(addr, buf, len))
    extern_read_failure(s, addr, len);
}

static uint64_t read_word(struct extern_state *s, uint64_t addr) {
  uint64_t word;
  if (!s->reader->ReadWord(addr, word))
    extern_read_failure(s, addr, sizeof(word));
  return word;
}

/* The header word lives one word before the address a block pointer
   designates (Hd_val in the runtime). */
static header_t read_header(struct extern_state *s, value v) {
  return (header_t)read_word(s, (uint64_t)v - sizeof(value));
}

/* Field(v, i) in the runtime */
static value read_field(struct extern_state *s, value v, mlsize_t i) {
  return (value)read_word(s, (uint64_t)v + i * sizeof(value));
}

/* caml_string_length in the runtime: the length is the block size in bytes
   minus one, minus the padding count stored in the last byte.  [wosize] is
   the block size from the header (nonzero: atoms are handled earlier). */
static mlsize_t string_length(struct extern_state *s, value v,
                              mlsize_t wosize) {
  mlsize_t temp = wosize * sizeof(value) - 1;
  uint8_t padding;
  read_bytes(s, (uint64_t)v + temp, &padding, 1);
  if (padding > sizeof(value) - 1)
    extern_invalid_argument(s, "output_value: corrupted string block");
  return temp - padding;
}

static void extern_free_stack(struct extern_state *s) {
  /* Free the extern stack if needed */
  if (s->extern_stack != s->extern_stack_init) {
    free(s->extern_stack);
    init_extern_stack(s);
  }
}

static struct extern_item *extern_resize_stack(struct extern_state *s,
                                               const struct extern_item *sp) {
  asize_t newsize = 2 * (s->extern_stack_limit - s->extern_stack);
  asize_t sp_offset = sp - s->extern_stack;
  struct extern_item *newstack;

  if (newsize >= EXTERN_STACK_MAX_SIZE)
    extern_stack_overflow(s);
  newstack =
      (struct extern_item *)calloc(newsize, sizeof(struct extern_item));
  if (newstack == NULL)
    extern_stack_overflow(s);

  /* Copy items from the old stack to the new stack */
  memcpy(newstack, s->extern_stack, sizeof(struct extern_item) * sp_offset);

  /* Free the old stack if it is not the initial stack */
  if (s->extern_stack != s->extern_stack_init)
    free(s->extern_stack);

  s->extern_stack = newstack;
  s->extern_stack_limit = newstack + newsize;
  return newstack + sp_offset;
}

/* Multiplicative Fibonacci hashing
   (Knuth, TAOCP vol 3, section 6.4, page 518).
   HASH_FACTOR is (sqrt(5) - 1) / 2 * 2^wordsize. */
#define HASH_FACTOR 11400714819323198486UL
#define Hash(v, shift) (((uintnat)(v) * HASH_FACTOR) >> (shift))

/* When the table becomes 2/3 full, its size is increased. */
#define Threshold(sz) (((sz) * 2) / 3)

/* Initialize the position table */

static void extern_init_position_table(struct extern_state *s) {
  s->pos_table.size = POS_TABLE_INIT_SIZE;
  s->pos_table.shift = 8 * sizeof(value) - POS_TABLE_INIT_SIZE_LOG2;
  s->pos_table.mask = POS_TABLE_INIT_SIZE - 1;
  s->pos_table.threshold = Threshold(POS_TABLE_INIT_SIZE);
  s->pos_table.present = s->pos_table_present_init;
  s->pos_table.entries = s->pos_table_entries_init;
  memset(s->pos_table_present_init, 0,
         Bitvect_size(POS_TABLE_INIT_SIZE) * sizeof(uintnat));
}

/* Free the position table */

static void extern_free_position_table(struct extern_state *s) {
  if (s->pos_table.present != s->pos_table_present_init) {
    free(s->pos_table.present);
    free(s->pos_table.entries);
    /* Protect against repeated calls to extern_free_position_table */
    s->pos_table.present = s->pos_table_present_init;
    s->pos_table.entries = s->pos_table_entries_init;
  }
}

/* Accessing bitvectors */

static inline uintnat bitvect_test(uintnat *bv, uintnat i) {
  return bv[i / Bits_word] & ((uintnat)1 << (i & (Bits_word - 1)));
}

static inline void bitvect_set(uintnat *bv, uintnat i) {
  bv[i / Bits_word] |= ((uintnat)1 << (i & (Bits_word - 1)));
}

/* Grow the position table */

static void extern_resize_position_table(struct extern_state *s) {
  mlsize_t new_size, new_byte_size;
  int new_shift;
  uintnat *new_present;
  struct object_position *new_entries;
  uintnat h;
  struct position_table old = s->pos_table;

  /* Grow the table quickly (x 8) up to 10^6 entries,
     more slowly (x 2) afterwards. */
  if (old.size < 1000000) {
    new_size = 8 * old.size;
    new_shift = old.shift - 3;
  } else {
    new_size = 2 * old.size;
    new_shift = old.shift - 1;
  }
  if (new_size == 0 ||
      umul_overflow(new_size, sizeof(struct object_position), &new_byte_size))
    extern_out_of_memory(s);
  new_entries = (struct object_position *)malloc(new_byte_size);
  if (new_entries == NULL)
    extern_out_of_memory(s);
  new_present = (uintnat *)calloc(Bitvect_size(new_size), sizeof(uintnat));
  if (new_present == NULL) {
    free(new_entries);
    extern_out_of_memory(s);
  }
  s->pos_table.size = new_size;
  s->pos_table.shift = new_shift;
  s->pos_table.mask = new_size - 1;
  s->pos_table.threshold = Threshold(new_size);
  s->pos_table.present = new_present;
  s->pos_table.entries = new_entries;

  /* Insert every entry of the old table in the new table */
  for (uintnat i = 0; i < old.size; i++) {
    if (!bitvect_test(old.present, i))
      continue;
    h = Hash(old.entries[i].obj, s->pos_table.shift);
    while (bitvect_test(new_present, h)) {
      h = (h + 1) & s->pos_table.mask;
    }
    bitvect_set(new_present, h);
    new_entries[h] = old.entries[i];
  }

  /* Free the old tables if they are not the initial ones */
  if (old.present != s->pos_table_present_init) {
    free(old.present);
    free(old.entries);
  }
}

/* Determine whether the given object [obj] is in the hash table.
   If so, set [*pos_out] to its position in the output and return 1.
   If not, return 0.
   Either way, set [*h_out] to the hash value appropriate for
   [extern_record_location]. */
static inline int extern_lookup_position(struct extern_state *s, value obj,
                                         uintnat *pos_out, uintnat *h_out) {
  uintnat h = Hash(obj, s->pos_table.shift);
  while (1) {
    if (!bitvect_test(s->pos_table.present, h)) {
      *h_out = h;
      return 0;
    }
    if (s->pos_table.entries[h].obj == obj) {
      *h_out = h;
      *pos_out = s->pos_table.entries[h].pos;
      return 1;
    }
    h = (h + 1) & s->pos_table.mask;
  }
}

/* Record the output position for the given object [obj]. */
/* The [h] parameter is the index in the hash table where the object
   must be inserted.  It was determined during lookup. */
static void extern_record_location(struct extern_state *s, value obj,
                                   uintnat h) {
  bitvect_set(s->pos_table.present, h);
  s->pos_table.entries[h].obj = obj;
  s->pos_table.entries[h].pos = s->obj_counter;
  s->obj_counter++;
  if (s->obj_counter >= s->pos_table.threshold)
    extern_resize_position_table(s);
}

/* To buffer the output */

static void init_extern_output(struct extern_state *s) {
  s->extern_output_first =
      (struct output_block *)malloc(sizeof(struct output_block));
  if (s->extern_output_first == NULL)
    extern_raise(s, EXTERN_ERROR_OUT_OF_MEMORY,
                 "output_value: out of memory");
  s->extern_output_block = s->extern_output_first;
  s->extern_output_block->next = NULL;
  s->extern_ptr = s->extern_output_block->data;
  s->extern_limit = s->extern_output_block->data + SIZE_EXTERN_OUTPUT_BLOCK;
  s->output_written = 0;
}

static void close_extern_output(struct extern_state *s) {
  s->extern_output_block->end = s->extern_ptr;
}

static void free_extern_output(struct extern_state *s) {
  for (struct output_block *blk = s->extern_output_first, *nextblk;
       blk != NULL; blk = nextblk) {
    nextblk = blk->next;
    free(blk);
  }
  s->extern_output_first = NULL;
  extern_free_stack(s);
  extern_free_position_table(s);
}

[[noreturn]] static void extern_output_too_large(struct extern_state *s) {
  free_extern_output(s);
  extern_raise(s, EXTERN_ERROR_OUTPUT_TOO_LARGE,
               "output_value: marshalled representation exceeds the "
               "configured size limit");
}

static void grow_extern_output(struct extern_state *s, intnat required) {
  struct output_block *blk;
  intnat extra;

  s->extern_output_block->end = s->extern_ptr;
  /* Enforce the output cap here rather than on every byte written: a new
     block is requested at worst every SIZE_EXTERN_OUTPUT_BLOCK bytes, so
     the cap can only be overshot by one block. */
  s->output_written += s->extern_ptr - s->extern_output_block->data;
  if (s->output_written + (uintnat)required > s->max_output)
    extern_output_too_large(s);
  if (required <= SIZE_EXTERN_OUTPUT_BLOCK / 2)
    extra = 0;
  else
    extra = required;
  blk = (struct output_block *)malloc(sizeof(struct output_block) + extra);
  if (blk == NULL)
    extern_out_of_memory(s);
  s->extern_output_block->next = blk;
  s->extern_output_block = blk;
  s->extern_output_block->next = NULL;
  s->extern_ptr = s->extern_output_block->data;
  s->extern_limit =
      s->extern_output_block->data + SIZE_EXTERN_OUTPUT_BLOCK + extra;
}

static intnat extern_output_length(struct extern_state *s) {
  struct output_block *blk;
  intnat len;

  for (len = 0, blk = s->extern_output_first; blk != NULL; blk = blk->next)
    len += blk->end - blk->data;
  return len;
}

/* Error raising, with cleanup */

static void extern_out_of_memory(struct extern_state *s) {
  free_extern_output(s);
  extern_raise(s, EXTERN_ERROR_OUT_OF_MEMORY, "output_value: out of memory");
}

static void extern_invalid_argument(struct extern_state *s, const char *msg) {
  free_extern_output(s);
  extern_raise(s, EXTERN_ERROR_INVALID_ARGUMENT, msg);
}

static void extern_stack_overflow(struct extern_state *s) {
  free_extern_output(s);
  extern_raise(s, EXTERN_ERROR_OUT_OF_MEMORY,
               "output_value: stack overflow");
}

static void extern_read_failure(struct extern_state *s, uint64_t addr,
                                uint64_t len) {
  free_extern_output(s);
  snprintf(s->error_buf, sizeof(s->error_buf),
           "output_value: cannot read %llu byte(s) at address 0x%llx in the "
           "debugged process",
           (unsigned long long)len, (unsigned long long)addr);
  extern_raise(s, EXTERN_ERROR_READ_FAILURE, s->error_buf);
}

/* Conversion to big-endian */

static inline void store16(char *dst, int n) {
  uint16_t u = Htobe16(n);
  memcpy(dst, &u, 2);
}

static inline void store32(char *dst, intnat n) {
  uint32_t u = Htobe32(n);
  memcpy(dst, &u, 4);
}

static inline void store64(char *dst, int64_t n) {
  uint64_t u = Htobe64(n);
  memcpy(dst, &u, 8);
}

/* Write characters, integers, and blocks in the output buffer */

static inline void writebyte(struct extern_state *s, int c) {
  if (s->extern_ptr >= s->extern_limit)
    grow_extern_output(s, 1);
  *s->extern_ptr++ = c;
}

static void writeblock(struct extern_state *s, const char *data, intnat len) {
  if (s->extern_ptr + len > s->extern_limit)
    grow_extern_output(s, len);
  memcpy(s->extern_ptr, data, len);
  s->extern_ptr += len;
}

/* Copy [len] bytes from debuggee address [addr] into the output.  The copy
   goes through a bounded host buffer so that a huge (possibly corrupt)
   length does not translate into a huge host allocation before the output
   cap check in grow_extern_output can fire. */
static void writeblock_from_target(struct extern_state *s, uint64_t addr,
                                   intnat len) {
  char chunk[4096];
  while (len > 0) {
    intnat n = len < (intnat)sizeof(chunk) ? len : (intnat)sizeof(chunk);
    read_bytes(s, addr, chunk, n);
    writeblock(s, chunk, n);
    addr += n;
    len -= n;
  }
}

static void writecode8(struct extern_state *s, int code, intnat val) {
  if (s->extern_ptr + 2 > s->extern_limit)
    grow_extern_output(s, 2);
  s->extern_ptr[0] = code;
  s->extern_ptr[1] = val;
  s->extern_ptr += 2;
}

static void writecode16(struct extern_state *s, int code, intnat val) {
  if (s->extern_ptr + 3 > s->extern_limit)
    grow_extern_output(s, 3);
  s->extern_ptr[0] = code;
  store16(s->extern_ptr + 1, (int)val);
  s->extern_ptr += 3;
}

static void writecode32(struct extern_state *s, int code, intnat val) {
  if (s->extern_ptr + 5 > s->extern_limit)
    grow_extern_output(s, 5);
  s->extern_ptr[0] = code;
  store32(s->extern_ptr + 1, val);
  s->extern_ptr += 5;
}

static void writecode64(struct extern_state *s, int code, intnat val) {
  if (s->extern_ptr + 9 > s->extern_limit)
    grow_extern_output(s, 9);
  s->extern_ptr[0] = code;
  store64(s->extern_ptr + 1, val);
  s->extern_ptr += 9;
}

/* Marshaling integers */

static inline void extern_int(struct extern_state *s, intnat n) {
  if (n >= 0 && n < 0x40) {
    writebyte(s, PREFIX_SMALL_INT + n);
  } else if (n >= -(1 << 7) && n < (1 << 7)) {
    writecode8(s, CODE_INT8, n);
  } else if (n >= -(1 << 15) && n < (1 << 15)) {
    writecode16(s, CODE_INT16, n);
  } else if (n < -((intnat)1 << 30) || n >= ((intnat)1 << 30)) {
    writecode64(s, CODE_INT64, n);
  } else {
    writecode32(s, CODE_INT32, n);
  }
}

static inline void extern_null(struct extern_state *s) {
  writecode8(s, CODE_NULL, 0);
}

/* Marshaling references to previously-marshaled blocks */

static inline void extern_shared_reference(struct extern_state *s, uintnat d) {
  if (d < 0x100) {
    writecode8(s, CODE_SHARED8, d);
  } else if (d < 0x10000) {
    writecode16(s, CODE_SHARED16, d);
  } else if (d >= (uintnat)1 << 32) {
    writecode64(s, CODE_SHARED64, d);
  } else {
    writecode32(s, CODE_SHARED32, d);
  }
}

/* Marshaling block headers */

static inline void extern_header(struct extern_state *s, mlsize_t sz,
                                 tag_t tag) {
  if (tag < 16 && sz < 8) {
    writebyte(s, PREFIX_SMALL_BLOCK + tag + (sz << 4));
  } else {
    header_t hd = Make_header(sz, tag);
    if (hd < (uintnat)1 << 32)
      writecode32(s, CODE_BLOCK32, hd);
    else
      writecode64(s, CODE_BLOCK64, hd);
  }
}

/* Marshaling strings */

static inline void extern_string(struct extern_state *s, value v,
                                 mlsize_t len) {
  if (len < 0x20) {
    writebyte(s, PREFIX_SMALL_STRING + len);
  } else if (len < 0x100) {
    writecode8(s, CODE_STRING8, len);
  } else {
    if (len < (uintnat)1 << 32)
      writecode32(s, CODE_STRING32, len);
    else
      writecode64(s, CODE_STRING64, len);
  }
  writeblock_from_target(s, (uint64_t)v, len);
}

/* Marshaling FP numbers.  Doubles are assumed to be IEEE 754 with the same
   byte order as integers on both the host and the debuggee; the
   CODE_DOUBLE*_NATIVE codes record which endianness that is. */

static inline void extern_double(struct extern_state *s, value v) {
  writebyte(s, CODE_DOUBLE_NATIVE);
  writeblock_from_target(s, (uint64_t)v, 8);
}

/* Marshaling FP arrays */

static inline void extern_double_array(struct extern_state *s, value v,
                                       mlsize_t nfloats) {
  if (nfloats < 0x100) {
    writecode8(s, CODE_DOUBLE_ARRAY8_NATIVE, nfloats);
  } else {
    if (nfloats < (uintnat)1 << 32)
      writecode32(s, CODE_DOUBLE_ARRAY32_NATIVE, nfloats);
    else
      writecode64(s, CODE_DOUBLE_ARRAY64_NATIVE, nfloats);
  }
  writeblock_from_target(s, (uint64_t)v, nfloats * 8);
}

/* Marshal the given value in the output buffer */

static void extern_rec(struct extern_state *s, value v) {
  struct extern_item *sp;
  uintnat h = 0;
  uintnat pos = 0;

  /* for Double_tag and Double_array_tag */
  static_assert(sizeof(double) == 8, "");

  extern_init_position_table(s);
  sp = s->extern_stack;

  while (1) {
    if (Is_null(v)) {
      extern_null(s);
    } else if (Is_long(v)) {
      extern_int(s, Long_val(v));
    } else {
      header_t hd = read_header(s, v);
      tag_t tag = Tag_hd(hd);
      mlsize_t sz = Wosize_hd(hd);
      if (Reserved_hd(hd) != 0) {
        /* Nonzero reserved header bits indicate a mixed block. */
        extern_invalid_argument(s, "output_value: mixed block");
        break;
      }

      if (tag == Forward_tag) {
        value f = read_field(s, v, 0);
        tag_t ftag = Is_block(f) ? Tag_hd(read_header(s, f)) : 0;
        if (Is_block(f) && (ftag == Forward_tag || ftag == Lazy_tag ||
                            ftag == Forcing_tag ||
                            /* Double_tag check because of flat float
                               arrays */
                            ftag == Double_tag)) {
          /* Do not short-circuit the pointer. */
        } else {
          v = f;
          continue;
        }
      }
      /* Atoms are treated specially for two reasons: they are not allocated
         in the externed block, and they are automatically shared. */
      if (sz == 0) {
        extern_header(s, 0, tag);
        goto next_item;
      }
      /* Check if object already seen */
      if (extern_lookup_position(s, v, &pos, &h)) {
        extern_shared_reference(s, s->obj_counter - pos);
        goto next_item;
      }
      /* Output the contents of the object */
      switch (tag) {
      case String_tag: {
        mlsize_t len = string_length(s, v, sz);
        extern_string(s, v, len);
        s->size_64 += 1 + (len + 8) / 8;
        extern_record_location(s, v, h);
        break;
      }
      case Double_tag: {
        extern_double(s, v);
        s->size_64 += 1 + 1;
        extern_record_location(s, v, h);
        break;
      }
      case Double_array_tag: {
        /* sizeof(double) == sizeof(value), per the static_assert above */
        mlsize_t nfloats = sz;
        extern_double_array(s, v, nfloats);
        s->size_64 += 1 + nfloats;
        extern_record_location(s, v, h);
        break;
      }
      case Abstract_tag:
        extern_invalid_argument(s,
                                "output_value: abstract value (Abstract)");
        break;
      case Infix_tag:
        /* An infix pointer into a closure block; closures cannot be
           marshalled (see Closure_tag below). */
        extern_invalid_argument(s, "output_value: functional value");
        break;
      case Custom_tag:
        /* Custom blocks (Int32, Int64, Nativeint, Bigarray, ...) cannot
           be marshalled here: their serialization functions live in the
           debuggee and cannot be run. */
        extern_invalid_argument(s, "output_value: abstract value (Custom)");
        break;
      case Closure_tag:
        /* The runtime's code-fragment table is not available here, so
           code pointers, and hence closures, cannot be marshalled. */
        extern_invalid_argument(s, "output_value: functional value");
        break;
      case Cont_tag:
        extern_invalid_argument(s, "output_value: continuation value");
        break;
      default: {
        extern_header(s, sz, tag);
        s->size_64 += 1 + sz;
        extern_record_location(s, v, h);
        /* Remember that we still have to serialize fields 1 ... sz - 1 */
        if (sz > 1) {
          sp++;
          if (sp >= s->extern_stack_limit)
            sp = extern_resize_stack(s, sp);
          sp->field_addr = (uint64_t)v + sizeof(value);
          sp->count = sz - 1;
        }
        /* Continue serialization with the first field */
        v = read_field(s, v, 0);
        continue;
      }
      }
    }
  next_item:
    /* Pop one more item to marshal, if any */
    if (sp == s->extern_stack) {
      /* We are done.   Cleanup the stack and leave the function */
      extern_free_stack(s);
      extern_free_position_table(s);
      return;
    }
    v = (value)read_word(s, sp->field_addr);
    sp->field_addr += sizeof(value);
    if (--(sp->count) == 0)
      sp--;
  }
  /* Never reached as function leaves with return */
}

static intnat extern_value(struct extern_state *s, value v,
                           /*out*/ char header[MAX_INTEXT_HEADER_SIZE],
                           /*out*/ int *header_len) {
  intnat res_len;
  /* Initializations */
  s->obj_counter = 0;
  s->size_64 = 0;
  /* Marshal the object */
  extern_rec(s, v);
  /* Record end of output */
  close_extern_output(s);
  /* Write the header */
  res_len = extern_output_length(s);
  if (res_len >= ((intnat)1 << 32) || s->size_64 >= ((uintnat)1 << 32)) {
    /* The object is too big for the small header format.
       Use the big header. */
    store32(header, Intext_magic_number_big);
    store32(header + 4, 0);
    store64(header + 8, res_len);
    store64(header + 16, s->obj_counter);
    store64(header + 24, s->size_64);
    *header_len = 32;
    return res_len;
  }
  /* Use the small header format.  The field at offset 12 is the size in
     words when read on a 32-bit platform; 32-bit readers are not
     supported, so 0 is written instead. */
  store32(header, Intext_magic_number_small);
  store32(header + 4, res_len);
  store32(header + 8, s->obj_counter);
  store32(header + 12, 0);
  store32(header + 16, s->size_64);
  *header_len = 20;
  return res_len;
}

/* The entry point.  On success returns EXTERN_OK and stores a malloc'd
   buffer and its length in [*buf] and [*len]; the caller frees the buffer.
   On failure returns the error code (with the message in s->error_msg) and
   writes neither output. */

static extern_error_code_t output_value_to_malloc(struct extern_state *s,
                                                  value v,
                                                  /*out*/ char **buf,
                                                  /*out*/ intnat *len) {
  char header[MAX_INTEXT_HEADER_SIZE];
  int header_len;
  intnat data_len;
  char *res;

  s->error_code = EXTERN_OK;
  s->error_msg = NULL;
  if (setjmp(s->error_return) != 0)
    return s->error_code;

  init_extern_output(s);
  data_len = extern_value(s, v, header, &header_len);
  res = (char *)malloc(header_len + data_len);
  if (res == NULL)
    extern_out_of_memory(s);
  *buf = res;
  *len = header_len + data_len;
  memcpy(res, header, header_len);
  res += header_len;
  for (struct output_block *blk = s->extern_output_first, *nextblk;
       blk != NULL; blk = nextblk) {
    intnat n = blk->end - blk->data;
    memcpy(res, blk->data, n);
    res += n;
    nextblk = blk->next;
    free(blk);
  }
  s->extern_output_first = NULL;
  return EXTERN_OK;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry point

llvm::Expected<std::vector<uint8_t>>
lldb_private::formatters::oxcaml::MarshalOCamlValue(uint64_t root,
                                                    MarshalMemoryReader &reader,
                                                    uint64_t max_output_bytes) {
  // The state is large (~16 KiB of inline stacks and tables), so it lives on
  // the heap.  It is plain data: no constructors or destructors may exist
  // inside the setjmp region.
  struct extern_state *s =
      (struct extern_state *)calloc(1, sizeof(struct extern_state));
  if (s == NULL)
    return llvm::createStringError(
        "output_value: out of memory allocating marshalling state");
  s->reader = &reader;
  s->max_output = max_output_bytes;
  init_extern_stack(s);

  char *buf = NULL;
  intnat len = 0;
  extern_error_code_t err =
      output_value_to_malloc(s, (value)root, &buf, &len);
  if (err != EXTERN_OK) {
    std::string msg(s->error_msg ? s->error_msg
                                 : "output_value: unknown error");
    free(s);
    return llvm::createStringError("%s", msg.c_str());
  }
  free(s);

  std::vector<uint8_t> result(buf, buf + len);
  free(buf);
  return result;
}
