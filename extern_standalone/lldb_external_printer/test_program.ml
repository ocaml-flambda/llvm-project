(* Debuggee for exercising the external pretty-printer.  Break on
   Test_program.process_point or Test_program.process_shape and look at the
   parameters with "frame variable". *)

open Test_types

let process_point (p : point) =
  Printf.printf "processing %s\n%!" (string_of_point p);
  p.x + p.y

let process_shape (s : shape) =
  match s with
  | Circle (c, r) -> c.x + c.y + r
  | Line (a, b) -> a.x + b.x

let () =
  let p = { x = 3; y = 4 } in
  let n = process_point p in
  let c = Circle (p, 10) in
  let l = Line (p, { x = 7; y = 1 }) in
  let m = process_shape c + process_shape l in
  Printf.printf "results: %d %d\n" n m
