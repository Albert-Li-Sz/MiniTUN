# Performance & Soak Validation

The three-run performance, 24-hour stress and 7-day soak are optional engineering
validations, not release prerequisites for RC or GA. To get reproducible, comparable,
complete evidence, the host should be a dedicated Linux machine with no other workload:
exactly 4 vCPUs, about 8 GiB memory, and `RLIMIT_NOFILE >= 65536`. The GitHub Actions
`Performance and soak validation` workflow only schedules self-hosted runners with the
`minitun-benchmark-4cpu-8gib` label. That runner should be dedicated to this workflow; no
other jobs may be scheduled during a soak.

All complete JSON uses `evidence_format: 1` and records the full 40-character source
commit, host specs, wall-clock and monotonic durations, scale, throughput, latency, RSS,
convergence time and failure list. The workflow generates a GitHub OIDC attestation over
the core JSON for independent verification. `release.yml` does not download or verify these
artifacts.

## Three independent benchmarks

The local equivalent command is below; it is suitable for debugging, and should be run by
the dedicated runner when comparable evidence is needed:

```bash
cmake -S . -B build/performance -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMINITUN_USE_SYSTEM_DEPS=ON \
  -DMINITUN_BUILD_TESTS=OFF \
  -DMINITUN_ENABLE_LTO=ON
cmake --build build/performance --parallel 4

MINITUN_SOURCE_SHA=$(git rev-parse HEAD) \
RESULT_DIR=benchmark-results RUNS=3 \
  benchmarks/run_gate.sh \
    build/performance/minitun \
    build/performance/minitund \
    build/performance/minitun-server
```

Formal execution:

```bash
gh workflow run performance.yml --ref main -f operation=benchmark
```

Validation can run against any commit worth evaluating; it does not wait for an RC and does
not block RC or GA.

Each run recreates 100 daemons, 2,000 tunnels and 10,000 concurrent relays, and measures a
same-host direct TLS/TCP echo baseline. Each connection sends deterministic data and
verifies the echo block by block. Validation thresholds:

- median payload throughput across the three runs ≥1 Gbit/s, and ≥85% of the direct TLS
  baseline;
- zero corruption and zero non-quota rejections across the 10,000 relays each run, with
  p95 first byte ≤250 ms;
- the aggregate peak RSS of the MiniTun server and 100 daemons ≤4 GiB;
- 2,000 tunnels recover within 30 seconds after a server restart.

Results are written to `run-1.json`…`run-3.json`, per-run sanitized logs and
`gate-summary.json`. When thresholds are not met, the results can serve as input for
optimization or for later evaluating Protocol v2 `multiplexed_streams`, but they do not
block GA; v1 continues to use one relay per one TLS Worker.

## 24-hour and 7-day continuous soak

A single GitHub Actions step, token and job lifetime is shorter than a full 7 days. To keep
the same batch of processes and continuous RSS measurement, the `start-*` operations copy
the built binaries and scripts to `/var/lib/minitun-release-gates`, then start a systemd
service holding a host-level mutex. The workflow ending does not stop that service; after it
completes, results are collected using the numeric run ID of the start workflow. Collection
re-verifies the binary and script SHA-256 and rejects sessions that are running, failed,
shortened or substituted.

The workflow can run against any stable ref. The example below uses `v1.0.0-rc.2` to bind
the two long phases to a fixed commit; this is not a GA release requirement.

First start the full-scale 24-hour stress:

```bash
gh workflow run performance.yml --ref v1.0.0-rc.2 \
  -f operation=start-full-24h
```

Note the numeric run ID of this start workflow. Collect at least 24 hours later; collecting
early fails:

```bash
gh workflow run performance.yml --ref v1.0.0-rc.2 \
  -f operation=collect-full-24h \
  -f session_id=<START_WORKFLOW_RUN_ID>
```

Only after the 24-hour artifact passes, start the following 7-day mixed load:

```bash
gh workflow run performance.yml --ref v1.0.0-rc.2 \
  -f operation=start-mixed-7d
# seven full calendar days later:
gh workflow run performance.yml --ref v1.0.0-rc.2 \
  -f operation=collect-mixed-7d \
  -f session_id=<START_WORKFLOW_RUN_ID>
```

The mixed phase performs server restarts, 2-second network-outage windows, policy SIGHUP
and per-client PSK rotation between sustained 10,000-connection batches. Any load failure,
process crash, data corruption or convergence timeout fails the session; the steady-phase
aggregate RSS growth must not exceed 5%. Artifacts retain summaries, event records,
server/daemon logs, kernel, CPU and memory info, and never include PSKs, private keys,
databases or user payloads.

An administrator can check status on the dedicated runner:

```bash
sudo systemctl list-units 'minitun-soak-*'
benchmarks/soak_service.sh status \
  mixed-7d <40_HEX_COMMIT> <START_WORKFLOW_RUN_ID>
```

`SOAK_SECONDS_OVERRIDE` is only for local script development; the full-duration verifier
fixedly requires the actual wall clock and monotonic durations to reach 86,400 seconds and
604,800 seconds respectively, so an overridden value cannot produce complete 24-hour or
7-day evidence.

## Relationship to GA release

`release.yml` explicitly records the three results as `not-required` and does not download
the three-run performance, 24-hour stress or 7-day soak artifacts; missing evidence or
unmet thresholds never block release. GA still keeps the following release conditions:

1. the RC tags are consecutive and all annotated, and GA points to the same commit as the
   last rc.2 or higher RC;
2. no open GitHub issues with a P0/P1 priority label;
3. the required build, test, packaging and blocking security checks pass; OCI High/Critical
   vulnerabilities are fully reported but do not block release;
4. the SBOM, checksums, keyless signatures and provenance/attestation of the release
   artifacts are successfully generated and verified.

Any protocol, schema or SDK ABI change after the final RC produces a new commit, so GA's
same-commit check rejects the release; an additional rc.N must be published, but the
optional performance or soak validation does not need to be re-run as a release gate.
