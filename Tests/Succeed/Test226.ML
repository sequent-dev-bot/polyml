(* On Linux a saved state's data segments are mapped from the file at the
   address they were saved from, and read in and relocated as before when that
   address is taken.  Save a two-level hierarchy, then load it in one child that
   checks both files were mapped and in another that first reserves the whole
   region states are saved in, so that neither can be, and check both see the
   same values.  Code segments are loaded where they were saved too: a third
   child loads the hierarchy twice, so that the second load finds the first's
   data and code where it wants to put its own and has to relocate both, then
   saves a grandchild that a fourth child loads.  The first load must not move
   any segment the files create. *)
case #lookupStruct (PolyML.globalNameSpace) "Unix" of
    SOME _ => ()
|   NONE => raise NotApplicable;
(* Only Linux maps states, and not in 32-in-64. *)
if OS.FileSys.access ("/proc/self/maps", [OS.FileSys.A_READ]) andalso
   Word.wordSize = 63
then () else raise NotApplicable;

val parentFile = OS.FileSys.tmpName ();
val childFile = OS.FileSys.tmpName ();
val grandchildFile = OS.FileSys.tmpName ();

(* Run a script in a child poly that exits with an error on any exception.
   Returns whether it succeeded and its output. *)
fun runChildWith (args: string list) (script: string): bool * string =
let
    val proc: (TextIO.instream, TextIO.outstream) Unix.proc =
        Unix.execute(CommandLine.name(), "--error-exit" :: args)
    val toChild = Unix.textOutstreamOf proc
    val () = TextIO.output(toChild, script)
    val () = TextIO.closeOut toChild
    val fromChild = Unix.textInstreamOf proc
    val out = TextIO.inputAll fromChild
    val () = TextIO.closeIn fromChild
in
    (OS.Process.isSuccess(Unix.reap proc), out)
end;

fun runChild script = #1 (runChildWith [] script);

fun str s = "\"" ^ String.toString s ^ "\"";

val saveParent =
    "val xs = List.tabulate (100000, fn i => (i, Int.toString i));\n\
    \val r: (string * int) list ref = ref [];\n\
    \val () = PolyML.SaveState.saveChild (" ^ str parentFile ^ ", 0);\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val saveChild =
    "val () = PolyML.SaveState.loadState " ^ str parentFile ^ ";\n\
    \val ys = map (fn (i, s) => (s, i * 2)) xs;\n\
    \val () = r := ys;\n\
    \fun f i = i * 3 + 1;\n\
    \val g = fn i => f i + length xs;\n\
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
    \        (ListPair.zip (xs, ys)) andalso\n\
    \    length (!r) = 100000 andalso hd (!r) = (\"0\", 0) andalso f 5 = 16 andalso g 1 = 100004;\n";

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
    \val reserved = mmap (base, 0x100000000000, 0, 0x2 + 0x20 + 0x4000 + 0x100000, ~1, 0);\n\
    \val () = if Foreign.Memory.voidStar2Sysword reserved = 0wx200000000000 then () else raise Fail \"reserve\";\n" ^
    checkLoaded ^ checkValues ^
    "val () = OS.Process.exit\n\
    \    (if ok andalso mapped " ^ str parentFile ^ " = 0 andalso mapped " ^ str childFile ^ " = 0\n\
    \     then OS.Process.success else OS.Process.failure);\n";

(* The second load relocates the hierarchy: the first's segments are still there.
   It first says whether the code area is at the address memmgr.cpp prefers, as it
   is unless that is taken or the host needs separate writable and executable
   mappings of code. *)
val loadTwice =
    "val codeFixed =\n\
    \    let val s = TextIO.openIn \"/proc/self/maps\"\n\
    \        fun go () = case TextIO.inputLine s of\n\
    \            NONE => false\n\
    \          | SOME l => String.isPrefix \"6eff00000000-\" l orelse go ()\n\
    \    in go () before TextIO.closeIn s end;\n\
    \val () = print (if codeFixed then \"CODEAREA fixed\\n\" else \"CODEAREA elsewhere\\n\");\n\
    \val () = (PolyML.SaveState.loadHierarchy [" ^ str parentFile ^ ", " ^ str childFile ^ "];\n\
    \          PolyML.SaveState.loadHierarchy [" ^ str parentFile ^ ", " ^ str childFile ^ "]);\n" ^
    checkValues ^
    "val zs = map (fn (s, i) => (i, f i)) (!r);\n\
    \val () = PolyML.SaveState.saveChild (" ^ str grandchildFile ^ ", 2);\n\
    \val () = OS.Process.exit (if ok then OS.Process.success else OS.Process.failure);\n";

val loadGrandchild =
    "val () = PolyML.SaveState.loadHierarchy [" ^ str parentFile ^ ", " ^ str childFile ^ ", " ^ str grandchildFile ^ "];\n" ^
    checkValues ^
    "val ok = ok andalso length zs = 100000 andalso hd zs = (0, 1);\n\
    \val () = OS.Process.exit (if ok then OS.Process.success else OS.Process.failure);\n";

(* With --debug savetime each load logs
   "LOAD: <file>: <n> of <m> segments moved (<k> of the <l> this file creates), ...".
   The first load must put every segment either file creates where it was saved,
   code included, when the code area is where it was when they were saved.  The
   executable's segments, and the ones the files overwrite in it, may move if it is
   position-independent.  The second load of the child must have moved some of the
   segments it creates. *)
fun ownMoved (file: string) (out: string): int list =
let
    val prefix = "LOAD: " ^ file ^ ": "
    val lines = List.filter (String.isPrefix prefix) (String.fields (fn c => c = #"\n") out)
    fun own l =
        case String.fields (fn c => c = #"(") l of
            _ :: rest :: _ => (case Int.fromString rest of SOME n => n | NONE => raise Fail l)
          | _ => raise Fail l
in
    map own lines
end;

fun placedThenMoved (out: string) =
let
    val codeFixed = List.exists (fn l => l = "CODEAREA fixed") (String.fields (fn c => c = #"\n") out)
in
    (case (ownMoved parentFile out, ownMoved childFile out) of
        (p :: _, [c, second]) => (not codeFixed orelse (p = 0 andalso c = 0)) andalso second > 0
      | _ => false)
    handle Fail _ => false
end;

val savedParent = runChild saveParent;
val savedChild = savedParent andalso runChild saveChild;
val mappedOk = savedChild andalso runChild loadMapped;
val relocatedOk = savedChild andalso runChild loadRelocated;
val (twiceOk, twiceOut) = if savedChild then runChildWith ["--debug", "savetime"] loadTwice else (false, "");
val grandchildOk = twiceOk andalso runChild loadGrandchild;
val () = app (fn f => OS.FileSys.remove f handle OS.SysErr _ => ()) [parentFile, childFile, grandchildFile];
if savedChild then () else raise Fail "saving the hierarchy failed";
if mappedOk then () else raise Fail "the hierarchy was not mapped or its values differ";
if relocatedOk then () else raise Fail "the relocated hierarchy was mapped or its values differ";
if twiceOk then () else raise Fail "the hierarchy loaded twice has wrong values or its child couldn't be saved";
if placedThenMoved twiceOut then () else raise Fail "the first load moved a segment a file creates or the second didn't relocate the child";
if grandchildOk then () else raise Fail "the grandchild saved after relocating has wrong values";
