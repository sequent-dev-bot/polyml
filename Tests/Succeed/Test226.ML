(* On Linux a saved state's data segments are mapped from the file at the
   address they were saved from, and read in and relocated as before when that
   address is taken.  Save a two-level hierarchy, then load it in one child that
   checks both files were mapped and in another that first reserves the whole
   region states are saved in, so that neither can be, and check both see the
   same values. *)
case #lookupStruct (PolyML.globalNameSpace) "Unix" of
    SOME _ => ()
|   NONE => raise NotApplicable;
(* Only Linux maps states, and not in 32-in-64. *)
if OS.FileSys.access ("/proc/self/maps", [OS.FileSys.A_READ]) andalso
   Word.wordSize = 63
then () else raise NotApplicable;

val parentFile = OS.FileSys.tmpName ();
val childFile = OS.FileSys.tmpName ();

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

fun str s = "\"" ^ String.toString s ^ "\"";

val saveParent =
    "val xs = List.tabulate (100000, fn i => (i, Int.toString i));\n\
    \val () = PolyML.SaveState.saveChild (" ^ str parentFile ^ ", 0);\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val saveChild =
    "val () = PolyML.SaveState.loadState " ^ str parentFile ^ ";\n\
    \val ys = map (fn (i, s) => (s, i * 2)) xs;\n\
    \val () = PolyML.SaveState.saveChild (" ^ str childFile ^ ", 1);\n\
    \val () = OS.Process.exit OS.Process.success;\n";

(* Lines of /proc/self/maps naming each state file, and the values. *)
val checkLoaded =
    "val () = PolyML.SaveState.loadHierarchy [" ^ str parentFile ^ ", " ^ str childFile ^ "];\n\
    \fun mapped f =\n\
    \    let val s = TextIO.openIn \"/proc/self/maps\"\n\
    \        fun count n = case TextIO.inputLine s of\n\
    \            NONE => n\n\
    \          | SOME l => count (if String.isSuffix (f ^ \"\\n\") l then n + 1 else n)\n\
    \    in count 0 before TextIO.closeIn s end;\n";

val checkValues =
    "val ok = length xs = 100000 andalso length ys = 100000 andalso\n\
    \    List.all (fn ((i, s), (t, j)) => s = Int.toString i andalso t = s andalso j = 2 * i)\n\
    \        (ListPair.zip (xs, ys));\n";

val loadMapped =
    checkLoaded ^ checkValues ^
    "val () = OS.Process.exit\n\
    \    (if ok andalso mapped " ^ str parentFile ^ " > 0 andalso mapped " ^ str childFile ^ " > 0\n\
    \     then OS.Process.success else OS.Process.failure);\n";

(* MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE over the region
   memmgr.cpp saves states in. *)
val loadRelocated =
    "local open Foreign in\n\
    \val mmap = buildCall6 (getSymbol (loadExecutable ()) \"mmap\",\n\
    \    (cPointer, cSize, cInt, cInt, cInt, cInt64), cPointer)\n\
    \end;\n\
    \val base = Foreign.Memory.sysWord2VoidStar 0wx200000000000;\n\
    \val r = mmap (base, 0x100000000000, 0, 0x2 + 0x20 + 0x4000 + 0x100000, ~1, 0);\n\
    \val () = if Foreign.Memory.voidStar2Sysword r = 0wx200000000000 then () else raise Fail \"reserve\";\n" ^
    checkLoaded ^ checkValues ^
    "val () = OS.Process.exit\n\
    \    (if ok andalso mapped " ^ str parentFile ^ " = 0 andalso mapped " ^ str childFile ^ " = 0\n\
    \     then OS.Process.success else OS.Process.failure);\n";

val savedParent = runChild saveParent;
val savedChild = savedParent andalso runChild saveChild;
val mappedOk = savedChild andalso runChild loadMapped;
val relocatedOk = savedChild andalso runChild loadRelocated;
val () = app (fn f => OS.FileSys.remove f handle OS.SysErr _ => ()) [parentFile, childFile];
if savedChild then () else raise Fail "saving the hierarchy failed";
if mappedOk then () else raise Fail "the hierarchy was not mapped or its values differ";
if relocatedOk then () else raise Fail "the relocated hierarchy was mapped or its values differ";
