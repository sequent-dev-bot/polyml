(* On Linux a save writes a new file rather than truncating the old one, which
   another process may have mapped.  Check that it keeps the old file's
   permissions and writes through a symbolic link, as truncating did, and that
   loading a state with a segment past the end of the file fails rather than
   mapping it. *)
case #lookupStruct (PolyML.globalNameSpace) "Unix" of
    SOME _ => ()
|   NONE => raise NotApplicable;
if OS.FileSys.access ("/proc/self/maps", [OS.FileSys.A_READ]) andalso
   Word.wordSize = 63
then () else raise NotApplicable;

local open Posix.FileSys in
    fun modeOf f = SysWord.andb (S.toWord (ST.mode (stat f)), 0wx1ff)
    fun sizeOf f = ST.size (stat f)
    fun isLink f = ST.isLink (lstat f)
end;

(* Run a script in a child poly; true if it exits with success. *)
fun runChild (script: string): bool =
let
    val proc: (TextIO.instream, TextIO.outstream) Unix.proc =
        Unix.execute(CommandLine.name(), ["-q", "--error-exit"])
    val toChild = Unix.textOutstreamOf proc
    val () = TextIO.output(toChild, script)
    val () = TextIO.closeOut toChild
    val fromChild = Unix.textInstreamOf proc
    val _ = TextIO.inputAll fromChild
    val () = TextIO.closeIn fromChild
in
    OS.Process.isSuccess(Unix.reap proc)
end;

val stateFile = OS.FileSys.tmpName ();
val linkFile = stateFile ^ ".link";
val truncFile = OS.FileSys.tmpName ();

(* Permissions: 0640 is neither what tmpName nor a new file under umask 022 gets. *)
val () = Posix.FileSys.chmod (stateFile, Posix.FileSys.S.fromWord 0wx1a0);
val () = PolyML.SaveState.saveState stateFile;
val modeKept = modeOf stateFile = 0wx1a0;

(* Through a symbolic link: the link stays and its target is written. *)
val () = let val out = BinIO.openOut stateFile in BinIO.closeOut out end;
val () = Posix.FileSys.symlink {old = stateFile, new = linkFile};
val () = PolyML.SaveState.saveState linkFile;
val linkKept = isLink linkFile andalso sizeOf stateFile > Position.fromInt 0 andalso modeOf stateFile = 0wx1a0;

(* A state whose largest data segment runs past the end of the file: point its
   descriptor's data offset at the last 64K boundary before the end. *)
(* Saved by a child, so that it holds data segments of known size. *)
val savedForTrunc = runChild
    ("val xs = List.tabulate (100000, fn i => (i, Int.toString i));\n\
     \val () = PolyML.SaveState.saveState \"" ^ String.toString truncFile ^ "\";\n");
val () = if savedForTrunc then () else raise Fail "saving the state to truncate failed";
val () =
let
    val inp = BinIO.openIn truncFile
    val all = BinIO.inputAll inp before BinIO.closeIn inp
    val a = Word8Array.tabulate (Word8Vector.length all, fn i => Word8Vector.sub (all, i))
    (* Little-endian fields of the header and the segment descriptors. *)
    fun get (off, n) =
        List.foldr (fn (i, acc) => acc * 256 + Word8.toInt (Word8Array.sub (a, off + i))) 0 (List.tabulate (n, fn i => i))
    fun put (off, n, v) =
        List.app (fn i => Word8Array.update (a, off + i, Word8.fromInt ((v div IntInf.toInt (IntInf.pow (256, i))) mod 256)))
            (List.tabulate (n, fn i => i))
    val descrLength = get (16, 4) and descrs = get (24, 8) and count = get (32, 4)
    fun descr i = descrs + i * descrLength
    (* Segments with data in the file that are neither code nor overwrites. *)
    val candidates =
        List.filter (fn i => get (descr i, 8) <> 0 andalso Word.andb (Word.fromInt (get (descr i + 32, 4)), 0w18) = 0w0)
            (List.tabulate (count, fn i => i))
    val largest = List.foldl (fn (i, j) => if get (descr i + 8, 8) > get (descr j + 8, 8) then i else j) (hd candidates) candidates
    val newOffset = (Word8Array.length a - 1) div 65536 * 65536
    val () = if get (descr largest + 8, 8) > Word8Array.length a - newOffset then () else raise Fail "segment too small"
    val () = put (descr largest, 8, newOffset)
    val out = BinIO.openOut truncFile
in
    BinIO.output (out, Word8Array.vector a);
    BinIO.closeOut out
end;

(* Loading it in a child must raise Fail, not succeed or die of a signal. *)
val truncatedFails = runChild
    ("val () = (PolyML.SaveState.loadState \"" ^ String.toString truncFile ^ "\"; \
     \OS.Process.exit OS.Process.failure) handle Fail _ => OS.Process.exit OS.Process.success;\n");

val () = app (fn f => OS.FileSys.remove f handle OS.SysErr _ => ()) [linkFile, stateFile, truncFile];
if modeKept then () else raise Fail "the save changed the file's permissions";
if linkKept then () else raise Fail "the save replaced the symbolic link";
if truncatedFails then () else raise Fail "loading a state with a segment past its end didn't fail";
