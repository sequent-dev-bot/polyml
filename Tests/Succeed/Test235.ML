(* A state saved with saveChildWithModules after loading a module by a
   relative path loads from another working directory: the state records the
   module's absolute path. *)
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

val saveA =
    "structure A = struct val r = ref 3 end;\n\
    \val _ = PolyML.SaveState.saveModule (" ^ quoted "A.pmo" ^ ", {structs = [\"A\"], functors = [], sigs = [], onStartup = NONE});\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val saveState =
    "val () = OS.FileSys.chDir \"" ^ String.toString dir ^ "\";\n\
    \val _ = PolyML.SaveState.loadModule \"./A.pmo\";\n\
    \val () = PolyML.SaveState.saveChildWithModules (" ^ quoted "st" ^ ", 0);\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val loadElsewhere =
    "val () = OS.FileSys.chDir \"/\";\n\
    \val () = PolyML.SaveState.loadState " ^ quoted "st" ^ ";\n\
    \val () = if !A.r = 3 then () else raise Fail \"A changed\";\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val saved = runChild saveA andalso runChild saveState;
val loaded = saved andalso runChild loadElsewhere;
val () = List.app (fn f => OS.FileSys.remove (file f) handle OS.SysErr _ => ()) ["A.pmo", "st"];
val () = OS.FileSys.rmDir dir handle OS.SysErr _ => ();
if saved then () else raise Fail "saving the module or the state failed";
if loaded then () else raise Fail "the state didn't load from another directory";
