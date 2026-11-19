(* saveChildWithModules saves a state that refers to the modules loaded from
   files instead of copying them.  One child poly saves module A and module B,
   which depends on A.  Another loads B, and so A, saves a state with the
   modules and, for comparison, one without.  Further children load the state,
   which loads the modules, and save children of it, with modules and without,
   and loading the state fails cleanly when A has changed or is missing. *)
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
    "structure A = struct\n\
    \  val xs = List.tabulate (20000, fn i => (i, Int.toString i ^ \"-a\", real i / 2.0))\n\
    \  val r = ref 0\n\
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
val checkAB =
    "val () = if List.length A.xs = 20000 then () else raise Fail \"A length\";\n\
    \val () = List.app (fn (i, s, r) => if s = Int.toString i ^ \"-a\" andalso Real.== (r, real i / 2.0) then ()\n\
    \    else raise Fail \"A value\") A.xs;\n\
    \val () = ListPair.appEq (fn ((_, s, _), t) => if t = s ^ \"b\" then () else raise Fail \"B value\") (A.xs, B.ys);\n\
    \val () = if PolyML.pointerEq (A.xs, B.xs) then () else raise Fail \"B.xs is not A.xs\";\n";

val saveWithModules =
    "val _ = PolyML.SaveState.loadModule " ^ quoted "B.pmo" ^ ";\n" ^
    checkAB ^
    "val () = PolyML.SaveState.saveChildWithModules (" ^ quoted "state" ^ ", 0);\n\
    \val deps = PolyML.SaveState.showModuleDependencies " ^ quoted "state" ^ ";\n\
    \val () = if List.map #1 deps = PolyML.SaveState.showLoadedModules () then () else raise Fail \"dependency ids\";\n\
    \val () = if List.map #2 deps = List.map OS.FileSys.realPath [" ^ quoted "A.pmo" ^ ", " ^ quoted "B.pmo" ^ "] then () else raise Fail \"dependency paths\";\n" ^
    checkAB ^
    "val () = PolyML.SaveState.saveState " ^ quoted "plain" ^ ";\n\
    \val () = if PolyML.SaveState.showModuleDependencies " ^ quoted "plain" ^ " = [] then () else raise Fail \"plain dependencies\";\n\
    \val () = OS.Process.exit OS.Process.success;\n";

(* The state loads the modules and refers to their data.  A child state saved
   from it with modules records none itself: its parent does.  A child saved
   without modules also refers to the modules' data and records a change to a
   ref in them rather than copying it. *)
val loadAndSaveChild =
    "val () = PolyML.SaveState.loadState " ^ quoted "state" ^ ";\n\
    \val () = if List.length (PolyML.SaveState.showLoadedModules ()) = 2 then () else raise Fail \"modules not loaded\";\n" ^
    checkAB ^
    "val C = List.map (fn (i, _, _) => i * 2) A.xs;\n\
    \val () = PolyML.SaveState.saveChildWithModules (" ^ quoted "child" ^ ", 1);\n\
    \val () = if PolyML.SaveState.showModuleDependencies " ^ quoted "child" ^ " = [] then () else raise Fail \"child dependencies\";\n\
    \val () = A.r := 7;\n\
    \val () = PolyML.SaveState.saveChild (" ^ quoted "plainchild" ^ ", 1);\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val loadChild =
    "val () = PolyML.SaveState.loadState " ^ quoted "child" ^ ";\n" ^
    checkAB ^
    "val () = if C = List.tabulate (20000, fn i => i * 2) then () else raise Fail \"C value\";\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val loadPlainChild =
    "val () = PolyML.SaveState.loadState " ^ quoted "plainchild" ^ ";\n" ^
    checkAB ^
    "val () = if C = List.tabulate (20000, fn i => i * 2) then () else raise Fail \"C value\";\n\
    \val () = if !A.r = 7 then () else raise Fail \"A.r value\";\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val changeA =
    "structure A = struct val xs = [1] end;\n\
    \val _ = PolyML.SaveState.saveModule (" ^ quoted "A.pmo" ^ ", " ^ contentsOf "A" ^ ");\n\
    \val () = OS.Process.exit OS.Process.success;\n";

fun loadFails reason =
    "val () = (PolyML.SaveState.loadState " ^ quoted "state" ^ "; raise Fail \"loaded\")\n\
    \    handle Fail msg => if String.isSubstring \"" ^ reason ^ "\" msg then () else raise Fail msg;\n\
    \val () = OS.Process.exit OS.Process.success;\n";

val saved = runChild save;
val savedState = saved andalso runChild saveWithModules;
(* The modules' data is in the plain state but not in the one that refers to them. *)
val smaller = savedState andalso
    OS.FileSys.fileSize (file "state") + OS.FileSys.fileSize (file "A.pmo") div 2 < OS.FileSys.fileSize (file "plain");
val loaded = savedState andalso runChild loadAndSaveChild;
val childLoaded = loaded andalso runChild loadChild;
val plainChildLoaded = loaded andalso runChild loadPlainChild;
val plainChildSmaller = loaded andalso
    OS.FileSys.fileSize (file "plainchild") < OS.FileSys.fileSize (file "child") + OS.FileSys.fileSize (file "A.pmo") div 2;
val changedFails = savedState andalso runChild changeA andalso runChild (loadFails "Module has a different id");
val () = OS.FileSys.remove (file "A.pmo") handle OS.SysErr _ => ();
val missingFails = savedState andalso runChild (loadFails "Cannot open load file");
val () = List.app (fn f => OS.FileSys.remove (file f) handle OS.SysErr _ => ()) ["A.pmo", "B.pmo", "state", "plain", "child", "plainchild"];
val () = OS.FileSys.rmDir dir handle OS.SysErr _ => ();
if saved then () else raise Fail "saving the modules failed";
if savedState then () else raise Fail "saving the state with modules failed or listed the wrong modules";
if smaller then () else raise Fail "the state with modules holds a copy of their data";
if loaded then () else raise Fail "the state did not load its modules intact or save a child";
if childLoaded then () else raise Fail "the child of the state did not load intact";
if plainChildLoaded then () else raise Fail "the plain child of the state did not load intact";
if plainChildSmaller then () else raise Fail "the plain child of the state holds a copy of the modules' data";
if changedFails then () else raise Fail "loading the state with a changed module did not fail cleanly";
if missingFails then () else raise Fail "loading the state with a missing module did not fail cleanly";
