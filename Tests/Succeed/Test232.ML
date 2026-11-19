(* A module exported over a state saved with saveChildWithModules refers to
   the modules that state recorded rather than copying their data.  One child
   saves module A, with a ref; another loads it and saves a state that records
   it.  A third loads the state and saves module B, which shares A's ref,
   depending on the state, and a fourth module C that shares it too, naming
   only B.  The last loads the state, B and C, sets A's ref and finds B and C
   see the change. *)
case #lookupStruct (PolyML.globalNameSpace) "Unix" of
    SOME _ => ()
|   NONE => raise NotApplicable;

val dir = OS.FileSys.tmpName ();
val () = OS.FileSys.remove dir handle OS.SysErr _ => ();
val () = OS.FileSys.mkDir dir;
fun file name = OS.Path.joinDirFile {dir = dir, file = name};
fun quoted name = "\"" ^ String.toString (file name) ^ "\"";

fun runChild (script: string): bool =
let
    val proc: (TextIO.instream, TextIO.outstream) Unix.proc =
        Unix.execute(CommandLine.name(), ["--error-exit"])
    val toChild = Unix.textOutstreamOf proc
    val () = TextIO.output(toChild, script)
    val () = TextIO.closeOut toChild
    val fromChild = Unix.textInstreamOf proc
    val output = TextIO.inputAll fromChild
    val () = TextIO.closeIn fromChild
    val ok = OS.Process.isSuccess(Unix.reap proc)
in
    if ok then () else print output;
    ok
end;

fun contentsOf s = "{structs = [\"" ^ s ^ "\"], functors = [], sigs = [], onStartup = NONE}";

val saveA =
    "structure A = struct val r = ref 3 end;\n\
    \val _ = PolyML.SaveState.saveModule (" ^ quoted "A.pmo" ^ ", " ^ contentsOf "A" ^ ");\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val saveState =
    "val _ = PolyML.SaveState.loadModule " ^ quoted "A.pmo" ^ ";\n\
    \val () = PolyML.SaveState.saveChildWithModules (" ^ quoted "st" ^ ", 0);\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val saveB =
    "val () = PolyML.SaveState.loadState " ^ quoted "st" ^ ";\n\
    \structure B = struct val r = A.r end;\n\
    \val _ = PolyML.SaveState.saveDependentModule (" ^ quoted "B.pmo" ^ ", " ^ contentsOf "B" ^
    ", PolyML.SaveState.showHierarchyWithIds ());\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val saveC =
    "val () = PolyML.SaveState.loadState " ^ quoted "st" ^ ";\n\
    \val _ = PolyML.SaveState.loadModule " ^ quoted "B.pmo" ^ ";\n\
    \structure C = struct val r = B.r end;\n\
    \val (b, _) = PolyML.SaveState.getModuleInfo " ^ quoted "B.pmo" ^ ";\n\
    \val _ = PolyML.SaveState.saveDependentModule (" ^ quoted "C.pmo" ^ ", " ^ contentsOf "C" ^
    ", [(b, " ^ quoted "B.pmo" ^ ")]);\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val useB =
    "val () = PolyML.SaveState.loadState " ^ quoted "st" ^ ";\n\
    \val _ = PolyML.SaveState.loadModule " ^ quoted "B.pmo" ^ ";\n\
    \val _ = PolyML.SaveState.loadModule " ^ quoted "C.pmo" ^ ";\n\
    \val () = A.r := 7;\n\
    \val () = if !B.r = 7 then () else raise Fail \"B has a copy of A's ref\";\n\
    \val () = if !C.r = 7 then () else raise Fail \"C has a copy of A's ref\";\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val saved = runChild saveA andalso runChild saveState andalso runChild saveB andalso
            runChild saveC;
val shared = saved andalso runChild useB;
val () = List.app (fn f => OS.FileSys.remove (file f) handle OS.SysErr _ => ()) ["A.pmo", "B.pmo", "C.pmo", "st"];
val () = OS.FileSys.rmDir dir handle OS.SysErr _ => ();
if saved then () else raise Fail "saving the modules or the state failed";
if shared then () else raise Fail "a module exported over a state with modules copied a recorded module's ref";
