# OxCaml LLDB Plugin - CLAUDE.md

This file provides guidance for Claude Code when working with the OxCaml LLDB plugin.

## Overview

The OxCaml plugin adds OCaml debugging support to LLDB. It enables LLDB to understand OCaml's type system, parse DWARF debug information from OCaml binaries, and display OCaml values in a meaningful way.

## Architecture

### Component Overview

The plugin consists of three interconnected components:

1. **Language Plugin** (`OxCamlLanguage`) - Entry point for language-specific behavior
2. **TypeSystem Plugin** (`TypeSystemOxCaml`) - Manages type information and creates DWARF parser
3. **DWARF Parser** (`DWARFASTParserOxCaml`) - Extracts type information from debug data

### Detailed Component Flow

When LLDB loads an OCaml binary with debug information:

1. **Binary Loading Phase**
   - LLDB reads the DWARF debug sections from the binary
   - Encounters `DW_LANG_OCaml` (0x1b) language identifier in compilation units
   - Triggers TypeSystem creation for OCaml language

2. **TypeSystem Initialization**
   - `SymbolFileDWARF` calls `TypeSystem::CreateInstance(eLanguageTypeOCaml)`
   - `TypeSystemOxCaml::CreateInstance` creates a new TypeSystem instance
   - TypeSystem is cached and reused for all OCaml types in that module

3. **DWARF Parsing Phase**
   - `SymbolFileDWARF` requests DWARF parser via `TypeSystemOxCaml::GetDWARFParser()`
   - `DWARFASTParserOxCaml` instance is created lazily on first request
   - Parser processes DWARF DIEs (Debug Information Entries) for types

4. **Type Creation**
   - `DWARFASTParserOxCaml::ParseTypeFromDWARF` receives DWARF DIEs
   - Creates LLDB `Type` objects with `CompilerType` wrapping the TypeSystem
   - Type objects are cached in SymbolFileDWARF's type list

5. **Value Display**
   - When displaying variables, LLDB queries `TypeSystemOxCaml::GetTypeName()`
   - Type name "ocaml_value" is matched against registered formatters
   - Language plugin's formatter is invoked to display the value

## Component Interactions

### How Changes Propagate

**TypeSystem Changes:**
- Modifying `GetTypeName()` affects how types are displayed and which formatters match
- Changing `GetBitSize()` impacts memory reads and value extraction
- Altering `GetTypeForFormatters()` affects the CompilerType used in Type objects

**DWARF Parser Changes:**
- Adding support for new DWARF tags enables parsing of more complex types
- Modifying type creation logic affects all subsequent type lookups
- Changes to type size or encoding impact value interpretation

**Language Plugin Changes:**
- Adding formatters enables custom display for specific type names
- Modifying formatter logic changes how values appear in debugger output
- Category enable/disable affects whether formatters are applied

### Critical Connection Points

1. **Type Name Matching**: TypeSystem must return consistent type names that match formatter registrations
2. **CompilerType Creation**: Parser must create valid CompilerType objects for Type construction
3. **Language Identification**: File extensions and language type must be correctly recognized

## Current Implementation

### Working Features
- Plugin registration and initialization
- OCaml source file recognition (`.ml`, `.mli`)
- Basic DWARF parsing for `DW_TAG_base_type`
- Type creation with "ocaml_value" name
- Simple value formatter for immediate integers and pointers

### Known Limitations
- GetTypeForFormatters returns nullptr (causes "void" display in some contexts)
- Only DW_TAG_base_type is parsed
- No support for complex OCaml types (variants, records, lists, arrays)
- Limited type introspection capabilities

## File Structure

```
lldb/source/Plugins/
├── Language/OxCaml/
│   ├── OxCamlLanguage.h          # Language plugin interface
│   ├── OxCamlLanguage.cpp        # Formatters and language behavior
│   └── CMakeLists.txt
├── TypeSystem/OxCaml/
│   ├── TypeSystemOxCaml.h       # Type system interface
│   ├── TypeSystemOxCaml.cpp     # Type operations and DWARF parser creation
│   └── CMakeLists.txt
└── SymbolFile/DWARF/
    ├── DWARFASTParserOxCaml.h   # DWARF parser interface
    └── DWARFASTParserOxCaml.cpp # DWARF DIE processing
```

## Building and Testing

### Build Commands
```bash
# Quick rebuild of OxCaml components only
ninja -C build lldbPluginLanguageOxCaml lldbPluginTypeSystemOxCaml

# Full LLDB rebuild
ninja -C build lldb

# Build and run tests
ninja -C build check-lldb
```

### Testing with OCaml Binaries


Test the plugin:
```bash
# Run LLDB with stderr visible for debug output
./build/bin/lldb test_program 2>&1 | grep OxCaml

# In LLDB session
(lldb) target create test_program
(lldb) image lookup -t ocaml_value
(lldb) breakpoint set --name main
(lldb) run
(lldb) frame variable
```

### Debug Output

The implementation includes fprintf statements for debugging. Expected output:
```
OxCaml: TypeSystemOxCaml constructor called
OxCaml: GetDWARFParser called
OxCaml: ParseTypeFromDWARF called, tag=0x24, name=ocaml_value
OxCaml: Processing DW_TAG_base_type
OxCaml: GetTypeForFormatters called with type=0x0
OxCaml: Creating Type with name=ocaml_value
```

## OCaml Value Representation

OCaml uses a uniform value representation with tagged pointers:

### Immediate Values (LSB = 1)
- Integers: actual_value = (tagged_value >> 1)
- Characters, booleans, and other small values

### Heap Pointers (LSB = 0)
- Point to heap-allocated blocks
- Special case: 0x0 represents unit value ()
- Block header contains size and tag information

### Memory Layout
```
Immediate: [63-1 bits: value][1 bit: 1]
Pointer:   [63-1 bits: address][1 bit: 0]
```

## Development Guidelines

### Adding New Functionality

1. **Start Minimal**: Implement methods with simple default returns first
2. **Add Debug Output**: Use fprintf to verify code paths are executed
3. **Test Incrementally**: Build and test after each change
4. **Follow Patterns**: Match existing code style and conventions

### Important Methods to Implement

**TypeSystem Core Methods:**
- `ParseTypeFromDWARF` - Convert DWARF DIEs to Type objects
- `GetTypeForFormatters` - Create CompilerType for formatter system
- `GetTypeName`/`GetDisplayTypeName` - Provide consistent type names
- `GetBitSize` - Return size for memory operations
- `GetEncoding` - Specify how values are encoded

**DWARF Parser Methods:**
- `ParseTypeFromDWARF` - Main entry point for type parsing
- `ParseTypeModifier` - Handle type qualifiers
- `CompleteTypeFromDWARF` - Fill in type details

### Methods That Can Stay Minimal

Most TypeSystem query methods can return simple defaults:
- Boolean queries → `false`
- Type creation → `CompilerType()`
- Counts → `0` or `-1`
- Names → `ConstString()`

## Troubleshooting

### Type Shows as "void"
**Problem**: Type displays as "void" instead of "ocaml_value"
**Cause**: GetTypeForFormatters returns nullptr
**Solution**: Implement proper opaque type pointer management

### Formatter Not Applied
**Problem**: Values show raw hex instead of formatted output
**Checks**:
1. TypeSystem returns "ocaml_value" from GetTypeName
2. Language plugin registers formatter for exact match "ocaml_value"
3. Formatter category is enabled
4. GetFormatters() is called during plugin initialization

### DWARF Parser Not Called
**Problem**: ParseTypeFromDWARF never executes
**Checks**:
1. Binary contains DW_LANG_OCaml (0x1b) in DWARF info
2. TypeSystem is registered for eLanguageTypeOCaml
3. GetDWARFParser() returns valid parser instance
4. SupportsLanguage() returns true for OCaml

### Build Errors
**Problem**: Undefined symbols or missing methods
**Solution**: Implement all pure virtual methods, even with minimal stubs

## Safety Considerations

- **Pointer Safety**: Never use arbitrary integers as pointers
- **Null Handling**: Use nullptr for unimplemented features until properly designed
- **Error Handling**: Return appropriate error values rather than crashing
- **Memory Management**: Use smart pointers (TypeSP, CompilerType) consistently
