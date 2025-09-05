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

## Type Name to Formatter Matching (Critical)

**Key Insight**: The type name serves as a universal format specifier, not a descriptive name. All OCaml types return "ocaml_value" from `GetTypeName()`, and a single formatter handles all type classes dynamically.

### How It Works

1. **TypeSystem Side**:
   - All OCaml types return "ocaml_value" from `GetTypeName()`
   - This acts as a universal format specifier for the formatter system
   - `GetDisplayTypeName()` returns the actual DWARF type name (e.g., "int @ value") for display in frame variables
   - Actual type information is preserved in the OxCamlType class hierarchy

2. **Language Plugin Side**:
   - Single formatter registered for "ocaml_value"
   - Formatter accesses the actual type via CompilerType's opaque pointer
   - Dispatches formatting based on the OxCamlType class (Base, Typedef, Enum)

3. **Implementation**:
   ```cpp
   // In TypeSystemOxCaml::GetTypeName:
   ConstString TypeSystemOxCaml::GetTypeName(lldb::opaque_compiler_type_t type, bool BaseOnly) {
     if (auto* ocaml_type = static_cast<OxCamlType*>(type))
       return ConstString("ocaml_value");  // Universal format specifier
     return ConstString();
   }

   // Single formatter registration in OxCamlLanguage:
   g_category->AddTypeSummary(
       "ocaml_value",  // All OCaml types match this
       eFormatterMatchExact,
       TypeSummaryImplSP(new CXXFunctionSummaryFormat(...))
   );

   // Formatter examines actual type class:
   auto* oxcaml_type = static_cast<OxCamlType*>(compiler_type.GetOpaqueQualType());
   while (oxcaml_type->GetKind() == OxCamlType::Typedef) {
     // Resolve through typedefs
     oxcaml_type = static_cast<OxCamlTypedefType*>(oxcaml_type)->GetUnderlyingType();
   }
   if (oxcaml_type->GetKind() == OxCamlType::Enum) {
     // Display enum value by name
   }
   ```

### Architectural Benefits

- **Single Registration Point**: One formatter handles all OCaml types
- **Dynamic Dispatch**: Formatter examines actual type class at runtime
- **Clean Extensibility**: New type classes added without changing registration
- **Type Information Preserved**: Full type details available via opaque pointer

### Future Design Pattern

When adding support for more OCaml types:
1. Extend the OxCamlType hierarchy (e.g., OxCamlRecordType, OxCamlVariantType)
2. Add handling in the single formatter's dispatch logic
3. No need for additional formatter registrations
4. Type name remains "ocaml_value" for all types

## Current Implementation

### Working Features
- Plugin registration and initialization
- OCaml source file recognition (`.ml`, `.mli`)
- Complete DWARF parsing for:
  - `DW_TAG_base_type` - OCaml base types
  - `DW_TAG_subprogram` - Functions with linkage names
  - `DW_TAG_typedef` - Type aliases (e.g., "int @ value")
  - `DW_TAG_formal_parameter` - Function parameters
  - `DW_TAG_structure_type` - Records and complex structures
  - `DW_TAG_variant_part` - Variant discriminated unions
  - `DW_TAG_variant` - Individual variant cases
  - `DW_TAG_enumeration_type` - OCaml enumerations
- Type creation with proper `OxCamlType` representation
- Functional type pointer management through TypeSystem
- **Variable Display**: Variables show correct values for all immediate types (integers, bools, chars)
- **Enum Support**: Full support for OCaml enumerations with proper value display
- **Breakpoints**: Full support for OCaml module.function syntax
- **Value Formatting**: Direct data extraction bypassing ValueObject API issues
- **Simple Variants**: Complete support for variants like `A | B | C of int`
  - Immediate variants: `{ Immediate[A] }`
  - Pointer variants: `{ Pointer[{ C[42] }] }`
- **Complex Variants**: Full support for variants with records and tuples
  - Record variants: `{ Pointer[{ Record[x = 10; y = 0x47afc0] }] }`
  - Tuple variants: `{ Pointer[{ Pair[42; 0x47afe8] }] }`
  - Mixed record variants with unboxed fields
- **Parametric Variants**: Option types, Either types, custom generic variants
  - Option: `{ Immediate[None] }`, `{ Pointer[{ Some[42] }] }`
  - Either: `{ Pointer[{ Left[42] }] }`, `{ Pointer[{ Right[0x47ae90] }] }`
- **Records**: Display as `{field1 = value1; field2 = value2}`
- **Tuples**: Display as `(value1, value2, value3)`
- **Custom DWARF Attributes**: Support for `DW_AT_ocaml_offset_record_from_pointer` (0x3106)
- **Advanced Memory Management**: Intelligent size estimation for variant structures
- **Two-Level Discrimination**: Proper OCaml immediate/pointer + constructor handling
- Critical TypeSystem methods properly configured:
  - `IsScalarType` returns true
  - `IsIntegerType` returns true with unsigned
  - `CanPassInRegisters` returns true
  - `GetTypeClass` returns `eTypeClassBuiltin`

### Known Limitations
- Line-based breakpoints not yet supported (need more DWARF parsing)
- Float dereferencing shows addresses instead of values
- String dereferencing shows addresses instead of content
- Unboxed float# fields show as raw integer bits
- ValueObject's GetValueAsUnsigned() doesn't work (we use raw data instead)

## Formatter Architecture

The OxCaml formatter uses a single-formatter, multi-type dispatch pattern:

### Type Dispatch Process
1. **Type Resolution**: Formatter receives a ValueObject with a CompilerType
2. **Type Extraction**: Gets the OxCamlType via `compiler_type.GetOpaqueQualType()`
3. **Typedef Resolution**: Follows typedef chains to reach the actual type
4. **Type-Based Display**: Formats value based on the resolved type class

### Supported Type Classes
- **OxCamlBaseType**: Display as integer (immediate value >> 1)
- **OxCamlTypedefType**: Transparently resolved to underlying type
- **OxCamlEnumType**: Display enumerator name if found, else numeric value

### Example Flow
```
Variable "x" of type "bool @ value"
  → GetTypeName() returns "ocaml_value"
  → Formatter matches and is invoked
  → Extracts OxCamlTypedefType from CompilerType
  → Resolves to underlying OxCamlEnumType
  → Looks up value in enum's enumerators
  → Displays "true" or "false"
```

## Type System Design

The TypeSystemOxCaml implements a clean type hierarchy:

### OxCamlType Class Hierarchy
```cpp
OxCamlType (abstract base)
├── OxCamlBaseType      // The fundamental "ocaml_value" type
├── OxCamlTypedefType   // Type aliases (e.g., "int @ value")
└── OxCamlEnumType      // Enumerations with name/value pairs
```

### Type Registry
- Types are created once and stored in `m_type_registry`
- Indexed by DWARF DIE ID for efficient lookup
- Registry owns all type instances via `std::unique_ptr`
- Non-owning pointers used for type references

### Key Design Decisions
1. **Universal Type Name**: All types return "ocaml_value" from GetTypeName()
2. **Type Information Preservation**: Actual type details accessible via opaque pointer
3. **Lazy Type Creation**: Types created on-demand during DWARF parsing
4. **Memory Safety**: Registry owns all types, preventing dangling pointers

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

Test the plugin efficiently using -o options:
```bash
# Recommended: Use -o options for quick testing
./build/bin/lldb test_program \
  -o "breakpoint set -n Module.function_name" \
  -o "run" \
  -o "frame variable" \
  -o "continue" \
  -o "frame variable" \
  -o "quit"

# Note: Avoid breaking on 'main' - OCaml's main is often just initialization
# Instead, break on your actual functions like Test.process_data
```

### Quick Iteration Workflow
1. Make code changes
2. `ninja -C build lldb` - rebuild LLDB
3. Test immediately using -o options for automation
4. No debug output needed - variables display correctly

### Interactive Testing
```bash
# For interactive exploration
./build/bin/lldb test_program

# In LLDB session
(lldb) breakpoint set -n "Module.function_name"  # Not 'main'
(lldb) run
(lldb) frame variable  # Shows variable values
(lldb) continue
```


## Established Workflow Patterns

### Adding DWARF Support for New Tags
1. Add a new case to the switch statement in `ParseTypeFromDWARF`
2. Implement a new `Parse[TagType]` helper method following the established pattern
3. Create the appropriate `OxCaml[TagType]` class in the TypeSystem hierarchy
4. Add the new helper method declaration to `DWARFASTParserOxCaml.h`
5. Update the formatter dispatch logic if needed for the new type class

### Debugging Variable Display Issues
1. First check if raw data is available via `DataExtractor`
2. If data exists but ValueObject methods fail, use raw data directly
3. Ensure TypeSystem methods return appropriate values:
   - IsScalarType, IsIntegerType for basic types
   - Proper type class and encoding

### Quick Testing Cycle
- Keep a test OCaml binary with various types ready
- Use function breakpoints (line breakpoints not yet supported)
- Test with `frame variable` to see immediate feedback
- No need for debug logging - formatters show values directly

## Custom DWARF Attributes

The OxCaml LLDB plugin supports custom DWARF attributes specific to OCaml's memory layout requirements.

### DW_AT_ocaml_offset_record_from_pointer (0x3106)

This is a custom OCaml DWARF extension that specifies pointer offset adjustments for heap-allocated structures.

**Purpose**:
- OCaml heap blocks have an 8-byte header before the actual data
- When LLDB reads a pointer to an OCaml structure, it needs to adjust the address to account for this header
- This attribute tells LLDB how many bytes to subtract from the pointer address

**Usage**:
```c
// In DWARF DIE for structure types:
DW_TAG_structure_type
  DW_AT_ocaml_offset_record_from_pointer: -8  // Typical value
  DW_AT_byte_size: 8  // Base structure size
  // ... members at offsets 8, 16, 24, etc.
```

**Implementation Details**:
- Attribute value: `0x3106` (custom extension)
- Typical value: `-8` (8-byte header offset)
- Applied in `FormatPointer` when dereferencing OCaml pointers to structures
- Enables proper reading of variant constructors and record fields

**Memory Layout Example**:
```
Heap Block:
[Header: 8 bytes]  <-- LLDB needs to read from here to determine block tag (offset 0)
[Data: Member 1]   <-- Pointer points here (offset +8)
[Data: Member 2]   <-- Next member (offset +16)
[Data: Member 3]   <-- Next member (offset +24)
```

**Note**: This is a temporary DWARF extension until the OCaml compiler provides better structure size information.

## Variant Implementation Details

The OxCaml plugin implements variant support using several helper functions:

### Key Components

1. **`ReadDiscriminatorValue`**: Reads discriminator values from memory with proper bit masking
2. **`FindActiveVariantsInStructure`**: Identifies which variants are active for given data
3. **`CalculateMinimumSizeForDiscriminators`**: Calculates minimum memory needed to read all discriminators
4. **`EstimatePointerAllocationSize`**: Estimates actual allocation size based on active variants

### Two-Pass Memory Reading

For structures with variant parts, the plugin uses a two-pass approach:

1. **First Pass**: Read minimum memory needed to analyze all discriminators
2. **Second Pass**: Calculate precise size based on active variants and read actual data

This approach optimizes memory usage while ensuring all variant data is accessible.

### Discriminator Reading

OCaml variants use sophisticated discrimination:
- **1-bit LSB**: Immediate vs pointer discrimination
- **Multi-byte tags**: Constructor identification within pointer variants
- **Nested structures**: Heap blocks contain their own discriminators

## OxCaml DWARF Emission Limitations

### Structure Size Issue

**Problem**: The OxCaml compiler emits incorrect `DW_AT_byte_size` values for variant structures.

**Details**:
- DWARF reports structure size as 8 bytes (header size only)
- Actual structures contain members at offsets 8, 16, 24, etc., requiring 32+ bytes
- This discrepancy causes memory reading issues in debuggers

**Example**:
```c
// DWARF says:
DW_TAG_structure_type
  DW_AT_byte_size: 8  // Only header size

// But members exist at:
DW_TAG_member
  DW_AT_data_member_location: 8   // Member 1
DW_TAG_member
  DW_AT_data_member_location: 16  // Member 2
DW_TAG_member
  DW_AT_data_member_location: 24  // Member 3
```

**Current Workaround**:
- `EstimatePointerAllocationSize` function calculates actual size from member offsets
- Two-pass reading approach ensures sufficient memory is read
- Conservative estimation when variant analysis is not possible

**Future Solution**:
The OxCaml compiler should emit accurate structure sizes that account for all possible variant members, eliminating the need for size estimation workarounds.

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
- `ParseTypeFromDWARF` - Main entry point for type parsing (dispatcher pattern with helper methods)
- `ParseBaseType` - Handle DW_TAG_base_type DIEs
- `ParseTypedefType` - Handle DW_TAG_typedef DIEs with recursive type resolution
- `ParseEnumType` - Handle DW_TAG_enumeration_type DIEs
- `CreateLLDBType` - Common LLDB Type object creation logic
- `ParseTypeModifier` - Handle type qualifiers
- `CompleteTypeFromDWARF` - Fill in type details

The parser uses a modular design where `ParseTypeFromDWARF` dispatches to specialized helper methods based on DWARF tag type, improving maintainability and extensibility.

### Methods That Can Stay Minimal

Most TypeSystem query methods can return simple defaults:
- Boolean queries → `false`
- Type creation → `CompilerType()`
- Counts → `0` or `-1`
- Names → `ConstString()`

## Breakpoint Usage

With the current implementation, you can set breakpoints on OCaml functions using their linkage names:

```bash
# Using the OCaml module.function syntax (linkage name)
(lldb) breakpoint set -n "Test_ocaml_debug.test_unit"
(lldb) breakpoint set -n "Module.function_name"

# Using the mangled symbol name (less preferred)
(lldb) breakpoint set -n "camlTest_ocaml_debug__test_unit_0_9_code"
```

### Current Breakpoint Status
- Functions are successfully parsed from DWARF with both symbol names and linkage names
- The DWARF parser extracts `DW_AT_linkage_name` which contains the OCaml module.function name
- Functions are indexed and can be found by name
- Breakpoint resolution finds the correct function and sets breakpoints successfully
- Breakpoints show correct source location and addresses
- **Working**: Breakpoints can be set and hit during debugging

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

## OxCaml DWARF Structure

This section documents the DWARF debug information structure emitted by the OxCaml compiler, based on analysis of compiled binaries.

### DWARF Tags Used

The OxCaml compiler emits the following DWARF tags:

- **`DW_TAG_compile_unit`**: Marks OCaml compilation units with `DW_AT_language = 27` (OCaml)
- **`DW_TAG_base_type`**: Defines "ocaml_value" as the fundamental type (8 bytes, signed)
- **`DW_TAG_subprogram`**: Represents OCaml functions with mangled names
- **`DW_TAG_typedef`**: Creates type aliases like "int @ value", "bool @ value", "string @ value"
- **`DW_TAG_enumeration_type`**: Defines enum types for bool and char
- **`DW_TAG_enumerator`**: Lists enum values with OCaml's tagged representation
- **`DW_TAG_formal_parameter`**: Function parameters with location info

### Function Naming Convention

OCaml functions appear in DWARF with specific naming patterns:

```
DW_AT_name: camlTest_ocaml_debug__test_unit_0_9_code
DW_AT_linkage_name: Test_ocaml_debug.test_unit
```

**Pattern**: `caml<Module>__<function>_<unique_id>_<unique_id>_code`
- Module name is prefixed with "caml"
- Function name follows double underscore
- Two numeric IDs before "_code" suffix
- Linkage name uses dot notation: `Module.function`

### Type Representation

All OCaml values use a uniform representation:

1. **Base Type**: "ocaml_value" (8 bytes, signed encoding)
2. **Type Aliases**: Types represented as typedefs with "@ value" suffix
   - `int @ value` → typedef to ocaml_value
   - `bool @ value` → typedef to enumeration type
   - `char @ value` → typedef to enumeration type
   - `string @ value` → typedef to ocaml_value
   - Custom types follow same pattern: `my_int @ value`

### Enumeration Encoding

OxCaml uses enumerations for discrete types with tagged values:

**Boolean Type**:
```
DW_TAG_enumeration_type (8 bytes)
  false = 0x1
  true = 0x3
```

**Character Type**:
```
DW_TAG_enumeration_type (8 bytes)
  '\000' = 0x1
  '\001' = 0x3
  '\002' = 0x5
  ...
  'a' = 0xc3
  'b' = 0xc5
  ...
```

All enum values are odd numbers following OCaml's tagged pointer scheme where odd numbers represent immediate values.

### Tagged Value Encoding

The const_value attributes in DWARF follow OCaml's runtime representation:
- Immediate integers: `(value << 1) | 1`
- Booleans: false=1, true=3 (following immediate encoding)
- Characters: ASCII value transformed to odd number
- Unit type: represented as 1 (immediate zero)

### Example DWARF Structure

For a simple OCaml function:
```ocaml
let test_int (x : int) = x
```

Generates DWARF:
```
DW_TAG_subprogram
  DW_AT_name: camlModule__test_int_1_10_code
  DW_AT_linkage_name: Module.test_int
  DW_TAG_typedef
    DW_AT_name: int @ value
    DW_AT_type: → ocaml_value
  DW_TAG_formal_parameter
    DW_AT_name: x
    DW_AT_type: → int @ value
```

## OCaml Variant DWARF Encoding

Based on analysis of compiled OCaml binaries, variants are encoded using DWARF variant parts and discriminated unions.

### Variant Structure Overview

OCaml variants like:
```ocaml
type simple_variant = A | B | C of int | D of float
```

Are encoded as:
```
DW_TAG_typedef
  DW_AT_name: "simple_variant @ value"
  DW_AT_type: → structure type

DW_TAG_structure_type (8 bytes)
  DW_TAG_variant_part
    DW_AT_discr: → discriminator member

    DW_TAG_member (discriminator)
      DW_AT_bit_size: 1
      DW_AT_artificial: true
      DW_AT_data_member_location: 0x00
      DW_AT_type: → enumeration type
      DW_AT_data_bit_offset: 0x00

    DW_TAG_variant (discr_value: 0x01)  // Immediate variants
      DW_TAG_member
        DW_AT_bit_size: 63
        DW_AT_data_member_location: 0x00
        DW_AT_type: → enum { A, B }
        DW_AT_data_bit_offset: 0x01

    DW_TAG_variant (discr_value: 0x00)  // Pointer variants
      DW_TAG_member
        DW_AT_data_member_location: 0x00
        DW_AT_type: → structure type for heap blocks
```

### Discriminator Types

Two enumeration types control variant discrimination:

1. **Boxed/Unboxed Discriminator** (1-bit):
   ```
   DW_TAG_enumeration_type
     DW_TAG_enumerator
       DW_AT_name: "Pointer"     // 0x00
       DW_AT_const_value: 0x00
     DW_TAG_enumerator
       DW_AT_name: "Immediate"   // 0x01
       DW_AT_const_value: 0x01
   ```

2. **Constructor Discriminator** (for each variant class):
   ```
   DW_TAG_enumeration_type
     DW_TAG_enumerator
       DW_AT_name: "A"
       DW_AT_const_value: 0x00
     DW_TAG_enumerator
       DW_AT_name: "B"
       DW_AT_const_value: 0x01
   ```

   ```
   DW_TAG_enumeration_type (1 byte)
     DW_TAG_enumerator
       DW_AT_name: "C"
       DW_AT_const_value: 0x00
     DW_TAG_enumerator
       DW_AT_name: "D"
       DW_AT_const_value: 0x01
   ```

### Runtime Representation

OCaml variants use a two-level discrimination scheme:

1. **LSB (Least Significant Bit)**:
   - `1` = Immediate value (constructors A, B stored directly)
   - `0` = Pointer to heap block (constructors C, D)

2. **Constructor Tag**:
   - For immediate: stored in upper 63 bits
   - For pointers: tag byte in heap block header

### Memory Layout Examples

**Immediate constructors (A, B)**:
```
Value layout: [63 bits: constructor tag][1 bit: 1]
A: 0x0000000000000001 (tag=0, immediate=1)
B: 0x0000000000000003 (tag=1, immediate=1)
```

**Pointer constructors (C of int, D of float)**:
```
Value layout: [63 bits: heap address][1 bit: 0]
Points to heap block:
  Header: [size][tag] where tag identifies C vs D
  Data: constructor payload
```

### Heap Block Structure (for pointer variants)

```
DW_TAG_structure_type (for heap blocks)
  DW_TAG_variant_part
    DW_AT_discr: → tag discriminator

    DW_TAG_variant (discr_value: 0x00)  // C of int
      DW_TAG_member
        DW_AT_byte_size: 8
        DW_AT_data_member_location: 0x08  // After header
        DW_AT_type: → ocaml_value (int)

    DW_TAG_variant (discr_value: 0x01)  // D of float
      DW_TAG_member
        DW_AT_byte_size: 8
        DW_AT_data_member_location: 0x08  // After header
        DW_AT_type: → ocaml_value (float)
```

### Implementation Implications

1. **Two-Phase Discrimination**: Check LSB first, then examine appropriate tag
2. **Type Resolution**: Follow variant part discriminators to find active member
3. **Value Extraction**: Different logic for immediate vs pointer variants
4. **Constructor Naming**: Enum names map directly to OCaml constructors

## Safety Considerations

- **Pointer Safety**: Never use arbitrary integers as pointers
- **Null Handling**: Use nullptr for unimplemented features until properly designed
- **Error Handling**: Return appropriate error values rather than crashing
- **Memory Management**: Use smart pointers (TypeSP, CompilerType) consistently
