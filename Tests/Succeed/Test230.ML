(* A module load that fails part way, after it has added some of the module's
   segments, removes them again.  One child poly saves module A.  Another tries
   to load a copy of A cut short in the middle of its data, which must fail,
   collects, then loads the whole of A and finds its values intact. *)
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

(* The first half of A.pmo: the descriptors are at the start and the data
   after them, so the load adds the first segments before a read fails. *)
fun cutShort () =
let
    val input = BinIO.openIn (file "A.pmo")
    val bytes = BinIO.inputAll input before BinIO.closeIn input
    val output = BinIO.openOut (file "short.pmo")
in
    BinIO.output (output, Word8VectorSlice.vector (Word8VectorSlice.slice (bytes, 0, SOME (Word8Vector.length bytes div 2))));
    BinIO.closeOut output
end;

val loadAfterFailure =
    "val failed = (PolyML.SaveState.loadModule " ^ quoted "short.pmo" ^ "; false)\n\
    \    handle Fail _ => true | OS.SysErr _ => true;\n\
    \val () = if failed then () else raise Fail \"the short module loaded\";\n\
    \val () = if PolyML.SaveState.showLoadedModules () = [] then () else raise Fail \"the short module is listed\";\n\
    \val () = PolyML.fullGC ();\n\
    \val _ = PolyML.SaveState.loadModule " ^ quoted "A.pmo" ^ ";\n\
    \fun check () =\n\
    \   (if List.length A.xs = 20000 then () else raise Fail \"A length\";\n\
    \    List.app (fn (i, s, r) => if s = Int.toString i ^ \"-a\" andalso Real.== (r, real i / 2.0) then ()\n\
    \        else raise Fail \"A value\") A.xs;\n\
    \    if !A.r = 3 then () else raise Fail \"A.r value\");\n\
    \val () = check ();\n\
    \val () = PolyML.fullGC ();\n\
    \val () = check ();\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val saved = runChild save;
val () = if saved then cutShort () else ();
val loaded = saved andalso runChild loadAfterFailure;
val () = List.app (fn f => OS.FileSys.remove (file f) handle OS.SysErr _ => ()) ["A.pmo", "short.pmo"];
val () = OS.FileSys.rmDir dir handle OS.SysErr _ => ();
if saved then () else raise Fail "saving the module failed";
if loaded then () else raise Fail "after a failed load of a short module, the whole module did not load intact";
