(* A module's id doesn't depend on the order its dependencies were loaded in.
   One child poly saves modules A and B, neither depending on the other.  Two
   more load them, in opposite orders, and each saves the same module C, which
   depends on both: the two copies of C must have the same id. *)
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

val save =
    "structure A = struct val xs = List.tabulate (5000, fn i => Int.toString i ^ \"-a\") end;\n\
    \val _ = PolyML.SaveState.saveModule (" ^ quoted "A.pmo" ^ ", " ^ contentsOf "A" ^ ");\n\
    \structure B = struct val ys = List.tabulate (5000, fn i => (i, real i)) end;\n\
    \val _ = PolyML.SaveState.saveModule (" ^ quoted "B.pmo" ^ ", " ^ contentsOf "B" ^ ");\n\
    \val () = OS.Process.exit OS.Process.success;\n";

fun saveC (first, second) n =
    "val _ = PolyML.SaveState.loadModule " ^ quoted first ^ ";\n\
    \val _ = PolyML.SaveState.loadModule " ^ quoted second ^ ";\n\
    \val idA = #1 (PolyML.SaveState.getModuleInfo " ^ quoted "A.pmo" ^ ");\n\
    \val idB = #1 (PolyML.SaveState.getModuleInfo " ^ quoted "B.pmo" ^ ");\n\
    \structure C = struct val zs = ListPair.zip (A.xs, B.ys) end;\n\
    \val idC = PolyML.SaveState.saveDependentModule (" ^ quoted ("C" ^ n ^ ".pmo") ^ ", " ^ contentsOf "C" ^
    ", [(idA, " ^ quoted "A.pmo" ^ "), (idB, " ^ quoted "B.pmo" ^ ")]);\n\
    \val out = BinIO.openOut " ^ quoted ("id" ^ n) ^ ";\n\
    \val () = BinIO.output (out, idC);\n\
    \val () = BinIO.closeOut out;\n\
    \val () = OS.Process.exit OS.Process.success;\n";

fun contents name =
let val input = BinIO.openIn (file name) in BinIO.inputAll input before BinIO.closeIn input end;

val saved = runChild save andalso runChild (saveC ("A.pmo", "B.pmo") "1") andalso runChild (saveC ("B.pmo", "A.pmo") "2");
val same = saved andalso contents "id1" = contents "id2";
val () = List.app (fn f => OS.FileSys.remove (file f) handle OS.SysErr _ => ()) ["A.pmo", "B.pmo", "C1.pmo", "C2.pmo", "id1", "id2"];
val () = OS.FileSys.rmDir dir handle OS.SysErr _ => ();
if saved then () else raise Fail "saving the modules failed";
if same then () else raise Fail "the module's id depends on the order its dependencies were loaded in";
