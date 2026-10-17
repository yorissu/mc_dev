
/// 128-bit integers as two 64-bit halves, the code msvc builds; forced here
/// so every build tests it. this file includes nothing that passes a
/// mc_i128_s to the rest of the compiler, whose type differs.

#define MC_INT128_PORTABLE
#define MC_INT128_SUITE int128_portable

#include "int128_cases.h"
