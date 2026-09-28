# Cooling Failover

Cooling Failover is repository 31 of the Data Center Control Plane (DCCP). It is the
failover orchestration layer for facility cooling: it decides which alternate cooling
source arrangement is eligible to assume service, governs the transition, tracks
degraded operation, and decides when service may return to the primary arrangement.

**Core question.** When a cooling source or path becomes unavailable or degraded, which
alternate source arrangement is eligible to assume service under current topology,
capacity, redundancy, obligations, policy and authority - and how is the transition
governed, rebalanced, recovered, and fenced against stale plans?

The answer this repository implements is deliberately conservative:

* A structurally redundant path is **not** automatically operationally eligible.
* Eligibility requires **current** evidence for the topology generation, required
  capacity and headroom, source health, service authority and policy.
* A reserve block is never counted twice within an arrangement or a plan.
* Plans are bound to exact source, topology, capacity, policy and evidence generations
  and are refused the moment any bound generation moves.
* A controller acknowledgement is **not** proof that cooling transferred. Verified
  failover requires observed effects from the owning systems.
* A partial transition is never reported as completion.
* Recovery and rebalance require explicit healthy evidence and hysteresis.

## Owned boundary

This repository owns cooling **failover orchestration**:

* failover domain and source-group identities;
* generation-bound candidate sets;
* eligibility evidence references and evidence currency;
* redundancy policy and protected obligations at the orchestration boundary;
* failover plan creation;
* deterministic candidate ranking and selection, only where policy explicitly defines it;
* transition attempts and their state machine;
* degraded-operation state;
* rebalancing and return-to-primary/recovery eligibility;
* stale-plan/epoch/generation fencing;
* durable attempt, command, observation and audit state.

## Explicit non-ownership

Cooling Failover does **not** own, model or reimplement:

* cooling topology itself - it consumes a generation-stamped topology projection;
* cooling-capacity accounting - it consumes generation-stamped block availability;
* device actuation - Airflow Control and Liquid Cooling Control actuate;
* thermal-zone modelling;
* thermal emergency orchestration;
* power failover;
* facility placement;
* low-level BMS sequencing.

Those systems are reached only through typed, generation-bound requests
(`ControlRuntimePort`) and typed observations. The library never opens a device, never
derives topology, and never computes capacity from first principles.

## Architecture

| Header | Contents |
| --- | --- |
| `cooling_failover/ids.hpp` | strong identities, generations, epochs, counters, logical `Tick` |
| `cooling_failover/units.hpp` | exact integer `Watts`, `BasisPoints`, checked arithmetic |
| `cooling_failover/status.hpp` | `ErrorCode`, `ErrorClass`, `Status`, `Result<T>` |
| `cooling_failover/canonical.hpp` | canonical little-endian encoding, SHA-256, CRC-32 |
| `cooling_failover/model.hpp` | imported evidence: topology projection, block availability, health, authority, policy, obligations |
| `cooling_failover/eligibility.hpp` | reserve ledger, eligibility states and causes, candidate sets |
| `cooling_failover/plan.hpp` | plans, steps, command specifications, attempts, observations |
| `cooling_failover/adapters.hpp` | the `ControlRuntimePort` boundary to the control runtimes |
| `cooling_failover/store.hpp` | writer lock, record framing, durable store, path safety |
| `cooling_failover/state.hpp` | durable domain state and the event codec |
| `cooling_failover/orchestrator.hpp` | the public orchestration API |
| `cooling_failover/synthetic.hpp` | SYNTHETIC facility model used by tests, examples, CLI and benchmark |

The orchestrator never mutates a device. Each plan step becomes one typed request per
owning runtime, and each request names the effects that must later be **observed** from
an owning system.

## Architecture: eligibility, reserve accounting and selection

An arrangement is a set of one or more source groups. Every candidate arrangement is
evaluated against the same current evidence, and the outcome is exactly one of
`Eligible`, `Ineligible` or `Indeterminate`. Missing or undecided evidence is never
treated as eligibility, and zero, absent, unavailable, unsupported and indeterminate
availability are distinct states.

The reserve ledger is what makes double counting structurally impossible. Capacity is
expressed as reserve blocks, and a block may serve several source groups. An arrangement
counts the union of the distinct blocks that serve its groups, and the ledger refuses to
allocate a block twice, so an arrangement's usable capacity is never the sum of
overlapping group totals. Allocation order is deterministic: obligations in ascending
obligation identity, blocks in ascending reserve-block identity.

Selection is deterministic where the policy defines it. The policy carries an explicit
selection mode and an ordered list of rank criteria, and ranking ends with a total
tie-break on the candidate key. When the policy defines no ordering and no explicit
target is named, the orchestrator refuses with `SelectionNotDefined` rather than
guessing. Candidate identity is derived from the arrangement, so the same arrangement
always has the same identity.

## Authority and generation model

Distinct concepts are distinct C++ types: an epoch cannot be passed where a topology
generation is expected.

* `ControlPlaneEpoch` - advancing the epoch supersedes every plan bound to the previous
  epoch, and all prior authority.
* `TopologyGeneration` / `CapacityGeneration` / `PolicyGeneration` - owned by other
  systems, imported as evidence.
* `EvidenceSetGeneration` - bumped whenever imported evidence changes.
* `StateRevision` - equals the durable commit sequence; a snapshot compaction changes
  how many events replay, never the revision.
* `CommitSequence` - the durable commit point.
* `EffectSequence` - owned by the observing controllers.
* `IssueSequence` - assigned by this orchestrator per issued command.

A `FailoverPlan` binds: domain, epoch, state revision, topology generation, capacity
generation, policy generation, evidence-set generation, an evidence digest over exactly
the evidence it consumed, the obligations generation, the incumbent, the target
arrangement, the steps and the command specifications. `FailoverPlan::binding_digest`
covers all of them. Before every single command issue the plan is re-checked, and a plan
that no longer matches is fenced durably with the specific reason
(`EpochSuperseded`, `TopologyGenerationChanged`, `CapacityGenerationChanged`,
`PolicyGenerationChanged`, `EvidenceChanged`, `AuthorityLost`).

Every mutation is refused deterministically when its input is stale, future, superseded,
conflicting or cross-generation. Validation precedence is fixed: shape and bounds, then
identity and ordering, then generation currency, then authority, then evidence
completeness, then eligibility. Eligibility causes are reported in evaluation order, so
the primary cause of the same invalid request is always the same.

## Transition semantics

A transition attempt has an explicit state machine:
`Created` -> `RequestsIssued` -> `Acknowledged` -> `PartiallyObserved` -> `Verified`,
with `Failed`, `Fenced`, `Interrupted` and `Refused` as the other terminal or recoverable
dispositions.

* An acknowledgement advances the attempt to `Acknowledged` and never further on its own.
* An effect counts only when it is observed by a system that owns it, carries the
  generations the plan is bound to, is sequenced after the command that requested it, is
  dated at or after that command, and is not recovered-unvalidated durable state.
  Observations whose origin is an actuation acknowledgement are recorded for audit and
  never verify anything.
* A partially observed transition stays `PartiallyObserved`, the domain reports
  `degraded`, and the plan stays active. It is never reported as complete.
* Losing authority fences the plan and every attempt that was in flight for it; no
  further command is issued under fenced authority.

## Persistence and recovery

One store directory per failover domain:

    <root>/<domain-hex>/writer.lock       OS-exclusive writer lock on a canonical path
    <root>/<domain-hex>/store.meta        magic + format version + domain identity
    <root>/<domain-hex>/snapshot.dat      complete state at a commit sequence
    <root>/<domain-hex>/wal.log           append-only records after the snapshot
    <root>/<domain-hex>/rollback.guard    highest commit sequence ever published

* Records are framed with a magic, a bounded payload length, a commit sequence, a
  payload CRC-32 and a header CRC-32.
* Exactly one record is committed per accepted mutation, so a crash resolves to either
  the previous complete generation or the new one, never a hybrid.
* The commit point is the flush plus a verified read-back of the record bytes.
* A torn append tail is detected, reported, and truncated away on the next write open.
  A discarded tail larger than a single record frame is corruption and is refused.
* `rollback.guard` is published atomically after every N commits and on graceful close;
  a store that resolves below it is refused as rolled back.
* `checkpoint()` publishes a state snapshot atomically and then starts a fresh log. A
  crash between the two publications resolves to the snapshot generation.
* Recovery marks every persisted dynamic observation `recovered_unvalidated` and discards
  health streaks: recovered state is not current physical evidence.
* Non-terminal attempts are recovered as `Interrupted` and need re-verification.
* Already-issued commands are never re-issued: the durable command journal and the
  idempotency record are replayed instead. If an issue is repeated for any reason, the
  owning runtime deduplicates on the command identity.
* A public mutation carries a caller request identity. Retrying with the same identity
  replays the original result; reusing it for a semantically different request is refused
  with `IdempotencyConflict`. The identity and fingerprint are durable, so the replay
  survives a restart.

Store paths are validated before use: no traversal components, no control characters, no
embedded NULs, no device prefixes, no alternate data streams, no reserved device names,
bounded component and total length, and no reparse point anywhere on the path. The lock
is taken on the canonical path and the canonical path of the opened handle is compared
with the requested path, so two processes cannot lock the same logical store under
different names.

## Concurrency model

The concurrency model is deliberately small:

* **Single writer per domain, enforced by the operating system.** `writer.lock` is opened
  with no sharing, so a second process - or a second orchestrator in the same process -
  receives `StoreLocked`. Process death releases it.
* **At most one active plan per protected domain**, enforced in durable state:
  `create_plan` returns `PlanAlreadyActive` while another plan is active.
* **The orchestrator object is not thread-safe.** It holds no mutexes and takes no locks
  other than the store's writer lock, which is held for the object's lifetime and is never
  acquired while another lock is held. Callers that share one orchestrator across threads
  must synchronise externally. Because the library holds no internal locks at all, there
  are no read-then-write lock upgrades, no callbacks under locks, no lock inversions, no
  re-entrant persistence callbacks and no shutdown inversions inside it.

## Real vs synthetic proof

* **REAL**: the library, the C++ implementation, the process model, the filesystem, the
  flush and read-back durability behaviour, operating-system writer exclusion, abrupt
  process death and restart, corruption and truncation handling, the CMake package and the
  independent downstream consumer. All of it runs on the host and is reproducible.
* **SYNTHETIC**: `cooling_failover/synthetic.hpp` provides a fabricated cooling plant,
  fabricated sensors, fabricated health conditions and fabricated control runtimes. Every
  test, example, CLI command and benchmark that mentions cooling uses this model. It
  exercises real semantics but is **not** hardware validation.
* **UNSUPPORTED**: no physical chiller, CDU, CRAH/CRAC, pump, valve, thermal store,
  facility BMS, DCIM or PLC was available. Nothing in this repository has been validated
  against real cooling hardware, and no hardware performance claim is made.

## Build

Requirements: CMake 3.20 or newer and a C++20 compiler.

    cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/release --parallel

Options: `COOLING_FAILOVER_BUILD_TESTS`, `COOLING_FAILOVER_BUILD_TOOLS`,
`COOLING_FAILOVER_BUILD_EXAMPLES`, `COOLING_FAILOVER_BUILD_BENCHMARK`,
`COOLING_FAILOVER_BUILD_SHARED`, `COOLING_FAILOVER_ENABLE_ASAN`,
`COOLING_FAILOVER_WARNINGS_AS_ERRORS`.

First-party targets are compiled with `/W4 /WX /permissive-` on MSVC and with
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror` elsewhere.
Warnings are never globally suppressed.

## Test

    cmake --build build/release --parallel
    ctest --test-dir build/release --output-on-failure

or run the suites directly:

    build/release/tests/test_core
    build/release/tests/test_eligibility
    build/release/tests/test_transition
    build/release/tests/test_persistence
    build/release/tests/test_process
    build/release/tests/test_recovery
    build/release/tests/test_determinism
    build/release/tests/test_bounds
    build/release/tests/test_adversarial

`tests/cf_test_child` is a helper executable used by the out-of-process tests. It is never
run directly.

## Install and consume

    cmake --install build/release --prefix build/install

An independent out-of-tree consumer lives in `consumer/`. It is not part of this project's
build; it is configured separately against the installed package:

    cmake -S consumer -B build/consumer -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_PREFIX_PATH=/absolute/path/to/build/install
    cmake --build build/consumer --parallel
    build/consumer/consumer_failover

## Examples, CLI and benchmark

    build/release/examples/cf_example_failover [store-directory]

runs a complete failover: import evidence, evaluate candidate arrangements, inject a
failure, create a plan, issue commands to the owning runtimes, observe effects and verify
completion.

    build/release/tools/cfctl <command> --root <dir> [options]

Commands: `version`, `init`, `status`, `candidates`, `failover`, `recover`, `checkpoint`,
`digest`. Options: `--domain`, `--incumbent`, `--target`, `--degrade`, `--heal`,
`--json`. Exit codes distinguish invalid, stale, denied, indeterminate, conflicting,
unavailable, unsupported and internal failures, so a partial transition is never mistaken
for success.

    build/release/bench/cf_bench_failover [iterations] [store-directory]

measures **completed** durable failovers per second with a flushed and read-back commit on
every mutation.

## Validation performed

Executed on the development host: Windows, MSVC 19.44.35209, CMake 4.3.2, Ninja 1.13.2.

* Release and Debug builds, both with zero warnings under `/W4 /WX /permissive-`.
* The full test suite, in Release and in Debug.
* `test_core` - identities, generations, checked integer arithmetic and overflow, SHA-256
  known-answer vectors, a CRC-32 known-answer vector, canonical encoding round trips,
  truncation at every length, trailing bytes, impossible counts, reserved fields.
* `test_eligibility` - structurally redundant but ineligible paths, missing evidence as
  indeterminate, shared reserve counted once against a brute-force reference, independence
  and headroom requirements, authority denial, reordered evidence, unknown blocks, and zero
  versus absent availability.
* `test_transition` - acknowledgement is not proof, acknowledgement-only origins never
  verify, partial transitions are never completion, stale-generation observations never
  verify and current ones do, repeated execution does not duplicate issued requests,
  plan-request idempotency and conflict, concurrent plans for one domain are excluded, loss
  of authority fences an in-flight plan, a generation change fences a plan, and commands
  are routed to the owning runtime per source kind.
* `test_persistence` - restart round trip, checkpoint, the crash window between the two
  checkpoint publications, torn tails produced by a real child process that dies
  mid-append, payload corruption at several offsets, header corruption, rollback below the
  guard, metadata tampering including a re-checksummed unknown format version, and
  recovered observations not counting as current evidence.
* `test_process` - exclusive writer authority across real processes, authority release on
  abrupt writer death, abrupt death after commits resolving to a complete generation,
  restart without duplicating already-issued requests, and recovered attempts marked
  interrupted until re-verified.
* `test_recovery` - consecutive healthy evidence plus dwell, streak reset on non-healthy
  evidence, the oscillation guard, current-evidence requirements on the primary
  arrangement, and a completed return-to-primary.
* `test_determinism` - semantic determinism of candidate sets, byte-for-byte determinism of
  the canonical state across independent directories, deterministic plan identity and
  shape, and a seeded differential comparison of production eligibility against an
  independent reference model.
* `test_bounds` - store-root path safety, topology and policy limits, capacity arithmetic
  overflow, observation retention bounds, malformed topology rejection, and capacity
  evidence for unknown blocks.
* `test_adversarial` - future-dated evidence, an unreachable control runtime recorded
  rather than hidden, epoch advance fencing every active plan, capacity generation
  regression, mutation after close, repeated open/close cycles, a directory junction as a
  store root, policy absence, manual-only selection, self-targeted failover, unknown plan
  and attempt identities, nil identities, duplicate domain registration, and health
  evidence for an unknown source.
* The runnable example, the CLI smoke path, and the benchmark.

### Sanitizer validation

AddressSanitizer was run on the full suite in a `RelWithDebInfo` configuration with
`-fsanitize=address`, and reported no errors. Note for reproducing it on this host: the
Visual Studio Community toolset installs only the 32-bit ASan runtime, so the x64 ASan
configuration must be configured from a Visual Studio Developer Command Prompt for the
Build Tools toolset, which does ship `clang_rt.asan_dynamic_runtime_thunk-x86_64.lib`. Without that
library the link step fails with `LNK1104` and no sanitizer claim should be made for that
toolset.

## Benchmark methodology

`cf_bench_failover` measures **completed** operations, not submission latency. One operation
is: install fresh health evidence, create a generation-bound plan, issue every command to
the owning synthetic control runtimes, poll and submit the observed effects, verify every
required effect, and commit every mutation. Each commit performs a flush to disk and a
verified read-back, so the reported latency includes the durability cost.

A 20 percent warm-up phase runs first and is not reported. The workload ran on the
development host: Windows, MSVC 19.44 release build, filesystem on a local NTFS volume,
200 measured iterations. The plant and the control runtimes are SYNTHETIC; the process,
filesystem, flush and durability behaviour is REAL. Reported figures are for this host
only and are not a hardware claim.

## Limitations

* No physical cooling hardware was available. Everything cooling-related is synthetic.
* The topology projection is imported as a whole. Incremental topology deltas are not
  modelled; a new projection is a new generation.
* Reserve allocation is a deterministic first fit in ascending reserve-block identity
  order. It is not an optimiser; a different order could cover more obligations from the
  same blocks.
* One protected domain per store. Cross-domain reserve sharing is out of scope because
  capacity accounting is owned elsewhere.
* The orchestrator is single-threaded by design and must be externally synchronised if
  shared across threads.
* Effect observation retention is bounded, so very old audit observations are evicted.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
