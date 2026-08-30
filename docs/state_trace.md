# Gameplay state trace (`SSB64_SYNC_TRACE` / `SSB64_SYNC_VERIFY`)

Purpose: prove, per tick, that the VS simulation evolved identically — across two runs on one
machine, across compilers/platforms, and (later) across peers. Replay verification
(`SSB64_REPLAY_PLAY`) only proves the *inputs* were fed identically; this proves the *state*.
It is the oracle every later netcode step (snapshot/restore, SyncTest, rollback) is checked
against.

## What it records

Once per simulated tick, right after `ifCommonBattleUpdateInterfaceAll()` in
`scVSBattleFuncUpdate()` (end of the simulation step, before `syNetReplayUpdate()`),
`syNetSyncRecordTick()` computes twelve FNV-1a 32-bit hashes and writes one text line:

```
# ssb64h v2 tick full rng battle fighters items weapons stage objman input joints camera vars
0 809832C5 53589704 D5403D9E D8CDFA18 9DDCA10B 050C5D1F 41B3C8F1 44A42265 06481D31 3D1D6252 8746A390 CEEABA37
```

| column | covers | gated |
|---|---|---|
| `rng` | `syUtilsRandSeed()` and whether the seed pointer still targets the default seed | yes |
| `battle` | `SCBattleState` incl. every `SCPlayerData` (scores, stocks, stale-move queue), `gFTManager*` counters | yes |
| `fighters` | every non-union scalar of `FTStruct`, physics, `MPCollData` (incl. the TopN translate it points at), timers, all 45 bitfields by name, inputs, `FTComputer`, attack/damage collisions incl. hit records, motion-script cursors, part status, callbacks as non-null bits | yes |
| `items` | `ITStruct` likewise, the item spawner (`gITManagerAppearActor`), random-weight scalars, the Poké Ball roll queue (`gITManagerMonsterData`) | yes |
| `weapons` | `WPStruct` likewise | yes |
| `stage` | `gMPCollisionBounds`, update tic, line/yakumono counts, yakumono speeds and poses | yes |
| `objman` | per gameplay link (1–5): object count and XOR of object keys | yes |
| `full` | fold of the gated columns | yes |
| `input` | the published-input checksum netreplay verifies — recorded for cross-reference | no |
| `joints` | `DObj` pose of every fighter joint (translate/rotate/scale/anim) | no |
| `camera` | `GMCamera` scalars | no |
| `vars` | the per-kind unions (`status_vars`, `passive_vars`, `item_vars`, `weapon_vars`, `GRStruct`) hashed word-wise with pointer slots masked from a generated table (`netsyncvars.inc`) | no |

Rules the hasher follows: no address is ever hashed — a `GObj *` becomes a stable object key
(player slot for fighters, per-match creation serial for items and weapons; `gobj->id` is only
the object *kind*), any other pointer becomes a non-null bit; floats are hashed by bit pattern;
bitfields by name (layout-independent); linked lists are accumulated per slot / per key and
merged order-independently. `joints` and `camera` are kept out of `full` so a divergence there is
attributed rather than folded into `fighters`; `vars` is kept out until layout equality across
builds is demonstrated, because it doubles as a layout probe.

Deliberately not hashed (render/audio only): `attack_matrix`, `colanim`, `afterimage`,
`magnify_pos`, `arrow_gobj`, sfx handles/ids, `display_mode`, fog/shade colors. Not yet covered:
map vertex data, effects.

## Usage

```
SSB64_SYNC_TRACE=<path.ssb64h>     write the trace for the next VS match
SSB64_SYNC_VERIFY=<path.ssb64h>    load a trace and compare every tick as the match runs
SSB64_SYNC_DUMP_TICK=<n>           log per-fighter group hashes and hit records at ticks n..n+2
                                   and, for every live item, its identity, position/velocity bit
                                   patterns, timers, the whole ITAttackColl and every attack record
```

Rig knobs (port layer, `port/gameloop.cpp` / `port/port.cpp`):

```
SSB64_RIG_FAST=1        headless fast-forward: display lists are dropped before Fast3D, the
                        idle-present/sleep fallback is skipped, audio is synthesized but not
                        queued. Nothing paces the loop, so a replay runs at CPU speed
                        (1800 ticks ≈ 1.7 s incl. boot on this box, was 31 s).
SSB64_RIG_HEADLESS=1    hide the game window (every SDL window of the process, or this thread's
                        Win32 windows on the DXGI backend); cosmetic; only exercised together
                        with SSB64_RIG_FAST=1
SSB64_LOG_PATH=<file>   write the log there instead of <pref dir>/ssb64.log (line-buffered;
                        an unopenable path falls back to the default and says so on stderr)
```

Why fast mode is safe, and its scope: the only 60 Hz pacing is the backend frame limiter inside
`DrawAndRunGraphicsCommands` / `PresentCurrentFramebuffer` (`SwapBuffersBegin` →
`SyncFramerateWithTime`) plus the idle-present fallback in `PortPushFrame`; fast mode never
reaches either. The VI/SP/DP message cadence the game sees is unchanged — one simulated vblank per
`PortPushFrame`, gfx completions one VI later with deferral N=1 (`port_get_last_dl_defer_n()`
short-circuits in fast mode; no DL is walked, so there is no cost to model). Gameplay never reads
wall-clock time (`osGetCount` deltas feed debug counters only). **Scope:** "identical to real
time" holds outside the RCP-freeze allowlist (`port_scene_wants_freeze_simulation`:
Opening/Ending/AutoDemo scenes). There real time uses N=2..3 for heavy display lists, which delays
game ticks against `sSYSchedulerTicCount` (it feeds `time_passed`); fast mode does not reproduce
those authored freezes and logs once if it finds itself in such a scene. VS battle — where every
replay boots — is not in the allowlist. Fast mode also never walks a display list, so a fast PASS
says nothing about a real-time crash in the DL walk. **Parallel instances need
`SSB64_RIG_EXIT=1`:** the clean-exit path saves the shared config file from every instance.

Log lines (`SSB64 SyncTrace:`): `sizeof ...` layout probe at startup, `trace start/wrote`,
`verify start`, `FIRST DIVERGENCE tick=N column=<name> expected=… actual=…` once per column
(with `(not gated)` where applicable), and the summary
`verify ticks=… compared=… divergences=… first_tick=… first_mask=… result=PASS|FAIL`.

With `SSB64_RIG_EXIT=1` the replay verdict becomes the exit code: 0 PASS, 1 input FAIL,
2 INCOMPLETE, 3 LOADFAIL (replay or verify trace could not be loaded), **4 DESYNC** (inputs
replayed identically but a gated state column diverged, or the verify trace was shorter than the
run). A diverged trace wins over INCOMPLETE.

The header's `v<N>` is the hasher version: bump it whenever the hasher changes what it hashes, and
regenerate reference traces — traces from two versions are not comparable (the rig's `tracediff.py`
refuses to compare them).

Offline: `tools/tracediff.py a.ssb64h b.ssb64h` (in the BattleShip-dev rig) reports the first
divergent tick per column and exits 1 on any gated difference.

## Typical loop

1. Record a replay (`SSB64_REPLAY_RECORD`) or generate one from metadata.
2. Replay it with `SSB64_SYNC_TRACE` on build A.
3. Replay it with `SSB64_SYNC_VERIFY=<that trace>` on build B (other compiler, other machine,
   or the same build again). Exit 0 = identical simulation; exit 4 + the `FIRST DIVERGENCE`
   line names the tick and subsystem.
4. Re-run build B with `SSB64_SYNC_DUMP_TICK=<tick>` on both sides and diff the `SyncDump`
   lines to find the field.

## Results so far (2026-08-29)

- Same build, two runs: identical on all 12 columns (MSVC and clang).
- MSVC 14.43 (Windows) vs clang 18 (Linux): identical on **all 12 columns** — gated and ungated,
  including `vars` — on the 4-file corpus (~10k ticks) and on an 18-file sweep covering all 9 VS
  stages, all 12 fighters, items on, random human inputs (18 × 1800 ticks). `vars` matching means
  the per-kind unions have the same word layout under both compilers; `FTCommandVars` (below) is
  the known outlier and is hashed by field name.
- Sensitivity control: replaying a recording with one button flipped at tick 900 against the
  clean trace reports `FIRST DIVERGENCE tick=900 column=fighters`.
- Layout probe: the two compilers lay out several structs differently (`FTStruct` 3768/3760,
  `ITStruct` 1216/1200, `SCBattleState` 520/512, `union FTCommandVars` 20/16 — MSVC starts a new
  storage unit when bitfield types change). The simulation still matches because the code
  accesses fields by name; the exception is `FTItemThrowFlags` (see `docs/bugs/`), where the
  `item_throw` bitfield view of `FTCommandVars` matches the script-written `flags` words only
  under the N64's MSB-first allocation — both PC hosts read wrong bits, each differently.
- What the trace cannot see: two hosts that are *consistently* wrong in the same way (or in
  ways a given replay never exercises) still match. The trace proves "same as the other build",
  not "same as the N64"; the bug above was found by reading, and its verification needs the ROM.
- **Fast mode (`SSB64_RIG_FAST=1`) vs real time: identical on all 12 columns** on all 26 corpus
  replays (8-file battery incl. the 4646-tick early-end file + 18-file all-stage sweep, ~50k
  ticks) on both MSVC and clang — against the real-time traces of the previous build and against
  real-time runs of the same binary. All battery verdicts (PASS/FAIL/INCOMPLETE/LOADFAIL) are
  unchanged. Eight instances run in parallel (`SSB64_RIG_HEADLESS=1`, per-process
  `SSB64_LOG_PATH`, `SSB64_RIG_EXIT=1`) produce the same traces as sequential runs. Whole 26-file
  sweep at 8-way parallel: 22 s on MSVC, 37 s on clang (was ~14 min per side).
- A hashed field written from a draw proc (`is_magnify_show`, `ftDisplayMainProcDisplay()`)
  produced isolated single-tick `fighters` divergences between hosts when the renderer dropped
  a draw pass (`syTaskmanRunTask` skips `task_draw` when no gfx context is free). Rule: fields
  written from any `*ProcDisplay` are never hashed in a gated column.
