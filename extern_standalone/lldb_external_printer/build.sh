#!/bin/sh
# Build the example debuggee and the external pretty-printer.
set -e
OCAMLOPT=${OCAMLOPT:-/Users/mark/dev/oxcaml2/_install/bin/ocamlopt}
cd "$(dirname "$0")"

"$OCAMLOPT" -g -c test_types.ml
"$OCAMLOPT" -g -o test_program test_types.cmx test_program.ml
"$OCAMLOPT" -o printer test_types.cmx printer.ml

echo "Built: test_program (debuggee) and printer (external pretty-printer)"
