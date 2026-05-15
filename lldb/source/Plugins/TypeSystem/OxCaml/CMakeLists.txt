set(LLVM_LINK_COMPONENTS Support)

add_lldb_library(lldbPluginTypeSystemOxCaml PLUGIN
  TypeSystemOxCaml.cpp
  OxCamlDynamicLayoutValue.cpp
  OxCamlTypes.cpp

  LINK_LIBS
    lldbCore
    lldbExpression
    lldbSymbol
    lldbTarget
    lldbUtility
    lldbPluginSymbolFileDWARF
  LINK_COMPONENTS
    Support
  )