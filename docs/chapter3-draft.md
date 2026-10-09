# Chapter 3 — System Architecture

> **Draft status.** Structural first draft. Markers used throughout:
> `[VERIFY]` a figure taken from session logs that must be re-derived from a recorded run
> before it enters the thesis; `[TABLE 3.x]` / `[FIG 3.x]` artefacts still to be produced.

---

## 3.1 Design constraints and the decision budget

The architecture is determined almost entirely by four constraints, three of which follow
from the instrument rather than from any design preference.

**3.1.1 A decision has value only while the molecule remains in the pore.** The Read Until
API delivers accumulated signal for each active channel at a fixed cadence, nominally every
0.4 s, and permits a client to request ejection of the molecule currently occupying a given
channel. A correct ejection removes a molecule before the sequencer commits further
capacity to it; an ejection issued after the molecule has already translocated is not merely
late but worthless, because the resource it was intended to reclaim has already been spent.
Adaptive sampling is therefore a soft-real-time problem in the standard sense: utility is a
decreasing function of latency with an effective cut-off, not a continuous function of
throughput. This observation motivates reporting decision latency distributions and
deadline-miss counts in Chapter 5, rather than the throughput figures conventional in the
mapping literature.

**3.1.2 Concurrency is dictated by the flow cell, not chosen.** A MinION flow cell presents
512 channels and a PromethION 2675. Each is an independent decision context: a chunk
arriving on one channel carries no information about any other. The system must therefore
maintain per-channel state for every active channel simultaneously, and the arrival process
is the superposition of up to 2675 independent, approximately periodic streams.

**3.1.3 The aggregate arrival rate fixes the per-chunk compute budget.** With `C` active
channels and a chunk interval `T_c`, the aggregate arrival rate is

&nbsp;&nbsp;&nbsp;&nbsp;λ = C / T_c

so for `C` = 512 and `T_c` = 0.4 s, λ = 1280 chunk/s. With `W` worker threads the mean
service time that keeps the system stable (utilisation ρ = λ·S/W < 1) is bounded by

&nbsp;&nbsp;&nbsp;&nbsp;S < W / λ = W·T_c / C

For `W` = 2 and the figures above, `S` < 1.56 ms per chunk. The measured median per-chunk
decision cost of the frozen configuration is 134–174 µs `[VERIFY]`, giving ρ ≈ 0.09–0.11 at
two workers. The observed sustained rate in the live rig was 975 chunk/s with zero chunks
dropped `[VERIFY]`, consistent with that utilisation. The practical consequence is that the
data plane is not the capacity constraint; the matching stage is, and Chapter 7 quantifies
why.

**3.1.4 Memory must be bounded in the number of channels, not the number of reads.** A
sequencing run produces an unbounded number of reads; it presents a fixed number of
channels. Any per-read allocation therefore grows without bound over a run, and any
structure whose size depends on how much signal a read has accumulated is unbounded in
principle. Every structure described in this chapter is sized as O(C) with per-channel
constants fixed at compile time, and the decision path performs no dynamic allocation after
initialisation. Section 3.6 gives the resulting byte budget.

---

## 3.2 Ingestion: single-copy admission, zero-copy propagation

**3.2.1 What the transport boundary does not permit.** The client communicates with MinKNOW
over gRPC with TLS, consuming a server-streaming `get_live_reads` response. gRPC
deserialises each response into protobuf message objects; the signal payload is materialised
in protobuf-owned storage as a byte string. It is therefore not possible to be zero-copy
*through* the transport: at minimum, the bytes are copied from the wire buffer into message
storage by the protobuf runtime. The claim made here is correspondingly narrower and
precise: **ingestion incurs one copy, performed by the protobuf runtime into arena-backed
storage, and no further copy of signal occurs at any later stage** — not during sharding,
not during the decision, and not during action dispatch.

**3.2.2 Arena-backed message storage.** Each response is deserialised into a protobuf arena
rather than onto the general-purpose heap. Two properties matter. First, deallocation is
performed by resetting the arena, which is a pointer reset rather than a traversal of
individual objects, so the cost of releasing a response does not depend on the number of
reads it contained. Second, and more importantly for what follows, the arena gives signal
payloads a *single, explicit owner with a well-defined lifetime*, which is what makes it
safe to pass pointers into that storage to other threads instead of copying the signal.

**3.2.3 `ChunkRef`: a borrowed view sized to one cacheline.** Downstream stages receive a
descriptor, not data. `ChunkRef` holds the absolute sample position, a pointer and length
for the read identifier, a pointer and length for the signal payload, the owning arena slot,
a timestamp, the channel, the reported chunk length, and the payload type.

Two layout decisions are worth recording. In declaration order natural to the problem the
struct occupies 72 bytes; reordered so that the 64-bit members precede the narrow tail it
occupies 56. A static assertion fixes the invariant that `sizeof(ChunkRef) ≤ 64`, i.e. that
a descriptor never straddles a cacheline. Because descriptors are the element type of the
handoff queues (§3.4), a descriptor spanning two lines would double the coherence traffic of
every enqueue and dequeue; the assertion makes the regression a compile error rather than a
performance mystery. The struct is additionally asserted trivially copyable, which is what
permits the ring to move elements with a plain store.

The timestamp field is stamped immediately after the streaming `Read()` call returns, and
therefore defines the origin of every latency measurement in Chapter 5. Placing the origin
at the transport boundary rather than at the start of the decision function means the
reported latency includes queueing delay inside the system and excludes only the network
and server, which is the division that corresponds to what the implementation controls.

`[TABLE 3.1]` ChunkRef field layout, widths and offsets, with the 72-byte and 56-byte
orderings side by side.

**3.2.4 Disagreement between reported and actual payload length is a first-class signal.**
MinKNOW reports a chunk length in samples independently of the payload it transmits. The
implementation computes the sample count implied by the payload byte length and the sample
width, and exposes both the computed value and a predicate for agreement. Where the two
disagree the payload is trusted and the discrepancy is counted rather than reconciled
silently: a short payload indicates a truncated or dropped frame, which is information about
the transport that should surface in run statistics instead of being absorbed into an
apparently successful decision.

---

## 3.3 Lifetime management: the arena slot reference-counting protocol

Passing pointers into arena-owned storage across threads replaces a copying cost with a
lifetime obligation. The protocol that discharges it is the most subtle part of the data
plane and is set out here in full.

**3.3.1 The protocol.** A fixed pool of arena slots is allocated at startup `[VERIFY: 32
slots]`. The reader acquires a slot, deserialises one response into it, constructs a
descriptor per chunk, and pushes each descriptor to the shard selected by its channel. A
worker that has finished reading a chunk's signal releases the slot. The release that drives
the count to zero resets the arena and returns the slot to the free list.

**3.3.2 The reader holds a reference, and the race that requires it.** `acquire()` returns a
slot whose count is already one, and that reference belongs to the reader, which must release
it exactly once after it has finished dispatching every descriptor derived from the response.

The reference is not bookkeeping convenience; omitting it is incorrect. Suppose the reader
pushed descriptors and took no reference of its own. A worker on the first shard may dequeue,
decide and release before the reader has pushed the descriptor for the second shard. If that
release drives the count to zero, the arena is reset and the signal storage is recycled while
the reader is still reading it to construct the remaining descriptors. The reader's reference
makes the count strictly positive for the whole dispatch window, and the hazard is eliminated
by construction rather than by assuming the reader is faster than the workers — an assumption
that is false precisely under the load where it matters.

**3.3.3 Verification by sanitiser, with an injected oracle.** A refcount protocol that is
wrong intermittently is worse than one that is wrong deterministically, because the symptom
is corrupted signal that still produces a plausible decision. The stress test therefore makes
the fault observable in two independent ways. The reader writes a known magic value into the
arena and hands out spans over it; workers verify that value. If the protocol permitted a
reset while a span was still live, the magic value would be clobbered and the check would
fail. Independently, the test is run under AddressSanitizer, under which a premature reset
manifests as a use-after-free at the point of access rather than as silent corruption. The
pool is deliberately starved relative to the number of workers `[VERIFY: 8 slots, 4 workers]`
so that the handoff runs at saturation. The test further asserts that no slot leaks and that
each acquisition is matched by exactly one reset, the latter using a per-slot cycle counter.

**3.3.4 A concept rather than an interface.** The pool is generic over any arena type
exposing a `Reset()` method, expressed as a C++20 concept. `google::protobuf::Arena`
satisfies it in production; a minimal bump allocator satisfies it in tests. This allows the
concurrency properties of the pool to be tested without linking protobuf or gRPC, which
matters because the sanitiser configurations are the ones in which the protocol is actually
validated and those builds are substantially faster and simpler without the generated
transport code. The abstraction is static, so it costs no indirection on the decision path.

`[FIG 3.1]` Arena slot state machine: free → acquired (count 1, reader) → dispatched (count
1 + n pushed) → draining → reset → free.

---

## 3.4 Chunk handoff: lock-free single-producer, single-consumer rings

**3.4.1 Sharding converts a multi-producer problem into several single-producer ones.** A
single reader thread drains the gRPC stream and is the only producer of descriptors. Rather
than feed one multi-producer/multi-consumer queue, descriptors are routed to one of `N`
rings by `channel & (N − 1)` with `N` a power of two, and each ring has exactly one consumer.
The topology is therefore single-producer/single-consumer per ring, which admits a
substantially cheaper implementation than the general case: no compare-and-swap is required
on either side, and the only synchronisation is a pair of release/acquire counter updates.

The mapping has a second consequence that is exploited throughout the system. Because a
channel's chunks always land on the same shard, all per-channel state is touched by exactly
one worker thread and requires no synchronisation of its own. The decision structures of
Chapter 4 are single-threaded data structures as a result, and their correctness argument
does not involve concurrency at all.

**3.4.2 Free-running counters and usable capacity.** Head and tail are 64-bit free-running
counters, masked to index a power-of-two slot array. Because the counters are not reduced
modulo the capacity, the full `Capacity` slots are usable rather than `Capacity − 1`: the
ambiguity between full and empty that forces a sacrificial slot in a modulo-indexed ring does
not arise when the raw difference `tail − head` is available. At realistic rates a 64-bit
counter cannot wrap within the life of an experiment, so overflow is not handled.

**3.4.3 Cached indices remove the cross-core load from the steady state.** The naive
implementation reads the peer's atomic counter on every operation, which is a load of a line
that the peer core is writing and therefore a coherence miss per element. Each side instead
keeps a private, non-atomic cached copy of the peer's counter and consults the atomic only
when the cached value indicates the ring is full (producer) or empty (consumer). A producer
whose cached head shows free space proceeds without touching the consumer's line at all; it
reloads only when it believes it has filled the ring, and only then discovers whether the
consumer has advanced. In the steady state of an unsaturated ring — which §3.1.3 establishes
is the operating regime — the common path therefore performs no cross-core read.

**3.4.4 False sharing is excluded by explicit alignment.** The two atomic counters and the
two cached copies are each aligned to a cacheline boundary, as is the slot array. Without
this, the producer's store to `tail_` would invalidate the line holding `head_` and defeat
the caching described above, reintroducing exactly the coherence traffic the design
eliminates. The alignment is expressed through a named cacheline constant rather than a
literal so that the assumption is stated once and is visible.

**3.4.5 Memory ordering.** The producer writes the slot and then publishes it with a release
store to `tail_`; the consumer acquires `tail_` before reading the slot. The release/acquire
pair establishes the happens-before edge that makes the slot contents visible, and no
stronger ordering is required because there is exactly one writer of each counter. The
symmetric argument applies to the consumer's release store to `head_` and the producer's
acquire of it when checking for space. Correctness of this reasoning is not asserted on
inspection alone: the stress test is executed under ThreadSanitizer, which is the run that
detects a missing or mis-paired fence, and Chapter 5 records it as the gating correctness
criterion.

**3.4.6 Ordering guarantee delivered.** Within a shard, order is preserved; across shards
no order is defined. This is the correct guarantee rather than a weakening of a stronger one,
because the decision for a channel depends only on that channel's chunks in arrival order,
and no cross-channel ordering has semantic content.

`[TABLE 3.2]` Ring geometry: capacity, element size, per-ring footprint, aggregate at 512
and 2675 channels for the configured shard counts.

---

## 3.5 Backpressure: dropping is the correct policy under a deadline

**3.5.1 Blocking is incorrect, not merely undesirable.** If a ring is full the producer
could block until space appears. Under a deadline this is the wrong choice on two counts.
First, blocking the single reader stalls *every* channel, converting localised congestion on
one shard into system-wide delay — a coupling that the sharded topology otherwise avoids.
Second, and decisively, the queued work whose completion the producer would be waiting for
consists of decisions that are themselves approaching their deadlines; admitting more work
by delaying the reader increases the number of decisions that miss, rather than reducing it.
The system is properly modelled as a loss system with a finite buffer, not as a delay system:
the design question is which chunks to discard, not how long to wait.

**3.5.2 The conservation invariant.** The policy is to attempt the push and, on failure,
immediately release the descriptor's reference and increment a drop counter. The resulting
invariant is

&nbsp;&nbsp;&nbsp;&nbsp;offered = pushed + dropped

which is asserted by the stress test under contention, with the further requirement that
every pushed element is eventually consumed exactly once. This is the property that makes
loss accountable: a run reports how many chunks it declined, so a reported decision rate can
never be inflated by silently discarded work. Chapter 5 reports zero drops at the measured
operating point `[VERIFY]`, which is a statement about headroom and not about the policy
being untested — the policy is exercised deliberately in the stress test at saturation.

**3.5.3 What loss costs.** Discarding a chunk does not discard a read. A read contributes
multiple chunks, and the decision structures accumulate evidence across them, so the loss of
one chunk degrades the evidence available for that read rather than removing it from
consideration. This is the reason loss is tolerable here and would not be in a system whose
decision depended on a single sample.

---

## 3.6 Bounded state: the per-channel footprint

**3.6.1 Inventory.** Per-channel state comprises the scaling estimate and its validity flag,
a chunk counter, the accumulated event base used to express positions in read-relative
coordinates, and the decision structure of Chapter 4. The decision structure dominates: the
bounded vote table occupies approximately 3 KB per channel, giving approximately 1.5 MB
across 512 channels `[VERIFY]`; the anchor-history chainer occupies approximately 14 KB per
channel. Thread-local scratch for quantisation and probing is sized per worker rather than
per channel and is therefore O(W).

`[TABLE 3.3]` Per-channel byte budget by structure, with totals at C = 512 and C = 2675 for
both decision structures.

**3.6.2 Why the alternative is unbounded.** A design that retained a read's anchors until
the read concluded would be O(reads in flight × read length), and read length is set by the
library preparation rather than by the client; long reads of hundreds of kilobases are
routine. Such a design is not merely larger but unbounded in the quantity the client cannot
observe in advance. The structures here instead retain a fixed summary, and Chapter 4
analyses what that summary loses — a Misra-Gries style guarantee for the vote table, and a
bounded look-back for the chainer.

**3.6.3 Where the memory actually goes.** The per-channel budget is a small fraction of
resident memory; the index dominates, at 1.6–4.4 GB depending on configuration `[VERIFY]`.
This is worth stating plainly because it locates the engineering achievement correctly: the
contribution of bounded per-channel state is that it does not *grow*, not that it is the
largest term.

---

## 3.7 Thread topology and processor affinity

**3.7.1 Roles.** The data plane runs one reader (draining the gRPC stream), one action
writer (dispatching ejection requests), and `N` workers, one per shard. Total thread count
is `N + 2`.

**3.7.2 Logical processor numbering is not portable, and assuming otherwise is a defect.**
The initial implementation assigned roles to consecutive logical processor identifiers:
reader to 0, writer to 1, workers from 2. This is incorrect on any machine with simultaneous
multithreading, because the mapping from logical identifiers to physical cores is firmware
dependent and the two common layouts disagree. On the measurement host, sibling threads are
numbered adjacently (`0-1`, `2-3`, …), so reader and writer were placed on two threads of a
single physical core, sharing its execution units and private caches, while a fourth physical
core remained idle. On many bare-metal Intel systems the enumeration instead lists all
physical cores before their siblings, so that logical processors 0 and `n/2` share a core.
No fixed stride is correct on both layouts.

The implementation therefore reads the sibling sets exported by the operating system and
allocates one logical processor per physical core before using any second thread of a core.
Role order is deliberate: workers are allocated first, because a worker executes the entire
decision path and is what the latency figure measures, while the reader and writer are
comparatively cheap. Where the thread count exceeds the physical core count, the system
reports that it has oversubscribed rather than failing silently, and the run is marked
accordingly — a tail latency measured under sibling sharing cannot be attributed to the
software under test.

**3.7.3 Thread count dominates placement.** A four-cell experiment crossing shard count
against pinning establishes that the dominant effect is whether the data-plane thread count
fits the physical core count, not how threads are placed within it `[TABLE 3.4]`. Pinning's
measurable benefit is not a lower median but a reproducible tail: at a fixed thread count the
pinned and unpinned medians are not separable, while the run-to-run spread of the 99th
percentile differs by an order of magnitude `[VERIFY: 8% against 133%]`. The practical
guidance that follows is to size the shard count to the available physical cores first and
treat pinning as a variance-reduction measure.

**3.7.4 Spin-then-yield, and the resource trade it represents.** Workers wait on their rings
by spinning for a bounded number of iterations before yielding. This removes the wakeup
latency of a blocking wait from the critical path at the cost of occupying a core while idle.
The cost is measurable and substantial: aggregate CPU utilisation is 182–202% of one core
against a basecalling-based client's 15.6–25.3% for comparable orchestration `[VERIFY]`. This
is a deliberate trade and is reported as one. On a dedicated sequencing host the trade is
favourable, since the alternative use of those cycles is nil; on a shared host it is not, and
the configuration should be revisited. The word "efficient" is not applicable to this design
and is not used of it.

---

## 3.8 Constraints imposed by the measurement platform

The architecture is described here independently of the platform on which it was measured,
but one platform property shapes which of its properties can be *demonstrated*, and stating
this in the architecture chapter avoids overclaiming in Chapter 5.

All measurements reported in this thesis were taken under a hypervisor (WSL2). Two
consequences follow. The scheduler is not the measured system's own: a vCPU may be descheduled
by the host at any point, including in the middle of a decision, and such an event is
indistinguishable in the data from a slow decision. The clock, although backed by an invariant
TSC on this host, is read inside a virtual machine whose timekeeping is host-mediated.

The effect is strongly percentile-dependent. The median and the 90th percentile are
reproducible across runs and across seeds, because the probability that a given decision is
interrupted is small and the median is insensitive to the tail. The 99th percentile and the
maximum are not: they are dominated by precisely the events the hypervisor introduces. The
thesis therefore quotes p50 and p90 as properties of the implementation and treats p99 and
above as upper bounds contaminated by the platform, and Chapter 5 records the configuration
that would be required to measure them properly — bare metal with isolated cores, full
tickless operation, interrupt affinity moved off the measured cores, and a fixed frequency
governor. The measurement harness refuses to label a run publishable unless those conditions
hold, and records the full environment alongside every result.

---

## 3.9 Summary

The data plane admits signal with a single copy performed by the protobuf runtime, propagates
it as 56-byte cacheline-resident descriptors that borrow arena-owned storage, and manages the
resulting lifetimes with a reference-counting protocol in which the reader's own reference
eliminates a reset-under-dispatch race by construction. Handoff is by single-producer,
single-consumer lock-free rings, one per shard, with producer- and consumer-private cached
indices that remove cross-core loads from the unsaturated steady state and cacheline-aligned
counters that prevent the false sharing which would otherwise defeat them. Sharding by channel
makes all per-channel state single-threaded, so the decision structures of Chapter 4 carry no
concurrency obligations. Backpressure discards rather than blocks, which is the correct policy
for a loss system under a deadline, and accounts for every discarded chunk. Total state is
O(channels) with compile-time per-channel constants and no allocation on the decision path.
Thread placement is derived from the operating system's sibling topology rather than from an
assumed processor numbering, and the system reports when it has been forced to oversubscribe.

Measured per-chunk decision latency of 134–174 µs at the median corresponds to a utilisation
of approximately 0.1 at two workers and 512 channels `[VERIFY]`, with no chunks dropped at the
sustained observed rate. The data plane is consequently not the limiting component of the
system; Chapters 6 and 7 identify and quantify what is.
