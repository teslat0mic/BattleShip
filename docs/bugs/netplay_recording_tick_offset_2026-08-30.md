# Netplay recordings are labelled one tick early

**Status:** open (worked around on ingest; see below)
**Found:** 2026-08-30, preparing the live controller session
**Affects:** `decomp/src/sys/netinput.c` (`syNetInputFuncRead`), `decomp/src/sys/netsync.c` (trace tick)

## What happens

A netplay match recorded with `SSB64_REPLAY_RECORD` does not reproduce that match when replayed
offline. The two simulations are bit-identical until the countdown ends and then diverge
permanently, on every gated column.

```
live netplay host trace  vs  offline replay of its own recording
  full     first=392   divergent=808
  fighters first=392   divergent=808
  rng      first=406   divergent=794
  result: FAIL
```

Tick 392 is not special in itself: it is the first tick after GO at which the input stream changes
value. Everything before GO is identical because input does not reach the fighters yet.

## Why

The recorded input stream is offset by one tick from the state trace. Shifting the recording
forward by one tick and replaying it reproduces the live match exactly:

```
python tools/shift_replay.py host_inputs.ssb64r shifted.ssb64r 1
  -> replay of shifted.ssb64r vs the live trace: result: PASS   (all 12 columns)
```

So the live simulation at trace tick T consumes the input the recorder stored at index T-1.

The tracer already detects this. `syNetSyncRecordTick` asserts the invariant
`syNetInputGetPublishedTickCount() == tick + 1` and warns once when it breaks; every netplay run
logs

```
SSB64 SyncTrace: WARNING trace tick 0 but netinput published 0 ticks
```

The cause is the netplay start barrier. In `syNetInputFuncRead` the resolve/publish/record loop
runs *before* the barrier and readiness checks:

```c
for (player = 0; player < MAXCONTROLLERS; player++) { resolve; publish; if (recording) record; }
if (syNetPeerCheckStartBarrierReleased() == FALSE) return;   /* no tick++, no published++ */
if (syNetPeerCheckSimulationInputReady(tick) == FALSE) { stalled; return; }
...
sSYNetInputTick++;
```

While the barrier is held the input is published into `gSYControllerDevices` but the tick never
advances, so the first simulated tick consumes a published input that was never counted at that
index. Offline replay has no barrier, so its read and its update stay in step; the same recording
therefore lands one tick later in the replay than it did live.

Stalls do *not* contribute: `scVSBattleFuncUpdate` returns before `syNetSyncRecordTick` when the
input read declined to advance, so a stalled VI adds no trace row. The offset is fixed at one and
comes entirely from the start of the session.

## Impact

- **Live netplay sync is unaffected.** Both peers do exactly the same thing, which is why the
  cross-peer and cross-compiler checks pass. This is a labelling defect, not a simulation defect.
- **Recorded netplay matches cannot be replayed as-is** - the artifact is lost unless the stream is
  shifted.
- **This matters for rollback.** Re-simulating tick T with "the input for tick T" is only correct
  if that mapping is right. An off-by-one between the input tick and the state tick would make
  every rollback re-simulation subtly wrong, in a way the SyncTest would not catch, because the
  SyncTest re-runs a tick with whatever is currently published rather than with an indexed input.

## Workaround in place

`t2/livetest-verify.ps1` shifts every recorded live match by +1 (`tools/shift_replay.py`) before
replaying it, and checks the shifted replay against the live trace. Recorded human matches are
therefore still faithful artifacts.

## Fix

Do not publish input on frames that do not advance the tick: move the resolve/publish/record loop
after the start-barrier check, so the first published input is the first simulated one, and the
tracer's `published == tick + 1` invariant holds in netplay as it already does offline. That
invariant should then be promoted from a warning to a hard failure under `SSB64_RIG_EXIT`, since
nothing else catches this class of bug.
