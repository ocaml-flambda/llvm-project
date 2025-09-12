add_lldb_library(lldbPluginOxCamlLanguage PLUGIN
  OxCamlLanguage.cpp
  OxCamlFormatters.cpp
  OxCamlFormatHelpers.cpp
  OxCamlValueFormatters.cpp
  LogChannelOxCaml.cpp
  
  LINK_LIBS
    lldbCore
    lldbTarget
    lldbUtility
)