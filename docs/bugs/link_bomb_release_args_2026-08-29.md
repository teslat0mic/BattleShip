# Link's bomb detonating in hand passes garbage `stat_flags`/`stat_count` to `itMainSetFighterRelease()`

**Date:** 2026-08-29
**Status:** FIXED on PC (decomp `port-patches`, `src/it/itfighter/itlinkbomb.c`, `#ifdef PORT`); original ROM behaviour preserved for the N64 build
**Class:** original-game bug (wrong prototype → uninitialised argument registers) → host-compiler-dependent state → cross-platform netplay desync

## Symptom

State trace, MSVC vs clang, 270-replay corpus: three replays diverge in the `items` column while a
held Link bomb explodes, one transiently (`c_sector_s5`, 6 ticks), one for the rest of the match
(`t_zebes_s4`, `fighters` too). `SSB64_SYNC_DUMP_TICK` isolates it to `ITStruct.attack_coll`; the
extended item dump isolates it further to `attack_coll.stat_flags.halfword` — `attack_id` and the
named flags agree, only the union's three `unused` bits differ (`0x4039` vs `0x6039`). The value also
changed from build to build on the same host: uninitialised memory.

## Root cause

`itlinkbomb.c` declares, on purpose (it is what the ROM does),

```c
// WARNING: Intentionally erroneous declaration. Missing two u16 arguments after f32. HAL's mistake, not mine.
extern void itMainSetFighterRelease(GObj*, Vec3f*, f32);
```

and calls it with three arguments when the fuse runs out in the holder's hand. The callee
(`itmain.c`) reads `u16 stat_flags, u16 stat_count` from the argument registers / stack, which hold
whatever the caller left there — on the N64 a pointer in `a3`, on x86-64 different leftovers under
MSVC and clang. The values land in the exploding bomb's `attack_coll.stat_flags/stat_count` and, on
hit, in the victim's `FTStruct.stat_flags` via `ftParamSetStatUpdate()`. In VS mode they only feed
the 1P-game bonus bookkeeping, so gameplay is unaffected; but the state differs between hosts and
would differ between two netplay peers.

## Fix

Under `#ifdef PORT` the real prototype is used and the call passes the holder's current
`stat_flags.halfword` and `stat_count`, as every other release path does. The N64 build keeps the
erroneous declaration for byte-identical matching.

Companion hasher change (`netsync.c`): `GMStatFlags` is hashed by its named fields, never the raw
`halfword` — the `unused` bits are never written by name and carry stack garbage whenever a local
`GMStatFlags` is filled by name and copied whole.

## Verification

The three replays agree on both hosts after the fix; 270/270 corpus identical (with
`msvc_trig_routing` and `ftcommandvars_msvc_layout` also applied).

## Audit hook

Every "intentionally erroneous declaration" in the decomp is a place where the ROM read
uninitialised registers. `grep -rn "Intentionally erroneous" decomp/src`. Each needs a PORT-side
deterministic stand-in, or the port inherits whatever the host compiler left in the registers.
