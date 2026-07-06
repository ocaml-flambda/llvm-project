lldb_tablegen(LanguageOxCamlProperties.inc -gen-lldb-property-defs
  SOURCE LanguageOxCamlProperties.td
  TARGET LLDBPluginLanguageOxCamlPropertiesGen)

lldb_tablegen(LanguageOxCamlPropertiesEnum.inc -gen-lldb-property-enum-defs
  SOURCE LanguageOxCamlProperties.td
  TARGET LLDBPluginLanguageOxCamlPropertiesEnumGen)

add_lldb_library(lldbPluginOxCamlLanguage PLUGIN
  LLDBFormatHelpersForOxCaml.cpp
  OxCamlLanguage.cpp
  OxCamlExternalPrinter.cpp
  OxCamlFormatters.cpp
  OxCamlFormatHelpers.cpp
  OxCamlMarshal.cpp
  OxCamlValueFormatters.cpp
  LogChannelOxCaml.cpp

  LINK_LIBS
    lldbCore
    lldbDataFormatters
    lldbExpression
    lldbTarget
    lldbUtility
)

add_dependencies(lldbPluginOxCamlLanguage
  LLDBPluginLanguageOxCamlPropertiesGen
  LLDBPluginLanguageOxCamlPropertiesEnumGen)
