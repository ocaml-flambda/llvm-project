(* Example external pretty-printer for lldb's OxCaml support.

   Protocol (see lldb's OxCamlExternalPrinter.cpp):
   - argv.(1) is the OCaml type name as recorded in the debug info, with the
     layout annotation stripped (e.g. "Test_types.point").
   - The value arrives on stdin in OCaml's Marshal wire format.
   - If the type is recognized, print a rendering of the value on stdout and
     exit with code 0.  The whole of stdout becomes the summary lldb shows.
   - Otherwise exit with a nonzero code; lldb then falls back to its
     built-in OCaml formatter.

   CAUTION: Marshal.from_channel is not type-safe.  Only read the value at
   the type the debugger reported, and keep this printer linked against the
   same type definitions as the program being debugged.

   The exact spelling of the type name depends on the DWARF the compiler
   emitted.  To see what lldb passes, run in lldb:
     log enable oxcaml formatting
   and look for the "external printer: running ..." line.  The [matches]
   helper below is deliberately forgiving while experimenting. *)

let matches type_name candidates =
  List.exists
    (fun candidate ->
      String.equal type_name candidate
      || (* Accept module-qualified spellings, e.g. "Test_types.point" for
            candidate "point", and stamped spellings such as "point/123". *)
      (let l = String.length type_name and lc = String.length candidate in
       (l > lc && String.sub type_name (l - lc - 1) (lc + 1) = "." ^ candidate)
       || (l > lc
           && String.sub type_name 0 (lc + 1) = candidate ^ "/"))
      )
    candidates

let () =
  if Array.length Sys.argv < 2 then exit 2;
  let type_name = Sys.argv.(1) in
  set_binary_mode_in stdin true;
  if matches type_name [ "point"; "Test_types.point" ] then begin
    let (p : Test_types.point) = Marshal.from_channel stdin in
    print_string (Test_types.string_of_point p)
  end
  else if matches type_name [ "shape"; "Test_types.shape" ] then begin
    let (s : Test_types.shape) = Marshal.from_channel stdin in
    print_string (Test_types.string_of_shape s)
  end
  else
    (* Not a type this printer knows: tell lldb to use its built-in
       formatter. *)
    exit 1
