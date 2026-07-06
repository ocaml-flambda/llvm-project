(* Types shared between the debugged program (test_program.ml) and the
   external pretty-printer (printer.ml).  The printer must be linked against
   the same type definitions it demarshals at, since Marshal is not
   type-safe. *)

type point = { x : int; y : int }

type shape =
  | Circle of point * int
  | Line of point * point

let string_of_point (p : point) = Printf.sprintf "(%d, %d)" p.x p.y

let string_of_shape (s : shape) =
  match s with
  | Circle (c, r) ->
    Printf.sprintf "circle centred on %s with radius %d" (string_of_point c) r
  | Line (a, b) ->
    Printf.sprintf "line from %s to %s" (string_of_point a) (string_of_point b)
