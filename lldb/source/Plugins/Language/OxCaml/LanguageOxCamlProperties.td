include "../../../../include/lldb/Core/PropertiesBase.td"

let Definition = "language_oxcaml" in {
  def ExternalSummaryExecutable:
    Property<"external-summary-executable", "FileSpec">,
    Global,
    DefaultStringValue<"">,
    Desc<"Path to an executable used to pretty-print OCaml values. The "
    "executable is run with the OCaml type name (e.g. Env.t) as its only "
    "argument and receives the value, marshalled in OCaml's Marshal wire "
    "format, on its standard input. If it recognizes the type it must print "
    "a rendering of the value on its standard output and exit with code 0; "
    "any other exit code makes LLDB fall back to the built-in OCaml "
    "formatter. When empty, the built-in formatter is always used.">;
}
