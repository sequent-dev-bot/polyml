// ELF fault injection for NewCodeSpace's metadata-registration failure path.
// Build: c++ -shared -fPIC CodeAllocationFailure.cpp -o reject-code-space.so
// Preload the shared object while poly compiles a declaration.  It should
// reject compilation with an ML exception, rather than dereference a null CodeSpace.
#include <cstdio>

extern "C" bool _ZN6MemMgr12AddCodeSpaceEP9CodeSpace(void*, void*)
{
    std::fputs("rejected code area\n", stderr);
    return false;
}
