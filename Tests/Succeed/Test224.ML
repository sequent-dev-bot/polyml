(* Saving a state copies the heap.  The copy must not recurse on the C stack
   once per level of a deeply nested value: a million-deep chain through a
   first field overflowed the stack and crashed.  This runs in a child poly
   so that a crash fails the test rather than the test run. *)
case #lookupStruct (PolyML.globalNameSpace) "Unix" of
    SOME _ => ()
|   NONE => raise NotApplicable;

val stateFile = OS.FileSys.tmpName ();

val script =
    "datatype t = L | N of t * (int * int);\n\
    \fun build 0 acc = acc | build n acc = build (n-1) (N (acc, (n, n+1)));\n\
    \val deep = build 1000000 L;\n\
    \val () = PolyML.SaveState.saveState \"" ^ String.toString stateFile ^ "\";\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val proc: (TextIO.instream, TextIO.outstream) Unix.proc = Unix.execute(CommandLine.name(), []);
val () = TextIO.output(Unix.textOutstreamOf proc, script);
val () = TextIO.closeOut(Unix.textOutstreamOf proc);
val _ = TextIO.inputAll(Unix.textInstreamOf proc);
val saved = OS.Process.isSuccess(Unix.reap proc);
val () = OS.FileSys.remove stateFile handle OS.SysErr _ => ();
if saved then () else raise Fail "saving a deeply nested value failed";
