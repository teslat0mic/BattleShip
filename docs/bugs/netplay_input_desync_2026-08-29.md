# Netplay desynced on the first real input: the input delay was applied to one side only, and a missing input was guessed and never corrected

**Date:** 2026-08-29
**Status:** FIXED (decomp `port-patches`: `sys/netinput.c`, `sys/netpeer.c`, `if/ifcommon.c`, `sc/sccommon/scvsbattle.c`)
**Class:** netcode correctness — three separate defects that were all invisible to a test with a neutral controller

## Symptom

A live two-instance session with a player on one side: the two peers' gameplay-state traces
(`docs/state_trace.md`) were identical through tick 626 and diverged from tick 627 onward — the
first tick the player's stick was not neutral. The desync is silent: no crash, no error, no early
end. Each peer continues a perfectly self-consistent match, and the two screens simply drift into
different fights.

Every automated peer-to-peer test had passed before that, including 1781-tick loopback runs and a
1475-tick Windows-to-Linux run. Those tests ran with no controller attached.

## Root causes

**1. The input delay was applied to the remote peer only.**
`syNetInputMakeLocalFrame()` applied this machine's controller sample immediately, at tick T, while
`syNetPeerBuildPacket()` transmitted that same sample tagged `T + delay`. The two machines
therefore ran the same button press on *different ticks*. This desyncs on the first input that
matters, no matter how good the network is. Proof: with `delay=30` and both peers started
simultaneously, `late=0` on both (nothing arrived late) and the states still diverged — at tick
391, which is the tick the match countdown ends and inputs first reach the fighters.

**2. A missing remote input was guessed, and the guess was never corrected.**
When no confirmed remote frame existed for the tick being simulated,
`syNetInputResolveFrame()` substituted `syNetInputMakePredictedFrame()` (repeat the last input) and
nothing ever re-simulated that tick. Two peers started a few hundred milliseconds apart — normal
for two people pressing start — leave the peer that is ahead missing every deadline.

**3. The battle clock counted VI frames, not simulated ticks.**
`ifCommonTimerFuncRun()` advances `time_passed` / `time_remain` by the delta of
`sySchedulerGetTicCount()`. On hardware one VI is one tick, so the two are the same. Once fix 2
makes a peer wait for input, they are not: the waiting peer burns VIs without simulating, and its
match clock runs ahead of its peer's. This is only observable once fix 2 exists, and it showed up
exactly that way — every other trace column agreed while `battle` and `full` diverged.

## Why the tests missed all of this

Prediction repeats the last input, so a peer whose controller never moves is predicted *perfectly*.
With neutral inputs all three defects are invisible: the sim matches, the guesses are right, and
nobody stalls so the clock never drifts. An automated netplay test with no controller attached
cannot see any of them. This is now addressed by `SSB64_NETPLAY_FAKE_INPUT=<seed>` (see below).

## Fixes

- **netinput.c** — the sample taken at tick T is scheduled as the input for tick `T + delay` on
  this machine too (`sSYNetInputLocalScheduled`), and applied when that tick arrives. The first
  `delay` ticks run neutral on both sides. **netpeer.c** — the packet carries those scheduled
  frames with their final ticks instead of re-adding the delay.
- **netpeer.c / netinput.c / scvsbattle.c** — flow control: `syNetPeerCheckSimulationInputReady()`
  holds the tick when the remote input for it has not arrived. The peer keeps receiving and
  sending while it waits; the peer that is behind always has what it needs, so it keeps running and
  releases the waiting one (no deadlock, and the pair self-aligns to the slower machine).
- **ifcommon.c** — the battle clock subtracts VIs spent waiting, so it advances once per simulated
  tick. The count restarts where the game already restarts the VI counter (`sySchedulerSetTicCount(0)`
  at GO).

Offline play and replays never wait and use delay 0, so all of this is inert outside a netplay
session: the 28-file replay battery is byte-identical to its pre-change traces on both compilers.

## Verification

`SSB64_NETPLAY_FAKE_INPUT=<seed>` generates a deterministic, input-rich local stream in place of the
controller, which is what makes any of this testable without a human. Reproduction before the fix:
diverges at tick 391 with `late=0`. After:

| delay | start offset | ticks compared | result |
|---|---|---|---|
| 2 | 0 ms | 1165 | identical |
| 2 | 700 ms | 1165 | identical |
| 2 | 3000 ms | 1164 | identical |
| 1 | 1500 ms | 861 | identical |
| 0 | 500 ms | 431 | identical |
| 6 | 2000 ms | 1168 | identical |
| 10 | 0 ms | 2373 | identical |

Plus **Windows (MSVC) host to WSL Ubuntu (clang) client over real UDP with synthetic inputs: 1403
ticks identical.**

Note the tick counts at low delay: `delay=0` completed 431 simulated ticks in 1200 host frames
because the peers spend most VIs waiting for each other. That is the cost of correctness in a
delay-based scheme, and it is what rollback (re-simulating from a snapshot when a prediction turns
out wrong) exists to reclaim. The "is this input confirmed" plumbing added here is the same
plumbing rollback needs.

## Audit hook

Anything a netplay peer derives from VI count rather than simulated tick count will drift the
moment the two stop being equal. `grep -rn "sySchedulerGetTicCount" decomp/src` — outside menus,
1P mode and logging, `ifCommonTimerFuncRun` was the only consumer in the VS path.
