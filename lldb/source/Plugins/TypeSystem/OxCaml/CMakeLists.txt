set(LLVM_LINK_COMPONENTS Support)

add_lldb_library(lldbPluginTypeSystemOxCaml PLUGIN
  TypeSystemOxCaml.cpp

  LINK_LIBS
    lldbCore
    lldbSymbol
    lldbTarget
    lldbUtility
    lldbPluginSymbolFileDWARF
  LINK_COMPONENTS
    Support
  )