# LLDB Changes for OxCaml Support

This document tracks changes made to LLDB core that are suitable for upstream contribution.

## 1. Add OxCaml DWARF Parser to Kind Enum

**Status**: Ready for upstream

### Changes Required:
Extended DWARFASTParser Kind enum to include OxCaml parser

### Code Changes:

**lldb/source/Plugins/SymbolFile/DWARF/DWARFASTParser.h**:
```cpp
enum class Kind { 
  DWARFASTParserClang, 
  DWARFASTParserOxCaml  // Added for OxCaml support
};
```

## 2. Type Display in Function Arguments

**Status**: Ready for upstream

### Purpose:
Allow language plugins to control whether types are shown with function arguments in frame display. This enables OCaml to show `x=42 : int @ value` instead of just `x=42`.

### Code Changes:

**lldb/include/lldb/Target/Language.h**:
```cpp
class Language : public PluginInterface {
  // ... existing methods ...
  
  /// Returns true if types should be shown with function arguments in frame display
  /// Default is false to maintain existing behavior for all languages
  virtual bool ShouldShowTypesInFunctionArguments() const { return false; }
};
```

**lldb/source/Core/FormatEntity.cpp** (in `PrettyPrintFunctionArguments`):
```cpp
// Around line 2639, after getting var_value_sp
bool show_types = false;
if (var_value_sp) {
  lldb::LanguageType lang_type = var_value_sp->GetObjectRuntimeLanguage();
  if (lang_type == lldb::eLanguageTypeUnknown)
    lang_type = var_value_sp->GetPreferredDisplayLanguage();
  if (Language *language_plugin = Language::FindPlugin(lang_type))
    show_types = language_plugin->ShouldShowTypesInFunctionArguments();
}

// Around line 2655, when printing the value
if (var_value_sp->GetError().Success()) {
  if (!var_representation.empty()) {
    out_stream.Printf("%s=%s", var_name, var_representation.str().c_str());
    
    // Append type information if requested and available
    if (show_types) {
      ConstString type_name = var_value_sp->GetDisplayTypeName();
      if (type_name)
        out_stream.Printf(" : %s", type_name.GetCString());
    }
  }
}
```