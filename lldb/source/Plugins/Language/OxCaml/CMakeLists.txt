add_lldb_library(lldbPluginOxCamlLanguage PLUGIN
  OxCamlLanguage.cpp
  
  LINK_LIBS
    lldbCore
    lldbTarget
    lldbUtility
)