# A scheduled input could be overwritten after it had been sent

**Status:** fixed
**Found:** 2026-08-30, live controller session
**Affects:** `decomp/src/sys/netinput.c` (`syNetInputMakeLocalFrame`), `decomp/src/sys/netpeer.c`

## What happened

Two live matches against a human desynced. The state trace showed fighter divergence in short
bursts that healed by themselves - 24 ticks of 14459 in the first match, 3 of 12013 in the second -
which is not how a lockstep desync normally behaves.

Recording what *each peer resolved* per tick, rather than only hashing state, split the question in
one run:

```
p0: 2 differing ticks, first at 1353     <- the human's own slot
p1: identical
host stalls: 2
```

One mismatch per host stall, and the state diverged one tick after each.

```
tick 1353   host: x=-20  (stick returning to neutral)   client: x=-81  (the previous value)
tick 2358   host: buttons=8000                          client: 0000  (press dropped)
```

Both peers marked the frame `RemoteConfirmed`, `predicted=0`. The client was not guessing; it was
confidently applying a different input.

## Why

`syNetInputMakeLocalFrame` stored this VI's controller sample into the scheduled ring on **every
VI**, keyed `tick + delay`:

```c
syNetInputMakeFrame(&sample, tick + delay, controller->button_hold, ...);
syNetInputStoreFrame(sSYNetInputLocalScheduled, player, &sample);
```

A stalled VI does not advance the tick, so the next VI rewrote the slot for that same future tick
with a fresher controller reading - after the older value had already been transmitted. The peer
applied what it received; this side applied the overwrite.

It matters in real play: it corrupts input exactly when the network is struggling, and a dropped
press on a stock-losing frame is a lost stock.

## Why no automated test could have found it

`syNetInputMakeFakeFrame` is a pure function of `(seed, player, tick)`. Re-scheduling the same
target tick therefore rewrites an **identical** value, and the overwrite is invisible by
construction. Six automated P2P configurations and a 7/7 delay matrix passed over this bug. It
needs input that varies between the VIs of one tick - which means a human, or a recorded human.

## Fix

Schedule each tick once (`sSYNetInputScheduledTick[player]`): the first sample taken for a tick is
the one sent and the one applied.

Two further defects fell out of the same investigation:

- **Fake input filled every controller slot**, not just the one the peer owns, so two peers ran
  with different controller arrays for a whole match (12010 of 12012 ticks differing on the CPU
  slots). Harmless in play - CPU fighters read `fp->input.cp`, never the controller devices - but
  it is noise in exactly the comparison meant to detect desyncs, and it sent one investigation down
  a wrong path before the input diff settled it.
- **The outgoing frame window came from the last published tick.** Once publishing was gated to
  VIs that advance a tick, a waiting peer's window stopped moving; at delay 0 that is the same
  number as the tick being waited for, so neither peer ever sent it and both hung. The window now
  follows `syNetInputGetTick() + delay`, the tick just scheduled.

## Verification

Against the recorded human match, replayed as a two-peer netplay session
(`SSB64_NETPLAY_LOCAL_INPUT`):

| run | host stalls | input mismatches | peer sync |
|---|---|---|---|
| before the fix | 2 | 2 | desync at 1354 |
| after, delay 2 | 3 | 0 | PASS |
| after, delay 0 | 136 | 0 | PASS |

136 stalls with zero mismatches, where the rate before was one mismatch per stall. Delay sweep
0/1/2/6 all pass with identical resolved inputs on both peers. Offline battery: 53/53 identical to
the previous reference, 53/53 identical across MSVC and clang.
