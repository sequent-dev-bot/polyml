(* A fatal signal must leave a trace: on Linux with glibc the runtime writes
   the signal, the faulting address and registers and a backtrace to stderr,
   then dies by the same signal so the parent still sees it.  A child poly
   calls strlen on a null pointer through the foreign function interface, so
   the fault is in C code. *)
case (#lookupStruct (PolyML.globalNameSpace) "Unix",
      #lookupStruct (PolyML.globalNameSpace) "Foreign") of
    (SOME _, SOME _) => ()
|   _ => raise NotApplicable;
(* The runtime installs the handler only on Linux with glibc. *)
case List.find (fn (key, _) => key = "sysname") (Posix.ProcEnv.uname ()) of
    SOME (_, "Linux") => ()
|   _ => raise NotApplicable;
val _ =
    Foreign.symbolAsAddress
        (Foreign.getSymbol (Foreign.loadExecutable ()) "gnu_get_libc_version")
    handle Foreign.Foreign _ => raise NotApplicable;

val crash =
    "val strlen = Foreign.buildCall1\n\
    \    (Foreign.getSymbol (Foreign.loadExecutable ()) \"strlen\",\n\
    \     Foreign.cPointer, Foreign.cUlong);\n\
    \val _ = strlen Foreign.Memory.null;\n";

(* The shell turns off core files and sends the child's stderr to the pipe. *)
val proc: (TextIO.instream, TextIO.outstream) Unix.proc =
    Unix.execute("/bin/sh",
        ["-c", "ulimit -c 0; exec \"$0\" --error-exit 2>&1", CommandLine.name()]);
val toChild = Unix.textOutstreamOf proc;
val () = TextIO.output(toChild, crash);
val () = TextIO.closeOut toChild;
val fromChild = Unix.textInstreamOf proc;
val output = TextIO.inputAll fromChild;
val () = TextIO.closeIn fromChild;
val status = Unix.reap proc;

val header = "Poly/ML: fatal signal 11 code ";
val () =
    if String.isSubstring header output then ()
    else raise Fail ("no crash header in the child's stderr: " ^ output);
(* backtrace_symbols_fd writes each frame as "object(symbol+offset)[0xaddress]". *)
val afterHeader = #2 (Substring.position header (Substring.full output));
val () =
    if String.isSubstring ")[0x" (Substring.string afterHeader) then ()
    else raise Fail ("no backtrace after the crash header: " ^ output);
val () =
    case Posix.Process.fromStatus status of
        Posix.Process.W_SIGNALED s =>
            if s = Posix.Signal.segv then ()
            else raise Fail "the child died by a signal other than SIGSEGV"
    |   _ => raise Fail "the child did not die by a signal";
