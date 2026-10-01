// Force-included into every Box3D source. Box3D is C17; MSVC before 19.28
// (Visual Studio 16.8) has no /std:c11, so its C mode lacks _Static_assert,
// _Alignas, and the restrict keyword. A negative array size fails the same
// checks at compile time, __declspec(align) aligns a member as _Alignas does,
// and __restrict is MSVC's restrict.
#pragma once

#if defined(_MSC_VER) && !defined(__clang__) && !defined(__cplusplus) && _MSC_VER < 1928
// The CRT declares its allocators __declspec(restrict), which the restrict
// macro below would break, so every system header Box3D includes comes
// first. Their include guards keep Box3D's own includes from reading them again.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <assert.h>
#include <crtdbg.h>
#include <float.h>
#include <inttypes.h>
#include <intrin.h>
#include <limits.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#if defined(_M_X64) || defined(_M_IX86)
#include <emmintrin.h>
#include <xmmintrin.h>
#endif

#define AE_B3_CAT2(a, b) a##b
#define AE_B3_CAT(a, b) AE_B3_CAT2(a, b)
#define _Static_assert(condition, message) \
    typedef char AE_B3_CAT(ae_b3_static_assert_, __LINE__)[(condition) ? 1 : -1]
#define _Alignas(alignment) __declspec(align(alignment))
#define restrict __restrict
#endif
