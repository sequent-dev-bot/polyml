(* Saving a module whose contents equal a loaded module's fails: the loaded
   module's id would not name the new module's objects, such as its refs.
   A child saves a module, then a module with the same contents. *)
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

val contents = "{structs = [\"M\"], functors = [], sigs = [], onStartup = NONE}";

val saveTwice =
    "fun save name = (PolyML.SaveState.saveModule (name, " ^ contents ^ "); true)\n\
    \                handle OS.SysErr _ => false;\n\
    \structure M = struct val r = ref 0 end;\n\
    \val () = if save " ^ quoted "m1.pmo" ^ " then () else raise Fail \"first\";\n\
    \structure M = struct val r = ref 0 end;\n\
    \val () = if save " ^ quoted "m2.pmo" ^ " then raise Fail \"duplicate\" else ();\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val refused = runChild saveTwice;
val written = List.filter (fn f => OS.FileSys.access (file f, []))
                          ["m1.pmo", "m2.pmo"];
val () = List.app (fn f => OS.FileSys.remove (file f) handle OS.SysErr _ => ())
                  ["m1.pmo", "m2.pmo"];
val () = OS.FileSys.rmDir dir handle OS.SysErr _ => ();
if refused then () else raise Fail "a module with a loaded module's contents was saved";
if written = ["m1.pmo"] then () else raise Fail "the refused module's file was written";
