add_lldb_library(lldbPluginOxCamlLanguage PLUGIN
  OxCamlLanguage.cpp
  OxCamlFormatters.cpp
  
  LINK_LIBS
    lldbCore
    lldbTarget
    lldbUtility
)