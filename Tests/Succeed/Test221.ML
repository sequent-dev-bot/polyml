(* A forked child must be able to collect before exec, even if another
   runtime thread held the allocation or statistics lock during fork. *)
case #lookupStruct PolyML.globalNameSpace "Unix" of
    SOME _ => ()
|   NONE => raise NotApplicable;

if OS.FileSys.access("/bin/true", [OS.FileSys.A_EXEC]) then ()
else raise NotApplicable;

local
    val stop = ref false
    fun peer () =
        if !stop then ()
        else
            let
                val cells = List.tabulate(1000, ref)
                val () = OS.Process.sleep(Time.fromMilliseconds 1)
                val () = if !(List.last cells) = 999 then ()
                         else raise Fail "peer data changed"
            in
                peer ()
            end
    val thread = Thread.Thread.fork(peer, [])
    fun loop 0 = ()
    |   loop n =
        case Posix.Process.fork() of
            NONE =>
                (PolyML.fullGC(); Posix.Process.exec("/bin/true", ["true"]))
        |   SOME pid =>
                (case #2 (Posix.Process.waitpid(Posix.Process.W_CHILD pid, [])) of
                     Posix.Process.W_EXITED => loop (n - 1)
                 |   _ => raise Fail "fork/GC child failed")
    fun finish () =
        (stop := true;
         while Thread.Thread.isActive thread do
             OS.Process.sleep(Time.fromMilliseconds 1))
in
    val () = (loop 256; finish()) handle e => (finish(); raise e)
end;
