(* shareCommonData PolyML.rootFunction after loading saved states must not
   merge their immutable data: their spaces are permanent but not
   write-protected, and merging an object of a state into an equal one of its
   child made the parent point into the child's spaces, which saving the child
   again replaces.  One child poly saves a state, another a child of it with a
   copy of its data, and a third loads the child, shares, saves it again and
   collects; a fourth loads what it saved. *)
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

val check =
    "val () = if List.length S.xs = 20000 then () else raise Fail \"S length\";\n\
    \val () = List.app (fn (i, s) => if s = Int.toString i ^ \"-s\" then () else raise Fail \"S value\") S.xs;\n\
    \val () = if T.ys = S.xs then () else raise Fail \"T value\";\n";

val save =
    "structure S = struct\n\
    \  val xs = List.tabulate (20000, fn i => (i, Int.toString i ^ \"-s\"))\n\
    \end;\n\
    \val () = PolyML.SaveState.saveState " ^ quoted "state" ^ ";\n\
    \val () = OS.Process.exit OS.Process.success;\n";

(* T.ys equals S.xs but is a copy, saved one level below it. *)
val saveChild =
    "val () = PolyML.SaveState.loadState " ^ quoted "state" ^ ";\n\
    \structure T = struct\n\
    \  val ys = List.map (fn (i, s) => (i + 0, String.implode (String.explode s))) S.xs\n\
    \end;\n\
    \val () = PolyML.SaveState.saveChild (" ^ quoted "child" ^ ", 1);\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val loadShareSave =
    "val () = PolyML.SaveState.loadState " ^ quoted "child" ^ ";\n\
    \val () = PolyML.shareCommonData PolyML.rootFunction;\n" ^
    check ^
    "val () = PolyML.SaveState.saveChild (" ^ quoted "resaved" ^ ", 1);\n\
    \val () = PolyML.fullGC ();\n" ^
    check ^
    "val () = OS.Process.exit OS.Process.success;\n";

val loadResaved =
    "val () = PolyML.SaveState.loadState " ^ quoted "resaved" ^ ";\n" ^
    check ^
    "val () = OS.Process.exit OS.Process.success;\n";

val saved = runChild save andalso runChild saveChild;
val shared = saved andalso runChild loadShareSave;
val resavedLoaded = shared andalso runChild loadResaved;
val () = List.app (fn f => OS.FileSys.remove (file f) handle OS.SysErr _ => ()) ["state", "child", "resaved"];
val () = OS.FileSys.rmDir dir handle OS.SysErr _ => ();
if saved then () else raise Fail "saving the state and its child failed";
if shared then () else raise Fail "sharing after loading the child, or saving it again, failed";
if resavedLoaded then () else raise Fail "the state saved again did not load intact";
