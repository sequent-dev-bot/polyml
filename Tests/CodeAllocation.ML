(* Compiler-only timing diagnostic: compile the same structure 50 times.
   Compare COMPILER_GC_EVERY=0 and 5 in fresh processes. CURVE columns are
   phase, index, wall/user/system/GC seconds, code capacity before/after,
   heap capacity after, full/partial GC count changes. No timing assertion. *)
fun environment name fallback =
  case OS.Process.getEnv name of NONE => fallback | SOME value => value
fun number name fallback =
  valOf (Int.fromString (environment name (Int.toString fallback)))

val source = String.concat (List.tabulate (number "COMPILER_FUNCTIONS" 2000,
  fn index =>
    let val name = Int.toString index
    in "fun f" ^ name ^ " x = let val cell = ref x in fn y => " ^
      "(cell := !cell + y; !cell + " ^ name ^ ") end\n" end))
val program = "structure Repeated = struct\n" ^ source ^ "\nend;\n"
val count = number "COMPILER_LOOPS" 50
val gc_every = number "COMPILER_GC_EVERY" 0
val _ = print ("SOURCE\t" ^ Int.toString (String.size source) ^
  "\tLOOPS\t" ^ Int.toString count ^ "\tGC_EVERY\t" ^
  Int.toString gc_every ^ "\n")

fun compile () =
  let
    val stream = TextIO.openString program
    fun loop () =
      if TextIO.endOfStream stream then ()
      else (PolyML.compiler (fn () => TextIO.input1 stream,
        [PolyML.Compiler.CPOutStream (fn _ => ())]) (); loop ())
    val _ = loop () handle error => (TextIO.closeIn stream; raise error)
  in TextIO.closeIn stream end

fun measure phase index action =
  let
    val stats_before = PolyML.Statistics.getLocalStats ()
    val wall = Timer.startRealTimer ()
    val timer = Timer.startCPUTimer ()
    val _ = action ()
    val elapsed = Timer.checkRealTimer wall
    val cpu = Timer.checkCPUTimer timer
    val gc_cpu = Timer.checkGCTime timer
    val stats_after = PolyML.Statistics.getLocalStats ()
    val _ = print (String.concatWith "\t"
      ["CURVE", phase, Int.toString index, Time.toString elapsed,
       Time.toString (#usr cpu), Time.toString (#sys cpu),
       Time.toString gc_cpu,
       LargeInt.toString (#sizeCode stats_before),
       LargeInt.toString (#sizeCode stats_after),
       LargeInt.toString (#sizeHeap stats_after),
       Int.toString (#gcFullGCs stats_after - #gcFullGCs stats_before),
       Int.toString (#gcPartialGCs stats_after - #gcPartialGCs stats_before)] ^ "\n")
  in TextIO.flushOut TextIO.stdOut end

val _ = measure "warm" 0 compile
val _ = measure "initial_gc" 0 PolyML.fullGC
fun loop index =
  if index > count then () else
    (if gc_every > 0 andalso index > 1 andalso
        (index - 1) mod gc_every = 0
     then measure "periodic_gc" index PolyML.fullGC else ();
     measure "compile" index compile;
     loop (index + 1))
val _ = loop 1
val _ = measure "final_gc" (count + 1) PolyML.fullGC
val _ = measure "post_gc" (count + 1) compile
val _ = OS.Process.exit OS.Process.success
