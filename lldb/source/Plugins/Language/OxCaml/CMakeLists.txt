add_lldb_library(lldbPluginOxCamlLanguage PLUGIN
  LLDBFormatHelpersForOxCaml.cpp
  OxCamlLanguage.cpp
  OxCamlFormatters.cpp
  OxCamlFormatHelpers.cpp
  OxCamlValueFormatters.cpp
  LogChannelOxCaml.cpp

  LINK_LIBS
    lldbCore
    lldbExpression
    lldbTarget
    lldbUtility
)
