(* A state can't be saved over the file of a module it loaded: the save
   would replace the module the state depends on.  One child saves module A;
   another loads it and tries to save a state to A's file, which must fail
   and leave A loadable. *)
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

val overwrite =
    "val _ = PolyML.SaveState.loadModule " ^ quoted "A.pmo" ^ ";\n\
    \val refused = (PolyML.SaveState.saveChildWithModules (" ^ quoted "A.pmo" ^ ", 0); false)\n\
    \              handle OS.SysErr _ => true;\n\
    \val () = if refused then () else raise Fail \"saved over A.pmo\";\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val loadA =
    "val _ = PolyML.SaveState.loadModule " ^ quoted "A.pmo" ^ ";\n\
    \val () = if !A.r = 3 then () else raise Fail \"A changed\";\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val saved = runChild saveA;
val refused = saved andalso runChild overwrite;
val intact = refused andalso runChild loadA;
val () = OS.FileSys.remove (file "A.pmo") handle OS.SysErr _ => ();
val () = OS.FileSys.rmDir dir handle OS.SysErr _ => ();
if saved then () else raise Fail "saving the module failed";
if refused then () else raise Fail "a state was saved over a loaded module's file";
if intact then () else raise Fail "the module's file was damaged";
