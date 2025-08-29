# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Primary Documentation

See the main OxCaml plugin documentation at:
`lldb/source/Plugins/Language/OxCaml/CLAUDE.md`

## TypeSystem-Specific Notes

### Include Path
The DWARFASTParserOxCaml header must be included using relative path:
```cpp
#include "../../SymbolFile/DWARF/DWARFASTParserOxCaml.h"
```

### LLVM RTTI Implementation
The TypeSystem uses LLVM's custom RTTI system:
```cpp
static char ID;  // Static member for type identification
bool isA(const void *ClassID) const override { return ClassID == &ID; }
static bool classof(const TypeSystem *ts) { return ts->isA(&ID); }
```

### CMake Dependencies
From CMakeLists.txt - the TypeSystem links against:
- lldbCore
- lldbSymbol
- lldbTarget
- lldbUtility
- lldbPluginSymbolFileDWARF

### Pure Virtual Methods
All ~39 pure virtual methods from TypeSystem base class must be implemented, even if with minimal stubs. Use simple default returns rather than llvm_unreachable for methods that might be called.

## Key Architectural Decisions

### Universal Format Specifier Pattern
The TypeSystemOxCaml uses "ocaml_value" as a universal format specifier:
- `GetTypeName()` always returns "ocaml_value" for any OCaml type
- This ensures all types match the single formatter in the Language plugin
- Actual type information is preserved in the OxCamlType hierarchy

### OxCamlType Class Hierarchy
```cpp
class OxCamlType {
  enum Kind { Base, Typedef, Enum };
  // Abstract base for all OCaml types
};

class OxCamlBaseType : public OxCamlType {
  // Represents the fundamental ocaml_value type
};

class OxCamlTypedefType : public OxCamlType {
  OxCamlType* m_underlying;  // Non-owning pointer to underlying type
  // Represents type aliases like "int @ value"
};

class OxCamlEnumType : public OxCamlType {
  std::vector<Enumerator> m_enumerators;
  // Represents enumerations with name/value pairs
};
```

### Type Registry Management
- Types are stored in `m_type_registry` indexed by DWARF DIE ID
- Registry owns all types via `std::unique_ptr`
- `GetType()` returns `std::optional<OxCamlType*>` for safe access
- `RegisterType()` adds new types to the registry

### Important: DumpTypeValue
The `DumpTypeValue` method should remain unimplemented (return false) in the TypeSystem. Value formatting is handled by the Language plugin's formatters, not the TypeSystem. The formatter accesses type information via the CompilerType's opaque pointer.