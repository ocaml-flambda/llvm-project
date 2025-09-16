# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Related Documentation

- **OxCaml Plugin Details**: See @lldb/source/Plugins/Language/OxCaml/CLAUDE.md for in-depth plugin architecture, implementation details, and DWARF structure documentation.
- **TypeSystem Implementation**: See @lldb/source/Plugins/TypeSystem/OxCaml/CLAUDE.md for TypeSystem-specific guidance and implementation notes.

## Project Overview

This is a fork of the LLVM project focused on adding LLDB support for the OxCaml language. The primary goal is to create a comprehensive LLDB plugin that enables debugging of OxCaml programs with native understanding of OxCaml types, values, and runtime structures.

## Build System

This project uses CMake with Ninja as the build generator.

### Initial Configuration

Configure the build with CMake from the repository root:

```bash
cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_INSTALL_PREFIX=_install \
  -DLLVM_ENABLE_PROJECTS="clang;lldb" \
  -DLLVM_TARGETS_TO_BUILD="host" \
  -DLLVM_ENABLE_ASSERTIONS=ON
```

For release builds, use `-DCMAKE_BUILD_TYPE=Release` instead.

### Building

Build the entire project:
```bash
ninja -C build
```

Build only LLDB:
```bash
ninja -C build lldb
```

Build specific LLDB components:
```bash
ninja -C build lldb-server
ninja -C build liblldb
```

### Installation

Install to the `_install` directory:
```bash
ninja -C build install
```

## Testing

### Running LLDB Tests

Run all LLDB tests:
```bash
ninja -C build check-lldb
```

Run LLDB unit tests:
```bash
ninja -C build LLDBUnitTests
```

Run specific test suites:
```bash
# API tests
cd build && python ../lldb/test/API/dotest.py

# Shell tests
ninja -C build check-lldb-shell
```

### Running Individual Tests

Run a specific test file:
```bash
cd build && python ../lldb/test/API/dotest.py -p TestBreakpoint.py
```

## OCaml Value Types vs Unboxed Types

The OxCaml compiler emits two distinct categories of base types in DWARF debug information, which require different handling in the LLDB plugin.

### Boxed OCaml Values (ocaml_value)

**DWARF Representation:**
- Base type: `ocaml_value` (8 bytes, `DW_ATE_signed`)
- Type aliases: `int @ value`, `bool @ value`, `string @ value`, etc.
- All regular OCaml types use this representation through typedefs

**Runtime Representation:**
- Tagged pointer system with LSB indicating immediate vs heap values
- Immediate values: `(value << 1) | 1` (e.g., integers, booleans, characters)
- Heap pointers: 8-byte aligned addresses to heap blocks (LSB = 0)

**Examples:**
```ocaml
let x : int = 42        (* ocaml_value via "int @ value" typedef *)
let b : bool = true     (* ocaml_value via "bool @ value" typedef *)
let s : string = "hi"   (* ocaml_value via "string @ value" typedef *)
let f : float = 3.14    (* ocaml_value via "float @ value" typedef *)
```

### Unboxed Primitive Types

**DWARF Representation:**
- Direct base types with specific encodings
- `float#` → 8-byte base type (`DW_ATE_float`) with `@ float64` annotation
- `int32#` → 4-byte base type (`DW_ATE_signed`) with `@ bits32` annotation
- `int64#` → 8-byte base type (`DW_ATE_signed`) with `@ bits64` annotation
- No tagged pointer wrapper - stored directly

**Runtime Representation:**
- Native machine representation without tagging overhead
- Floats stored as IEEE 754 binary formats
- Integers stored in two's complement format
- No heap allocation required

**Examples:**
```ocaml
let x : float# = #3.14      (* 8-byte IEEE 754 double *)
let i : int32# = #42l       (* 4-byte signed integer *)
let j : int64# = #1000L     (* 8-byte signed integer *)
```

### Type Annotations

OxCaml uses specific type name suffixes to distinguish representations:

- `@ value` - Boxed OCaml values using `ocaml_value` base type
- `@ float64` - Unboxed 64-bit IEEE 754 floats
- `@ float32` - Unboxed 32-bit IEEE 754 floats
- `@ bits64` - Unboxed 64-bit signed integers
- `@ bits32` - Unboxed 32-bit signed integers
- `@ word` - Unboxed native-sized integers
- `@ bits16` - Unboxed 16-bit signed integers
- `@ bits8` - Unboxed 8-bit signed integers

### Mixed Data Structures

Complex types can combine both categories:

```ocaml
(* Record with mixed field types *)
type mixed = {
  boxed: int;        (* ocaml_value *)
  unboxed: float#    (* 8-byte IEEE 754 *)
}

(* Unboxed tuple - all elements unboxed *)
type unboxed_tuple = #(float# * int32#)

(* Unboxed record - all fields unboxed *)
type unboxed_record = #{ x: float#; y: int32# }
```

### LLDB Plugin Implications

**Type System:**
- Must handle both `ocaml_value` and native base types
- Create different `OxCamlType` subclasses for each category
- Parse type annotations to determine representation

**Value Formatting:**
- Boxed values: Decode tagged representation (shift right for immediates, dereference for heap pointers)
- Unboxed values: Display directly using native format
- Mixed structures: Apply appropriate decoding per field

**Memory Access:**
- Boxed values: 8-byte reads, tag bit examination
- Unboxed values: Read exact type size (1, 2, 4, or 8 bytes)
- Heap dereferencing: Account for OCaml block headers when following pointers

## LLDB Plugin Architecture

### Language Plugin Structure

LLDB language plugins are located in `lldb/source/Plugins/Language/`. Each language has its own subdirectory with:

- `<Language>Language.cpp/h` - Main language plugin implementation
- `CMakeLists.txt` - Build configuration
- Additional support files for formatters, type systems, etc.

Key existing language plugins for reference:
- `CPlusPlus/` - C++ language support
- `ObjC/` - Objective-C language support
- `ClangCommon/` - Common utilities for Clang-based languages

### TypeSystem Integration

LLDB uses TypeSystem plugins in `lldb/source/Plugins/TypeSystem/` to understand language-specific type information. The OxCaml plugin will need integration with the Clang TypeSystem or a custom TypeSystem implementation.

### Key Integration Points

1. **Language Recognition** - Register OxCaml file extensions and language identification
2. **Value Formatting** - Custom formatters for OxCaml types (variants, records, lists, etc.)
3. **Expression Evaluation** - Support for evaluating OxCaml expressions in debugger
4. **Frame Variable Display** - Pretty-printing of OxCaml variables
5. **Type Synthesis** - Understanding OxCaml's boxed/unboxed value representation

## Development Workflow

### Incremental Building

For faster iteration during plugin development:

```bash
# Build only the language plugins
ninja -C build LLDBLanguagePlugins

# Build and install just LLDB
ninja -C build lldb && ninja -C build install-lldb
```

### Testing the Plugin

Launch LLDB with the new plugin:
```bash
./build/bin/lldb
# or from install
./_install/bin/lldb
```

Test with OxCaml binaries:
```bash
lldb ./path/to/oxcaml_program
(lldb) target create ./path/to/oxcaml_program
(lldb) breakpoint set --name main
(lldb) run
```

### Debugging LLDB Itself

Debug LLDB while developing the plugin:
```bash
lldb ./build/bin/lldb
(lldb) run ./path/to/test_program
```

## Code Structure

### Plugin Registration

Language plugins are registered in `lldb/source/Plugins/Plugins.def.in`. The OxCaml plugin will need an entry here.

### Key Source Locations

- `lldb/include/lldb/lldb-enumerations.h` - Language type enumeration
- `lldb/source/Target/Language.cpp` - Language registry and factory
- `lldb/source/Plugins/Language/` - Language plugin implementations
- `lldb/test/API/lang/` - Language-specific tests

### OxCaml-Specific Considerations

The OxCaml plugin will need to handle:
- OCaml's tagged pointer representation
- Variant type discrimination
- Block/immediate value distinction
- Custom data structure traversal (lists, arrays, records)
- Module and functor debugging support
- Exception handling and stack unwinding


## Build Artifacts

- `build/` - Ninja build files and intermediate objects
- `_install/` - Installation target (portable across machines)
- `build/bin/` - Built executables (lldb, clang, etc.)
- `build/lib/` - Built libraries

## Common Issues

### Build Issues
- Ensure sufficient disk space (LLVM builds are large)
- Use `ninja -j<N>` to limit parallel jobs if memory constrained

### Plugin Development
- **Quick rebuild**: `ninja -C build lldb` is sufficient for most changes
- Check plugin loading with LLDB's `plugin list` command
- Use LLDB's logging for debugging: `log enable lldb types` or `log enable lldb expression`
- Always add functions to compile unit with `comp_unit.AddFunction(func_sp)` after creation
- Ensure functions have valid address ranges - return nullptr if ranges are empty
- **Variable display issues**: Check raw data with DataExtractor before investigating ValueObject
- **TypeSystem configuration**: Ensure IsScalarType, IsIntegerType return true for basic types

### OxCaml Plugin Logging

The OxCaml plugin provides comprehensive logging through the "oxcaml" log channel with the following categories:

- **`types`** - DWARF type parsing
- **`functions`** - Function parsing and name resolution
- **`formatting`** - Value formatting and display operations
- **`registry`** - Type registry operations (add/lookup)
- **`verbose`** - Verbose debugging information

**Usage:**
```bash
# Enable all OxCaml logging
(lldb) log enable oxcaml all

# Enable specific categories
(lldb) log enable oxcaml types functions registry

# Enable with verbose details (shows cache hits, etc.)
(lldb) log enable -v oxcaml registry

# Log to file
(lldb) log enable -f /tmp/oxcaml.log oxcaml all

# Disable logging
(lldb) log disable oxcaml
```

### Safety Considerations
- DO NOT, under any circumstances, use raw integers such as 1 as pointers.
- Always validate that address ranges exist before creating Function objects

## Current OxCaml Plugin Status

### Fully Working
- **Breakpoints**: Set using OCaml module.function syntax (e.g., `break Test.main`)
- **Variable Display**: Integer values display correctly in decimal format
- **Type System**: Basic ocaml_value type with proper typedef support
- **Enumeration Types**: Full support for bool, char with value display
- **Records**: Display as `{field1 = value1; field2 = value2}`
- **Tuples**: Display as `(value1, value2, value3)`
- **Simple Variants**: Complete support with proper discrimination
  - Immediate: `{ Immediate[A] }`, `{ Immediate[B] }`
  - Pointer: `{ Pointer[{ C[42] }] }`, `{ Pointer[{ D[0x47b018] }] }`
- **Complex Variants**: Full support for variants with nested structures
  - Record variants: `{ Pointer[{ Record[x = 10; y = 0x47afc0] }] }`
  - Tuple variants: `{ Pointer[{ Pair[42; 0x47afe8] }] }`
  - Mixed variants: `{ Pointer[{ Mixed[a = 100; c = true; b = 2307126535107494543] }] }`
- **Parametric Types**: Generic variants work correctly
  - Option: `{ Immediate[None] }`, `{ Pointer[{ Some[42] }] }`
  - Either: `{ Pointer[{ Left[42] }] }`, `{ Pointer[{ Right[0x47ae90] }] }`
- **DWARF Support**: Complete parsing of variant parts and discriminated unions
- **Custom Attributes**: Support for `DW_AT_ocaml_offset_record_from_pointer` (0x3106)
- **Memory Management**: Intelligent size estimation for variant structures

### In Progress
- Line-based breakpoints
- Expression evaluation

### Known Limitations
- Float dereferencing shows addresses instead of values (e.g., `0x47afc0`)
- String dereferencing shows addresses instead of content (e.g., `0x47ae90`)
- Unboxed float# fields show as raw integer bits instead of float values
- Some pointer fields show addresses instead of dereferenced values

## DWARF Type Encoding

### Custom DWARF Attributes

**DW_AT_ocaml_offset_record_from_pointer (0x3106)**:
- Custom OCaml DWARF extension for pointer offset adjustment
- Specifies how many bytes to adjust when dereferencing pointers to structures
- Typical value: `-8` (to account for OCaml heap block headers)
- Applied automatically in `FormatPointer` when reading variant and record data
- Enables proper reading of heap-allocated OCaml structures

### Records and Tuples
OxCaml uses: `DW_TAG_typedef → DW_TAG_reference_type → DW_TAG_structure_type`

- **Type names**: End with `@ value` (e.g., `point @ value`, `int * string @ value`)
- **Records**: Structure members have `DW_AT_name` attributes
- **Tuples**: Structure members have no names
- **Layout**: 8-byte members, 8-byte aligned offsets

### Variants
OxCaml uses complex DWARF variant part structures:

- **Two-level discrimination**: LSB bit + constructor tags
- **Immediate variants**: Stored directly in the value (odd numbers)
- **Pointer variants**: Point to heap blocks with discriminator tags
- **DWARF encoding**: Uses `DW_TAG_variant_part` with nested `DW_TAG_variant` structures
- **Size limitation**: DWARF reports base size (8 bytes) but actual data requires more memory

### Current Display
- **Records**: `{x = 1; y = 2}`, `{a = 42; c = true; d = 0x47af08; b = 2307126535107494543}`
- **Tuples**: `(42, 1.5)`, `(value1, value2, value3)`
- **Simple Variants**: `{ Immediate[A] }`, `{ Pointer[{ C[42] }] }`
- **Complex Variants**: `{ Pointer[{ Record[x = 10; y = 0x47afc0] }] }`
- **Option Types**: `{ Immediate[None] }`, `{ Pointer[{ Some[42] }] }`
- **Either Types**: `{ Pointer[{ Left[42] }] }`, `{ Pointer[{ Right[0x47ae90] }] }`
- **Immediate values**: Show as integers with correct OCaml tagging
- **Boxed floats**: Show as hex addresses (dereferencing limitation)
- **Unboxed floats**: Show as raw integer bits (interpretation limitation)

### Testing Quick Start
```bash
# Build LLDB
ninja -C build lldb

# Test with OCaml binary using -o options (recommended)
./build/bin/lldb your_ocaml_program -o "break Module.function" -o "run" -o "frame variable" -o "quit"

# Note: Don't break on 'main' - OCaml's main is typically just initialization
# Break on your actual functions instead
```
- Run lldb commands from the top-level directory.
