(* A first callback on a C-created pthread must initialize its stack and heap
   registers, then release ML memory when the pthread exits. *)
case #lookupStruct PolyML.globalNameSpace "Unix" of
    SOME _ => ()
|   NONE => raise NotApplicable;

if OS.FileSys.access("/bin/true", [OS.FileSys.A_EXEC]) then ()
else raise NotApplicable;

local
    open Foreign
    val executable = loadExecutable ()
    val createSymbol = getSymbol executable "pthread_create"
        handle Foreign _ => raise NotApplicable
    val joinSymbol = getSymbol executable "pthread_join"
        handle Foreign _ => raise NotApplicable
    val create: Memory.voidStar * Memory.voidStar *
            (Memory.voidStar -> Memory.voidStar) closure * Memory.voidStar -> int =
        buildCall4(createSymbol, (cPointer, cPointer, cFunction, cPointer), cInt)
    val join: Memory.voidStar * Memory.voidStar -> int =
        buildCall2(joinSymbol, (cPointer, cPointer), cInt)
    val called = ref false
    val callback = buildCallback1(cPointer, cPointer)
        (fn arg => (called := true; arg))
    val thread = Memory.malloc(Word.fromInt(SysWord.wordSize div 8))
    val () = if create(thread, Memory.null, callback, Memory.null) = 0 then ()
             else raise Fail "pthread_create failed"
    val () = if join(Memory.getAddress(thread, 0w0), Memory.null) = 0 then ()
             else raise Fail "pthread_join failed"
    val () = Memory.free thread
    val () = if !called then () else raise Fail "callback was not called"
in
    val () = PolyML.fullGC()
    val () = case Posix.Process.fork() of
        NONE => (PolyML.fullGC(); Posix.Process.exec("/bin/true", ["true"]))
      | SOME pid =>
            (case #2(Posix.Process.waitpid(Posix.Process.W_CHILD pid, [])) of
                 Posix.Process.W_EXITED => ()
               | _ => raise Fail "post-callback fork failed")
end;
