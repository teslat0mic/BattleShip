# Bare `sinf()`/`cosf()` call sites use the UCRT on MSVC, the libultra polynomial everywhere else

**Date:** 2026-08-29
**Status:** FIXED (outer repo, `CMakeLists.txt` + `port/msvc_trig.h`) — no decomp change
**Class:** host-libm leak into the simulation → Windows and Linux/macOS simulate projectiles differently → cross-platform netplay desync

## Symptom

Found by the gameplay-state trace (`docs/state_trace.md`) on a 270-replay synthetic corpus, MSVC vs
clang: 23 of 270 replays diverged, 20 of them with the same signature — the `weapons` column (and
only it) differs for 6–9 ticks and then re-converges, `rng` untouched; the spawn tick hashes
identically on both hosts, the **first physics tick** after it differs. That is a projectile whose
velocity differs in the last ulp, living its lifetime and despawning without hitting anything. When
one does hit (`d_hyrule_s4`: `fighters` diverged for 28 ticks) the difference becomes a real one.

The 18-replay T1 sweep never showed it: ~55k ticks, but the projectile-heavy fighters were never
paired the right way for one to be launched into empty space often enough to notice.

## Root cause

`decomp/src/libultra/gu/{sinf,cosf}.c` provide the N64's `__sinf`/`__cosf` and alias the bare names
with `#pragma weak sinf = __sinf`, which GCC/Clang honour: the 16 bare `sinf()`/`cosf()` call sites in
the decomp (`ft/ftphysics.c`, `sys/vector.c`, `sys/matrix.c`, `wp/wppikachu/wppikachuthunderjolt.c`,
`lb/lbcommon.c`, `sys/audio.c`, `sys/objdisplay.c`, `mn/mndata/mncharacters.c`) resolve to the libultra
polynomial, as they did on hardware. **MSVC ignores `#pragma weak`**, so on Windows those sites called
the UCRT's `sinf`/`cosf` — a different implementation with different last-ulp results. The 166
explicit `__sinf`/`__cosf` sites were fine on every host; the CMake comment next to the trig sources
even said so ("MSVC ignores the pragma; those sites keep host libm there") without treating it as
a determinism bug.

## Fix

MSVC-only: `port/msvc_trig.h` is force-included (`/FI`) into every C translation unit of the
`ssb64_game` library. It includes the host `<math.h>` first (so its declarations keep their names),
then declares `__sinf`/`__cosf` and `#define`s the bare names to them — the same routing the weak
alias gives GCC/Clang. `CMakeLists.txt` adds the option in the `else()` branch of the existing
`if (NOT MSVC)` trig block. No decomp change; C++ port code is untouched.

## Verification

Same 23 replays, MSVC rebuilt with the header: 20 of the 23 divergences gone (the remaining 3 were
two other bugs, `ftcommandvars_msvc_layout` and `link_bomb_release_args`). Full 270-file corpus
MSVC vs clang after all three fixes: 270/270 identical on all 12 trace columns. The 26-file
battery is unchanged in verdicts and identical to its previous traces on clang (which already used
the polynomial); on MSVC the traces of projectile-heavy files change, as expected.

## Audit hook

Any libm symbol the decomp calls by its bare name is a determinism hazard on a host that does not
honour the weak alias. Today only `sinf`/`cosf` (and `sqrtf`, which is correctly rounded on every
IEEE host) are used. `grep -rnoE '\b(atan2f|tanf|powf|fmodf|expf|logf)\(' decomp/src` should stay
empty; if one appears, route it the same way.
