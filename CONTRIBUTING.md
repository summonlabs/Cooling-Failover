# Contributing to Cooling Failover

Cooling Failover is licensed under the Apache License, Version 2.0. By submitting
a contribution you agree that your contribution is licensed under the same terms,
as described in section 5 of that license. There is no Contributor License
Agreement to sign and no copyright assignment is required: you keep the copyright
to your contribution while granting the project the license terms above.

## Ground rules

* Keep the systems boundary intact. This repository owns cooling failover
  orchestration and nothing else. Cooling topology, cooling-capacity accounting,
  device actuation, thermal-zone modelling, thermal emergency orchestration,
  power failover, facility placement and low-level BMS sequencing belong to other
  systems and must arrive here as typed, generation-stamped evidence or as typed
  requests - never as reimplemented logic.
* Never turn missing, stale, unknown, unsupported, denied or indeterminate state
  into zero, permission, availability or success.
* Never report a partial transition as complete. Verified failover requires
  current observed evidence from the system that owns the effect.
* Any authority-bearing decision must be bound to the generations it was planned
  against and must refuse stale, future, superseded, conflicting or
  cross-generation input deterministically.
* Use exact integer units for physical quantities. Floating point must not be an
  authority or accounting boundary.

## Code quality

* C++20. The core is portable where practical; Windows/MSVC is the primary
  supported toolchain.
* All first-party code must compile with zero warnings. On MSVC that means
  `/W4 /WX /permissive-`; elsewhere
  `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`.
  Never suppress a warning globally to make a build green.
* Public results carry an explicit `ErrorCode`; exception text is not the machine
  contract.
* Bound every input-controlled resource: payload sizes, object counts, graph
  edges, queues, history, retry state, temporary files and cache sizes. Never
  allocate from an untrusted declared size without a checked bound.
* Keep deterministic ordering and tie-breaking explicit, and document the
  validation precedence so that the same invalid input always produces the same
  primary error.

## Build and test

    cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/release --parallel
    ctest --test-dir build/release --output-on-failure

Also validate the Debug configuration, and the AddressSanitizer configuration
where the toolchain provides the sanitizer runtime:

    cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build/debug --parallel
    ctest --test-dir build/debug --output-on-failure

    cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
          -DCOOLING_FAILOVER_ENABLE_ASAN=ON
    cmake --build build/asan --parallel
    ctest --test-dir build/asan --output-on-failure

Tests are proof obligations, not decoration. Prefer a small number of tests that
prove something over a large number that prove nothing. Where a decision is
derived, cross-check it against an independent reference model rather than
against the implementation itself.

If you change durable state, also exercise real process death at meaningful
persistence stages and prove that recovery resolves to either the previous
complete generation or the new complete generation - never a hybrid.

## Documentation

Update `README.md` when behaviour changes. Describe only what the repository
actually does. Keep the real/synthetic labelling accurate: a synthetic facility
is not hardware validation. Do not add speculative roadmap material.

## Commits

Keep commits focused and messages concise, public-facing and neutral. Do not add
`Co-authored-by` trailers or other attribution trailers.
