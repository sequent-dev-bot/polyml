(* A forked child must be able to collect and set a signal handler before
   exec, even while allocation, GC workers and signal detection are active.
   Also run with --debug checkmem to detect writes to a promoted signal result. *)
case #lookupStruct PolyML.globalNameSpace "Unix" of
    SOME _ => ()
|   NONE => raise NotApplicable;

if OS.FileSys.access("/bin/true", [OS.FileSys.A_EXEC]) then ()
else raise NotApplicable;

local
    val oldHandler = Signal.signal(Posix.Signal.usr1, Signal.SIG_HANDLE(fn _ => ()))
    val self = Posix.ProcEnv.getpid()
    val stop = ref false
    fun peer () =
        if !stop then ()
        else
            let
                val cells = List.tabulate(1000, ref)
                val () = Posix.Process.kill(Posix.Process.K_PROC self, Posix.Signal.usr1)
                val () = OS.Process.sleep(Time.fromMilliseconds 1)
                val () = if !(List.last cells) = 999 then ()
                         else raise Fail "peer data changed"
            in
                peer ()
            end
    val thread = Thread.Thread.fork(peer, [])
    fun loop 0 = ()
    |   loop n =
        (PolyML.fullGC();
         case Posix.Process.fork() of
            NONE =>
                (PolyML.fullGC();
                 Signal.signal(Posix.Signal.usr1, Signal.SIG_IGN);
                 Posix.Process.exec("/bin/true", ["true"]))
        |   SOME pid =>
                (case #2 (Posix.Process.waitpid(Posix.Process.W_CHILD pid, [])) of
                     Posix.Process.W_EXITED => loop (n - 1)
                 |   _ => raise Fail "fork/GC child failed"))
    fun finish () =
        (stop := true;
         while Thread.Thread.isActive thread do
             OS.Process.sleep(Time.fromMilliseconds 1);
         Signal.signal(Posix.Signal.usr1, oldHandler); ())
in
    val () = (loop 256; finish()) handle e => (finish(); raise e)
end;
