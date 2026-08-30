/* msvc_trig.h — force-included (/FI) into every C translation unit of the game
 * library on MSVC only. See CMakeLists.txt next to the libultra trig sources.
 *
 * decomp/src/libultra/gu/{sinf,cosf}.c provide the N64's __sinf/__cosf and
 * alias the bare names with `#pragma weak sinf = __sinf`, which GCC/Clang
 * honour: every bare sinf()/cosf() call in the decomp (ftphysics.c, vector.c,
 * matrix.c, wppikachuthunderjolt.c, ...) resolves to the libultra
 * polynomial, as it did on hardware. MSVC ignores #pragma weak, so those
 * sites silently called the UCRT's sinf/cosf instead — a different
 * implementation with different last-ulp results. The gameplay-state trace
 * (docs/state_trace.md) caught it as MSVC-vs-clang divergences in the
 * `weapons` and `fighters` columns on projectile-heavy replays: identical
 * on the spawn tick, different on the first physics tick.
 *
 * The host <math.h> is included first so its own declarations keep their
 * real names; afterwards the bare names become the libultra symbols. */
#ifndef PORT_MSVC_TRIG_H
#define PORT_MSVC_TRIG_H
#ifdef _MSC_VER
#include <math.h>
float __sinf(float);
float __cosf(float);
#define sinf __sinf
#define cosf __cosf
#endif
#endif
