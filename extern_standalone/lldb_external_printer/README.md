# External pretty-printer for OxCaml values in lldb

lldb can hand an OCaml value to a user-provided executable for
pretty-printing.  The value is *marshalled out of the debugged process* by
lldb itself (`OxCamlMarshal.cpp`, an adaptation of the runtime's `extern.c`
whose memory reads go through the debugger), then piped to the executable,
which demarshals it with the ordinary `Marshal` module and prints whatever
rendering it likes.

## Protocol

The executable is run once per value:

- `argv.(1)`: the OCaml type name from the debug info, with the layout
  annotation stripped (`"Env.t @ value"` → `"Env.t"`).
- stdin: the value in `Marshal` wire format (read it with
  `Marshal.from_channel`; open stdin in binary mode first).
- If the type is recognized: print the rendering on stdout and exit 0.
  Everything written to stdout becomes the summary lldb displays.
- Otherwise: exit nonzero.  lldb falls back to its built-in OCaml
  formatter.  stderr is discarded by lldb.

lldb kills the printer if it takes more than 10 seconds.

## Limitations

Values that cannot be marshalled fall back to the built-in formatter
automatically: closures, continuations, custom blocks (`Int32.t`, `Int64.t`,
`Nativeint.t`, `Bigarray`, ...), abstract and mixed blocks, unboxed
primitives (`float#`, `int64#`, ...), values optimized into DWARF implicit
pointers, and any value whose object graph contains unreadable memory.
The marshalled output is capped (64 MiB) so that a corrupt value cannot
wedge the debugger.

Marshal is not type-safe: the printer must be linked against exactly the
type definitions the debuggee uses, and must only demarshal at the type
named in `argv.(1)`.

## Building the example

```sh
./build.sh    # uses /Users/mark/dev/oxcaml2/_install/bin/ocamlopt
```

This produces `test_program` (the debuggee) and `printer` (the external
pretty-printer, which recognizes `Test_types.point` and `Test_types.shape`).

## Using it

The recommended route is the OxCaml plugin setting, which keeps the
built-in formatter as a fallback:

```
(lldb) settings set plugin.oxcaml.display.external-summary-executable \
           /path/to/lldb_external_printer/printer
(lldb) b Test_program.process_point
(lldb) run
(lldb) frame variable
```

Put the `settings set` line in `~/.lldbinit` to make it permanent.

To see what lldb sends the printer (including the exact type name passed as
`argv.(1)` — adjust `printer.ml`'s matching to taste):

```
(lldb) log enable oxcaml formatting
```

There is also a generic, language-independent mechanism:

```
(lldb) type summary add ocaml_value --summary-executable /path/to/printer
```

This registers the executable through lldb's ordinary type-summary
machinery (it works for any language; for OCaml values the input is the
marshalled value, for other languages the value's raw bytes).  Note that a
summary added this way lives in the "default" category, which takes
precedence over the OxCaml plugin's category — so if the printer rejects a
type, lldb shows the plain value rather than falling back to the built-in
OCaml formatter.  Prefer the plugin setting for everyday use.
