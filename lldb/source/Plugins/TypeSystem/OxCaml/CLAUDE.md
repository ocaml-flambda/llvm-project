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