# Proposal: the worker pool is invisible to the OS and to the host

## Why

The library says the caller owns threading:

> All evaluation requests SHALL be plain data (flat buffers) with no per-sample
> allocation; **the caller owns threading and queues.**

It is not true of the CPU backend. `ThreadPool::instance()` is a process-wide
singleton that spawns `hardware_concurrency() - 1` threads the first time anyone
evaluates anything. A host that carefully sized its own pools has no idea this
one exists, cannot size it, cannot prioritise it, and cannot stop it competing
with its own work.

On a tablet three specific things go wrong.

**The workers have no QoS class.** They are plain `std::thread`s, so on Apple
platforms they run at the default QoS. The OS uses QoS to decide core placement,
clock and preemption; workers that never declare one get scheduled by guesswork.
When they are driven from the UI thread — which they are, that is what an
interactive path means — a user-interactive caller ends up waiting on
unspecified-QoS work, which is the textbook shape of a priority inversion.

**Every core is counted as one worker.** `hardware_concurrency()` on an
A-series or M-series SoC returns performance cores plus efficiency cores, and
the pool treats them as interchangeable. The load-balancing change of
2026-08-09 fixed the *scheduling* consequence — a fast worker can now steal from
a slow one — and left the *sizing* one: a chunk placed on an E-core still runs
at E-core speed, and spawning a worker per E-core oversubscribes a device that
is also running the app's UI.

**The join spins.**

```cpp
run(*job);  // calling thread participates
while (job->done.load(std::memory_order_acquire) < num_tasks)
    std::this_thread::yield();
```

Once the calling thread runs out of chunks to claim it burns a core yielding
until the stragglers finish. On a desktop that is waste. On a battery-powered
device with a thermal ceiling it is waste that costs clock for everything after
it — and the thread doing the spinning is usually the one the user is waiting
for.

## What changes

**Workers declare a QoS class** on Apple platforms, defaulting to the class the
work deserves rather than to whatever the runtime picks, and a caller can state
it. Elsewhere the equivalent hint is applied where the platform has one and
skipped where it does not.

**The pool is sized from performance cores**, not from every logical core, with
the count overridable by the host.

**The join blocks instead of spinning** once the calling thread has no work
left, so a finished thread sleeps rather than heating the device.

**The pool becomes configurable and inspectable through the C ABI** — worker
count and QoS at minimum — so the sentence about the caller owning threading
becomes either true or accurately qualified. A host that wants no library
threads at all should be able to say so and get serial execution.

## What it is not

**Not an external executor interface.** Letting a host inject its own scheduler
(dispatch queues, a game engine's job system) is the fuller answer and a larger
one: every batch entry point would take a submission handle and the ownership
rules would have to be stated. This change makes the built-in pool behave and
be configurable; the injection interface is a follow-up that this does not
foreclose.

**Not a change to the chunking policy.** The over-decomposition of 2026-08-09
stands, and `min_chunk` is not lowered — chunks worth less than the claim that
fetches them are a loss and that reasoning has not changed.

**Not a change to any result.** "Every element of a batch is computed exactly
once" is the requirement that governs this code and it is untouched.

## Open questions

- **Which QoS class by default.** `USER_INITIATED` matches a brush dab (the user
  is waiting, it is not a frame deadline). `UTILITY` would be right for
  background refills. These may genuinely be two pools, or one pool that takes
  the class per submission — to be decided in `design.md`, against how the app
  actually drives the library.
- **How to count performance cores portably.** Apple exposes it
  (`hw.perflevel0.logicalcpu`); Linux and Windows have their own answers and
  some have none. The fallback must be stated rather than left to the platform.
- **What "no library threads" means for the batch entry points** — serial on the
  calling thread is the obvious answer and should be tested, because it is also
  what a host debugging a determinism problem will reach for.
- **Whether the pool should shrink when idle.** Holding N sleeping threads for
  the life of the process is cheap in CPU and not free in memory on a device
  that kills apps for memory.

## Impact

`evaluation-backends` gains the scheduling requirements; `c-abi` gains the
configuration surface. No output values change, and a host that configures
nothing gets today's behaviour with better core placement.
