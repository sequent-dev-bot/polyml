(* A module's immutable data, when it is one segment, is mapped from its file at
   the address it was prelinked for, and is write-protected.  One child poly
   saves module A and module B, which depends on A.  Another loads B, and so A,
   checks that both files are mapped and the values are intact, then saves a
   state and module C while they are mapped, releases B and collects, and
   releases A and loads it again.  Further children load the state and C, and
   load B with A's address taken, so that A is read in instead. *)
case (#lookupStruct (PolyML.globalNameSpace) "Unix",
      #lookupStruct (PolyML.globalNameSpace) "Foreign") of
    (SOME _, SOME _) => ()
|   _ => raise NotApplicable;
val () = if OS.FileSys.access ("/proc/self/maps", [OS.FileSys.A_READ]) then () else raise NotApplicable;

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
    "structure A = struct\n\
    \  val xs = List.tabulate (20000, fn i => (i, Int.toString i ^ \"-a\", real i / 2.0))\n\
    \end;\n\
    \val idA = PolyML.SaveState.saveModule (" ^ quoted "A.pmo" ^ ", " ^ contentsOf "A" ^ ");\n\
    \structure B = struct\n\
    \  val ys = List.map (fn (_, s, _) => s ^ \"b\") A.xs\n\
    \  val xs = A.xs\n\
    \end;\n\
    \val idB = PolyML.SaveState.saveDependentModule (" ^ quoted "B.pmo" ^ ", " ^ contentsOf "B" ^
    ", [(idA, " ^ quoted "A.pmo" ^ ")]);\n\
    \val () = OS.Process.exit OS.Process.success;\n";

(* Checks of A and B, as ML declarations that raise Fail if a check fails. *)
val checkA =
    "val () = if List.length A.xs = 20000 then () else raise Fail \"A length\";\n\
    \val () = List.app (fn (i, s, r) => if s = Int.toString i ^ \"-a\" andalso Real.== (r, real i / 2.0) then ()\n\
    \    else raise Fail \"A value\") A.xs;\n";
val checkB =
    "val () = ListPair.appEq (fn ((_, s, _), t) => if t = s ^ \"b\" then () else raise Fail \"B value\") (A.xs, B.ys);\n";

val mapsCount =
    "fun mapsCount name =\n\
    \let\n\
    \    val maps = TextIO.openIn \"/proc/self/maps\"\n\
    \    val lines = String.tokens (fn c => c = #\"\\n\") (TextIO.inputAll maps)\n\
    \in\n\
    \    TextIO.closeIn maps; List.length (List.filter (String.isSuffix name) lines)\n\
    \end;\n";

val load =
    mapsCount ^
    "val idB = PolyML.SaveState.loadModule " ^ quoted "B.pmo" ^ ";\n\
    \val () = if mapsCount \"/A.pmo\" = 1 andalso mapsCount \"/B.pmo\" = 1 then () else raise Fail \"not mapped\";\n" ^
    checkA ^ checkB ^
    "val () = if PolyML.pointerEq (A.xs, B.xs) then () else raise Fail \"B.xs is not A.xs\";\n\
    \val idA = valOf (List.find (fn id => id <> idB) (PolyML.SaveState.showLoadedModules ()));\n\
    \val () = PolyML.SaveState.saveState " ^ quoted "state" ^ ";\n\
    \structure C = struct val zs = (A.xs, List.rev B.ys) end;\n\
    \val _ = PolyML.SaveState.saveDependentModule (" ^ quoted "C.pmo" ^ ", " ^ contentsOf "C" ^
    ", [(idB, " ^ quoted "B.pmo" ^ ")]);\n\
    \val () = PolyML.SaveState.releaseModule idB;\n\
    \val () = PolyML.fullGC ();\n" ^
    checkA ^ checkB ^
    "val oldXs = A.xs;\n\
    \val () = PolyML.SaveState.releaseModule idA;\n\
    \val _ = PolyML.SaveState.loadModule " ^ quoted "A.pmo" ^ ";\n\
    \val () = if PolyML.pointerEq (oldXs, A.xs) then raise Fail \"A was not loaded again\" else ();\n" ^
    checkA ^
    "val () = PolyML.fullGC ();\n" ^
    checkA ^
    "val () = OS.Process.exit OS.Process.success;\n";

val loadState =
    "val () = PolyML.SaveState.loadState " ^ quoted "state" ^ ";\n" ^
    checkA ^ checkB ^
    "val () = OS.Process.exit OS.Process.success;\n";

val loadC =
    "val _ = PolyML.SaveState.loadModule " ^ quoted "C.pmo" ^ ";\n" ^
    checkA ^ checkB ^
    "val () = ListPair.appEq (fn ((i, s, r), (j, t, q)) => if i = j andalso s = t andalso Real.== (r, q) then ()\n\
    \    else raise Fail \"C value\") (#1 C.zs, A.xs);\n\
    \val () = if List.rev (#2 C.zs) = B.ys then () else raise Fail \"C value\";\n\
    \val () = OS.Process.exit OS.Process.success;\n";

(* Reserve the pages A's data was prelinked for, found in its segment
   descriptors: one with data, no flags and an address. *)
val collide =
    mapsCount ^
    "val bytes = let val f = BinIO.openIn " ^ quoted "A.pmo" ^ " in BinIO.inputAll f before BinIO.closeIn f end;\n\
    \fun word32 i = LargeWord.toInt (PackWord32Little.subVec (bytes, i div 4));\n\
    \fun word64 i = word32 i + word32 (i + 4) * 4294967296;\n\
    \val descrs = word32 24 and count = word32 32;\n\
    \val SOME (addr, size) = List.find (fn (a, _) => a <> 0) (List.tabulate (count, fn n =>\n\
    \    let val d = descrs + 64 * n in\n\
    \        if word64 d <> 0 andalso word32 (d + 32) = 0 then (word64 (d + 48), word64 (d + 8)) else (0, 0)\n\
    \    end));\n\
    \val mmap = Foreign.buildCall6 (Foreign.getSymbol (Foreign.loadExecutable ()) \"mmap\",\n\
    \    (Foreign.cPointer, Foreign.cUlong, Foreign.cInt, Foreign.cInt, Foreign.cInt, Foreign.cLong), Foreign.cPointer);\n\
    \val start = addr - addr mod 65536;\n\
    \val wanted = Foreign.Memory.sysWord2VoidStar (SysWord.fromInt start);\n\
    \val got = mmap (wanted, addr + size - start, 0, 0x22 + 0x100000, ~1, 0);\n\
    \val () = if got = wanted then () else raise Fail \"could not reserve A's pages\";\n\
    \val _ = PolyML.SaveState.loadModule " ^ quoted "B.pmo" ^ ";\n\
    \val () = if mapsCount \"/A.pmo\" = 0 andalso mapsCount \"/B.pmo\" = 1 then () else raise Fail \"A was mapped\";\n" ^
    checkA ^ checkB ^
    "val () = OS.Process.exit OS.Process.success;\n";

val saved = runChild save;
val loaded = saved andalso runChild load;
val stateLoaded = loaded andalso runChild loadState;
val cLoaded = loaded andalso runChild loadC;
val collided = saved andalso runChild collide;
val () = List.app (fn f => OS.FileSys.remove (file f) handle OS.SysErr _ => ()) ["A.pmo", "B.pmo", "C.pmo", "state"];
val () = OS.FileSys.rmDir dir handle OS.SysErr _ => ();
if saved then () else raise Fail "saving the modules failed";
if loaded then () else raise Fail "loading, saving from and releasing the mapped modules failed";
if stateLoaded then () else raise Fail "the state saved with the modules mapped did not load back intact";
if cLoaded then () else raise Fail "the module saved with the modules mapped did not load back intact";
if collided then () else raise Fail "the module did not load intact with its address taken";
