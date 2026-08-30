# `FTItemThrowFlags` reads the wrong bits on every host compiler (`union FTCommandVars`: 20 bytes on MSVC, 16 on clang, 16 on the N64)

**Date:** 2026-08-29
**Status:** FIXED on PC (decomp `port-patches`, `ft/ftcommon/ftcommonitemthrow.c`, `#ifdef PORT` accessors reading the N64 bit positions from the `flags` words; the bitfield struct is kept for the N64 build) — found by review while building the state trace; confirmed by the state trace on a 270-replay corpus (`d_castle_s2`: full simulation divergence at tick 6535 when a CPU Link threw a bomb; gone after the fix). Not yet compared against the ROM
**Class:** compiler-dependent bitfield layout in a union that is written through one view and read through another → scripted item throws use the wrong damage/velocity/angle on both PC hosts (differently)

## Symptom

Not visible in the 18-replay determinism sweep (no scripted item throw was exercised) — and the
two hosts would each be *consistently* wrong, so a trace between them would not necessarily
show it. It surfaces in the state trace's startup layout probe:

```
SSB64 SyncTrace: sizeof FTStruct=3768 ITStruct=1216 WPStruct=864 MPCollData=224 FTCommandVars=20 ... SCBattleState=520   (MSVC 14.43)
SSB64 SyncTrace: sizeof FTStruct=3760 ITStruct=1200 WPStruct=864 MPCollData=224 FTCommandVars=16 ... SCBattleState=512   (clang 18)
```

## Root cause

`FTStruct.motion_vars` is a union of two views of the same bytes:

```c
union FTCommandVars {
    struct FTCommandFlags   { u32 flag0, flag1, flag2, flag3; } flags;
    struct FTItemThrowFlags { sb32 is_throw_item; u8 unk1; u32 damage : 24; u8 unk2; u32 vel : 12; s32 angle : 12; } item_throw;
} motion_vars;
```

Motion scripts write through the `flags` view (`ft/ftmain.c`, `nFTMotionEventSetFlag0..3`: `flag1`
carries the damage word, `flag2` the vel/angle word). `ft/ftcommon/ftcommonitemthrow.c:57-66` reads
through the `item_throw` view. The two views only agree under the N64 compiler's rules:

- **N64 (MIPS, big-endian, IDO):** `unk1` and `damage:24` share the 32-bit unit at bytes 4–7 with
  bitfields allocated from the MSB, so `damage == flag1 & 0xFFFFFF`; `unk2`, `vel:12`, `angle:12`
  share bytes 8–11: `vel == (flag2 >> 12) & 0xFFF`, `angle == flag2 & 0xFFF`. 16 bytes.
- **clang/GCC x86-64 (little-endian, SysV):** same 16-byte size, but bitfields allocate from the
  LSB: `unk1` is bits 0–7 of word 1, so `damage == flag1 >> 8`, `vel == (flag2 >> 8) & 0xFFF`,
  `angle == flag2 >> 20`. Linux reads shifted values. There is no `IS_BIG_ENDIAN` reversal of this
  struct in `fttypes.h` (unlike `FTAnimDesc`, `GMStatFlags`).
- **MSVC:** a bitfield after a member of a different type starts a new storage unit, so `damage`
  lands at bytes 8–11 (`flag2`'s word) and `vel/angle` at bytes 16–19 — still inside the
  20-byte union, in bytes the `flags` view never writes. Windows reads a different kind of garbage.

The same MSVC rule also inflates `ITStruct` (+16) and `SCBattleState` (+8); those are harmless
because their bitfields are only accessed by name.

## Fix (upstream decision)

The reads must reproduce the N64 bit positions regardless of host layout. Make the
`item_throw` view explicit words with accessors — e.g. in `ftcommonitemthrow.c`
`damage = flags.flag1 & 0xFFFFFF`, `vel = (flags.flag2 >> 12) & 0xFFF`, `angle` sign-extended from
`flags.flag2 & 0xFFF`, `is_throw_item = flags.flag0` — and delete the bitfield struct, or keep it
only for the N64 build. A `_Static_assert(sizeof(union FTCommandVars) == 16)` alone would make
Windows match Linux's *wrong* answer; add it after the accessor fix so the layout cannot drift
again.

## Verification (2026-08-29, after the fix)

Reachability was measured, not assumed. A temporary probe on the motion-script `SetFlag1`/
`SetFlag2` events across a 289-replay corpus recorded 26,703 flag writes, but almost all carry
small boolean-ish values. Exactly one packed word reached a throw status:

```
set2 fkind=9 (Pikachu) player=0 status=104 (LightThrowDrop) motion=90 value=0x0101EFB0
```

For that word the three hosts disagree: the N64 bit positions give `vel = 0x1EF (495)` and
`angle = sign-extend12(0xFB0) = -80`; clang's LSB-first bitfield view gives the same `vel` but
`angle = 0x0101EFB0 >> 20 = 16`; MSVC's 20-byte union reads `vel`/`angle` from bytes 16-19, i.e.
outside the words the script wrote. The replay that produces this event is `d_castle_s2` in the
generated corpus - the same file whose entire simulation diverged between MSVC and clang from
tick 6535 (every gated column) before the fix and matches exactly after it. So the bug is real,
reachable in ordinary CPU play, and the fix is what closes that divergence.

A 60-second human recording of deliberate item throwing (`corpus/human/item_throws_1.ssb64r`,
Saffron, 3 CPUs, 57.8% of frames with controller input) never triggers the path: a probe inside
`ftCommonItemThrowProcUpdate` logged zero events with `flag1`/`flag2` set, and traces with and
without the fix are identical for that file. Ordinary player item throws take their damage and
velocity from item attributes; only certain scripted throw motions pack them into `motion_vars`.
That is why the 18-replay sweep never saw this and why the 270-replay corpus did.

## Verification plan

A corpus replay in which a human slot smash-throws items (scripted `flag1/flag2`). Compare the
resulting item velocities/damage against the N64 ROM under an emulator for the same inputs (the
two PC hosts agreeing with each other is not enough here).

## Audit hook

Any union written through one member and read through another, where a member mixes bitfields
with plain members or mixes bitfield base types, is host-compiler-dependent. Grep for
`motion_vars.`, `item_vars.`, `weapon_vars.` accesses that alternate views, and for
`u8 x; u32 y : n;` sequences inside unions; check each against the N64 (MSB-first) bit positions.
