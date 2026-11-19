(* A module load that fails part way, after it has added some of the module's
   segments, removes them again.  One child poly saves module A, and another a
   state that refers to A.  A third loads the state with A.pmo cut short in the
   middle of its data: the state's loader reads A's header and descriptors and
   fails reading its data.  It collects, puts the whole of A back, loads the
   state again and finds A's values intact. *)
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

val save =
    "structure A = struct\n\
    \  val xs = List.tabulate (20000, fn i => (i, Int.toString i ^ \"-a\", real i / 2.0))\n\
    \  val r = ref 3\n\
    \end;\n\
    \val _ = PolyML.SaveState.saveModule (" ^ quoted "A.pmo" ^
    ", {structs = [\"A\"], functors = [], sigs = [], onStartup = NONE});\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val saveState =
    "val _ = PolyML.SaveState.loadModule " ^ quoted "A.pmo" ^ ";\n\
    \val () = PolyML.SaveState.saveChildWithModules (" ^ quoted "st" ^ ", 0);\n\
    \val () = OS.Process.exit OS.Process.success;\n";

fun contents name =
let val input = BinIO.openIn (file name) in BinIO.inputAll input before BinIO.closeIn input end;
fun write (name, bytes) =
let val output = BinIO.openOut (file name) in BinIO.output (output, bytes); BinIO.closeOut output end;

(* A.pmo becomes its first half: the header and descriptors are at the start,
   then the data, so the load adds the first segments before a read fails. *)
fun cutShort () =
let
    val bytes = contents "A.pmo"
in
    write ("A.full", bytes);
    write ("A.pmo", Word8VectorSlice.vector (Word8VectorSlice.slice (bytes, 0, SOME (Word8Vector.length bytes div 2))))
end;

val loadAfterFailure =
    "val message = (PolyML.SaveState.loadState " ^ quoted "st" ^ "; \"loaded\") handle Fail m => m;\n\
    \val () = if String.isSubstring \"Unable to read segment\" message orelse\n\
    \            String.isSubstring \"Unable to read relocation segment\" message then ()\n\
    \         else raise Fail (\"the state's load did not fail reading A: \" ^ message);\n\
    \val () = if PolyML.SaveState.showLoadedModules () = [] then () else raise Fail \"the short module is listed\";\n\
    \val () = PolyML.fullGC ();\n\
    \val () = OS.FileSys.rename {old = " ^ quoted "A.full" ^ ", new = " ^ quoted "A.pmo" ^ "};\n\
    \val () = PolyML.SaveState.loadState " ^ quoted "st" ^ ";\n\
    \fun check () =\n\
    \   (if List.length A.xs = 20000 then () else raise Fail \"A length\";\n\
    \    List.app (fn (i, s, r) => if s = Int.toString i ^ \"-a\" andalso Real.== (r, real i / 2.0) then ()\n\
    \        else raise Fail \"A value\") A.xs;\n\
    \    if !A.r = 3 then () else raise Fail \"A.r value\");\n\
    \val () = check ();\n\
    \val () = PolyML.fullGC ();\n\
    \val () = check ();\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val saved = runChild save andalso runChild saveState;
val () = if saved then cutShort () else ();
val loaded = saved andalso runChild loadAfterFailure;
val () = List.app (fn f => OS.FileSys.remove (file f) handle OS.SysErr _ => ()) ["A.pmo", "A.full", "st"];
val () = OS.FileSys.rmDir dir handle OS.SysErr _ => ();
if saved then () else raise Fail "saving the module or the state failed";
if loaded then () else raise Fail "after a failed load of a short module, the whole module did not load intact";
