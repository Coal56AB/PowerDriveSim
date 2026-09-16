# Reference CPU: prepared equation systems

The generic solver lazily caches the matrix for each exact step length, initialization mode, switch state and diode active set. The cache holds at most 32 small systems (up to 64 unknowns), or two larger sparse systems. Step lengths are never rounded and scheduled event times remain exact.

Each entry holds constant RHS terms, dynamic C/L terms and a reusable factorization. Elimination skips structural zero terms in the original arithmetic order. RHS and solution buffers are reused. Every solve still checks finite values, the linear residual and diode inequalities. When the current diode active set fails, the original bounded search order is used; its temporary containers are allocated only on failure. There is no topology-specific circuit shortcut or physical regularization.

The GUI detaches views and releases large previous results in a background task. This avoids blocking Run while freeing millions of old samples. Results retain all recorded samples; no decimation is applied to recording or CSV.

## Reproducing measurements

`powerdrive-benchmark project.pds 1 none` measures computation without recording.

`powerdrive-benchmark project.pds 1 stream` records the union of project scope/plot channels and consumes each streamed batch. It measures generation of every recorded sample but does not retain the complete history or paint a GUI. `first_sample_seconds` includes solver initialization but excludes `compile_seconds`. The payload estimate describes only the retained final batch in this mode. Default benchmark mode retains all channels and samples.

The recording plan includes plots inside hierarchy instances. `peak_resident_bytes` is the OS process high-water mark on Windows/Linux; `result_payload_estimate_bytes` counts retained sample/value/gate container capacities and excludes allocator overhead. With no recorded channels, `first_sample_seconds=0` means no sample was produced, not zero initialization cost.

Desktop measurement: set `PDS_SIM_BENCHMARK_PROJECT` to a project and run `powerdrive-interaction-tests simulation_performance -platform windows`. This retains the complete history, shows Scope and a plot, and measures two runs including dispatch, worker preparation/execution and rendering. The optional performance test checks dispatch below 100 ms on the measurement machine.
