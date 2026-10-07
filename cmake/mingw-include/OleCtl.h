// Windows SDK headers are spelled in mixed case by most Windows code, but
// MinGW-w64 ships them lowercase -- and on a case-sensitive filesystem
// <OleCtl.h> is simply a miss. Forward to the real header.
//
// Generated shim; see cmake/mingw-w64-x86_64.cmake.
#include <olectl.h>
