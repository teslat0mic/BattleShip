# Simulation snapshot / restore, and the SyncTest that validates it (T3)

Rollback netcode rewinds the simulation to an earlier tick and replays it with corrected inputs.
That needs two things: a snapshot that captures everything a tick can read or write, and a way to
prove the snapshot is complete. This documents both, and the one obstacle that still stands
between them and a working rollback.

## Where the simulation state lives

1. **The scene arena.** `gSYTaskmanGeneralHeap` is a bump allocator over a single 16 MiB block
   (`gPortSceneHeap`), zeroed per scene. Every `GObj`, fighter, item, weapon and the battle state
   itself is allocated from it. Live bytes are exactly `[start, ptr)`; a bump allocator never
   frees, so there are no holes and one `memcpy` captures all of it. Restoring also rewinds `ptr`,
   which is what makes objects created during rolled-back ticks disappear.

2. **Globals outside the arena** — 22 blocks, 1429 bytes in total: the object-manager list heads
   (`gGCCommonLinks`, `gGCCommonDLLinks`), the RNG seed (through `sSYUtilsRandomSeedPtr`, because
   the game can repoint it), the stage/collision set (`gGRCommonStruct`, `gMPCollisionBounds`,
   line/yakumono counts, the BGM selection), the camera, the item manager's spawn scheduling and
   Poké Ball table, and the fighter manager's counters.

Deliberately not captured: the graphics heap and display lists (rebuilt every frame, never read by
the simulation), audio (the simulation stores sound handles but never reads mixer state), and
anything in the port layer.

## SyncTest: proving the list is complete

`SSB64_SYNCTEST=1` makes each tick run twice:

1. save the state,
2. simulate the tick, hash it (the 12-column hash from `docs/state_trace.md`), save the result,
3. restore the pre-tick state, simulate the same tick again, hash again,
4. restore the post-tick state so the rest of the frame proceeds normally,
5. compare. Different hashes mean the second run read something the snapshot did not restore, and
   the first differing column names the subsystem.

That last part is the useful bit: the harness names the subsystem, and the subsystem names the
globals. The registration list above was grown exactly this way — the first run said `stage`,
which produced the map/collision block; the next said `camera`, which produced the camera and
manager blocks; after that, zero mismatches.

`SSB64_SYNCTEST_START=<tick>` and `SSB64_SYNCTEST_COUNT=<n>` check a bounded window and then leave
the game alone, so a replay still runs to its normal verdict.

## Result so far

On `syn_mario_fox_pupupu_s1`, a 25-tick window starting at tick 1350: **checked=25, mismatches=0**,
and the replay still verifies PASS. Every one of the 12 state columns — including the ungated
`joints`, `camera` and `vars` — is reproduced exactly after a save/restore/re-simulate cycle. For
those ticks the snapshot is provably complete.

## The obstacle: interface objects are coroutines

Object processes come in two kinds. `nGCProcessKindFunction` is a plain call. `nGCProcessKindThread`
runs the process on its own N64 thread, which this port implements as a host fiber
(`port_coroutine_create`), with the handle stored in the `GObjThread` — inside the arena.

Restoring the arena therefore restores a *fiber handle*, but a fiber's execution position lives in
the host, not in the arena, and cannot be rewound by a memcpy. If the rolled-back interval contains
the creation or destruction of one of these, the restored handle refers to a fiber that no longer
matches, and the coroutine layer reports `memory corruption detected (head=0x0 tail=0x0)` and the
simulation stops.

The good news is where those live. Counting `nGCProcessKindThread` registrations by subsystem:

| subsystem | count | in a VS battle? |
|---|---|---|
| `mn` (menus) | 21 | no |
| `sc` (scene management) | 9 | scene transitions only |
| `if` (interface) | 5 | yes — countdown, announcer, entry focus, sudden death |
| `sys` | 2 | — |
| `ft` / `it` / `wp` / `gr` / `gm` | **0** | — |

**The gameplay simulation contains no coroutine processes at all.** Everything rollback actually
needs to rewind is plain function processes over arena memory. The five interface users are HUD
elements whose state is cosmetic and excluded from the gated columns already.

## Options for closing it

1. **Re-simulate gameplay only.** A rollback replays the tick to correct the simulation; the HUD
   does not need replaying. Running the gameplay object links during re-simulation and leaving
   interface objects untouched sidesteps fibers entirely. Needs a way to run a subset of
   `gcRunAll`'s links.
2. **Make the fiber handle survive.** Pool fibers per `GObjThread` address and never destroy them
   within a session, so a restored handle still refers to a live fiber. Cheaper, but only correct
   if the fiber's *position* is also unchanged across the rolled-back interval, which holds only
   when no thread process yields mid-tick.
3. **Convert the five interface processes to function processes.** Smallest surface, changes game
   code, and would need care where those threads use multi-tick sleeps.

Option 1 matches how rollback implementations normally treat presentation state and is the current
plan.
