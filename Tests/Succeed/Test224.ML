(* Saving a state copies the heap.  The copy must not recurse on the C stack
   once per level of a deeply nested value: a million-deep chain through a
   first field overflowed the stack and crashed.  One child poly saves the
   chain and another loads it back and checks every level, so that a crash
   fails the test rather than the test run. *)
case #lookupStruct (PolyML.globalNameSpace) "Unix" of
    SOME _ => ()
|   NONE => raise NotApplicable;

val stateFile = OS.FileSys.tmpName ();

(* Run a script in a child poly that exits with an error on any exception. *)
fun runChild (script: string): bool =
let
    val proc: (TextIO.instream, TextIO.outstream) Unix.proc =
        Unix.execute(CommandLine.name(), ["--error-exit"])
    val toChild = Unix.textOutstreamOf proc
    val () = TextIO.output(toChild, script)
    val () = TextIO.closeOut toChild
    val fromChild = Unix.textInstreamOf proc
    val _ = TextIO.inputAll fromChild
    val () = TextIO.closeIn fromChild
in
    OS.Process.isSuccess(Unix.reap proc)
end;

val chain = "datatype t = L | N of t * (int * int);\n";

val save =
    chain ^
    "fun build 0 acc = acc | build n acc = build (n-1) (N (acc, (n, n+1)));\n\
    \val deep = build 1000000 L;\n\
    \val () = PolyML.SaveState.saveState \"" ^ String.toString stateFile ^ "\";\n\
    \val () = OS.Process.exit OS.Process.success;\n";

(* The outermost level holds (1, 2), the innermost (1000000, 1000001). *)
val check =
    chain ^
    "val () = PolyML.SaveState.loadState \"" ^ String.toString stateFile ^ "\";\n\
    \fun levels (L, k) = k - 1\n\
    \  | levels (N (x, (a, b)), k) =\n\
    \        if a = k andalso b = k + 1 then levels (x, k + 1) else raise Fail \"wrong level\";\n\
    \val () = OS.Process.exit\n\
    \    (if levels (deep, 1) = 1000000 then OS.Process.success else OS.Process.failure);\n";

val saved = runChild save;
val loaded = saved andalso runChild check;
val () = OS.FileSys.remove stateFile handle OS.SysErr _ => ();
if saved then () else raise Fail "saving a deeply nested value failed";
if loaded then () else raise Fail "the saved deeply nested value did not load back intact";
