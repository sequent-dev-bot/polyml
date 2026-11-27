(* makestring, and so exnMessage, must handle every pretty constructor a
   user-installed printer can return, not only those the compiler builds. *)
datatype t = T;
fun pp _ _ T =
    PolyML.PrettyBlock (0, false, [],
        [PolyML.PrettyString "a", PolyML.PrettyLineBreak,
         PolyML.PrettyStringWithWidth ("b", 1)]);
val () = PolyML.addPrettyPrinter pp;
exception E of t;

PolyML.makestring T = "a\nb" orelse raise Fail "wrong";
General.exnMessage (E T) = "E (a\nb)" orelse raise Fail "wrong";
