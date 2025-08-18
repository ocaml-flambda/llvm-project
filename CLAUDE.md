# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is **LLVM-oxcaml**, a specialized fork of the LLVM project with modifications to support OCaml debugging capabilities. The primary focus is on enhancing LLDB's ability to debug and display OCaml values intelligently.

## Build System Commands

The project uses CMake with Ninja as the build system. By default, all builds are configured in the `build/` directory.

### Core Build Commands
```bash
# Build everything (project is pre-configured)
ninja -C build

# Build with specific parallel jobs
ninja -C build -j <number>

# Clean build
ninja -C build clean

# Build specific components
ninja -C build clang
ninja -C build llc
ninja -C build opt
ninja -C build lldb
```

### Testing Commands
```bash
# Run all regression tests (primary test command)
ninja -C build check-all

# Component-specific tests
ninja -C build check-llvm      # LLVM core tests
ninja -C build check-clang     # Clang compiler tests
ninja -C build check-lldb      # LLDB debugger tests
```

### Installation
By default, built binaries are available in `_install/` directory. To reinstall:
```bash
ninja -C build install
```

## OCaml Debugging Features

This repository contains OCaml-specific debugging extensions:

### Key Features

1. **OCaml Value Debugging** (`lldb/source/Core/DumpDataExtractor.cpp`)
   - New LLDB format: `eFormatOCamlValue` for intelligent value display
   - Automatic tag detection and OCaml data structure interpretation
   - Support for arrays, records, variants, and lazy values
   - Forward pointer resolution for OCaml values
   - **Critical implementation detail**: OCaml uses tagged integers encoded as `(value << 1) | 1`. The `FormatOCamlValue` function in `DumpDataExtractor.cpp` handles decoding these values. When modifying this logic, ensure proper signed/unsigned integer handling to correctly interpret the encoded values.

2. **OCaml Type System Integration** (`clang/include/clang/AST/BuiltinTypes.def`)
   - New builtin type: `eBasicTypeOCamlValue`
   - DWARF support with `DW_LANG_OCaml` language identifier
   - TypeSystem integration in LLDB for OCaml type understanding

## Development Guidelines

### Working with OCaml Debugging
- When modifying LLDB, consider OCaml value display in `DumpDataExtractor.cpp`
- OCaml value formatting improvements should handle various OCaml data structures
- Test OCaml debugging features using the built `lldb` binary

### Build Configuration
- LLVM Version: 16.0.6
- Build system: Ninja (faster than make for incremental builds)
- By default, pre-built installation available in `_install/`

### Testing OCaml Value Debugging
When making changes to OCaml value formatting in `DumpDataExtractor.cpp`:

1. **Create test OCaml files** with non-inlined functions to ensure debugger visibility:
   ```ocaml
   let[@local never][@inline never] test_function x = x
   ```

2. **Compile with debugging flags** (ask user for OCaml compiler path):
   ```bash
   <ocaml-compiler-path> -g -gno-upstream-dwarf -shape-format debugging-shapes -o test file.ml
   ```
   - `-g`: Generate debug information
   - `-gno-upstream-dwarf`: Use OCaml-specific DWARF generation
   - `-shape-format debugging-shapes`: Include shape debugging information

3. **Test in LLDB**:
   ```bash
   lldb ./test
   (lldb) b <function_name>
   (lldb) run
   (lldb) frame variable  # Check parameter display
   (lldb) memory read -f ocaml_value -s8 -c1 <address>  # Test OCaml formatting
   ```

4. **Verify comprehensive test cases**: Test various value types (integers, strings, records, arrays) and edge cases to ensure proper encoding/decoding, especially for tagged immediate values.

## Key Files for OCaml Debugging Development

### Core OCaml Debugging Support
- `lldb/source/Core/DumpDataExtractor.cpp` - OCaml value debugging and formatting
- `clang/include/clang/AST/BuiltinTypes.def` - OCaml builtin types
- `lldb/source/Plugins/TypeSystem/Clang/TypeSystemClang.cpp` - LLDB type integration

## Repository Structure
This is the standard LLVM monorepo structure with OCaml debugging enhancements primarily in LLDB components. The project includes all major LLVM components (clang, lldb, lld, polly, etc.) with focused improvements for OCaml value debugging and display.
