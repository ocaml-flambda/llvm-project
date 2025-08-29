add_lldb_library(lldbPluginOxCamlLanguage PLUGIN
  OxCamlLanguage.cpp
  OxCamlFormatters.cpp
  LogChannelOxCaml.cpp
  
  LINK_LIBS
    lldbCore
    lldbTarget
    lldbUtility
)