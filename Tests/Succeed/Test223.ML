(* Repeated compilation fills code areas; full GC must reclaim dead code
   without losing retained code or corrupting allocations into the free holes. *)
local
    fun program count =
    let
        val functions = List.tabulate(count, fn i =>
            let val n = Int.toString i
            in "fun f" ^ n ^ " x = let val r = ref x in fn y => " ^
                "(r := !r + y; !r + " ^ n ^ ") end\n" end)
        val calls = List.tabulate(count, fn i => "f" ^ Int.toString i ^ " 1")
        val expected = List.tabulate(count, fn i => Int.toString(i + 3))
    in
        "val _ = let\n" ^ String.concat functions ^
        "val fs = [" ^ String.concatWith "," calls ^ "]\n" ^
        "in if List.map (fn f => f 2) fs = [" ^
        String.concatWith "," expected ^ "] then () " ^
        "else raise Fail \"compiled code changed\" end;"
    end

    fun compile source =
    let
        val stream = TextIO.openString source
        val code = PolyML.compiler(fn () => TextIO.input1 stream,
            [PolyML.Compiler.CPOutStream(fn _ => ())])
            handle error => (TextIO.closeIn stream; raise error)
        val () = TextIO.closeIn stream
    in code end

    fun loop (0, retained) = (PolyML.fullGC(); List.app (fn f => f()) retained)
    |   loop (n, retained) =
        let
            val code = compile(program(64 + 13 * (n mod 7)))
            val () = code()
            val retained = if n mod 4 = 0 then code :: retained else retained
            val () = if n mod 8 = 0 then
                (PolyML.fullGC(); List.app (fn f => f()) retained) else ()
        in loop(n - 1, retained) end
in
    val () = loop(48, [])
end;
