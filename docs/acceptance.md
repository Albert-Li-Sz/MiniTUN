# Final acceptance

MiniTun 0.1.0 completed stage-16 final acceptance on 2026-08-02. The accepted
implementation baseline is commit
[`14593f0d004e6d59513830570189e1c45b46e344`](https://github.com/LMTINSUZHOU/MiniTUN/commit/14593f0d004e6d59513830570189e1c45b46e344).
The acceptance-closing change updates documentation only and does not alter the tested
programs or packages.

## Automated evidence

| Requirement | Evidence | Result |
| --- | --- | --- |
| Complete test suite | [CI run 30691311281](https://github.com/LMTINSUZHOU/MiniTUN/actions/runs/30691311281): Ubuntu 22.04 GCC Debug, Ubuntu 24.04 GCC Release, and Ubuntu 24.04 Clang Debug each passed 206/206 CTest cases and CLI smoke tests | Pass |
| ASan and UBSan | [Sanitizers run 30691311287](https://github.com/LMTINSUZHOU/MiniTUN/actions/runs/30691311287): Clang ASan+UBSan passed 206/206 with leak detection and halt-on-error enabled | Pass |
| TSan | [Sanitizers run 30691311287](https://github.com/LMTINSUZHOU/MiniTUN/actions/runs/30691311287): native Clang TSan passed 206/206 with halt-on-error enabled | Pass |
| Fuzz smoke | [Sanitizers run 30691311287](https://github.com/LMTINSUZHOU/MiniTUN/actions/runs/30691311287): all five libFuzzer targets completed 2,000 runs | Pass |
| DEB | [Packages run 30691311270](https://github.com/LMTINSUZHOU/MiniTUN/actions/runs/30691311270): 206/206 tests, metadata/content verification, and clean Ubuntu installation smoke test | Pass |
| RPM | [Packages run 30691311270](https://github.com/LMTINSUZHOU/MiniTUN/actions/runs/30691311270): 206/206 tests, metadata/content verification, and clean Fedora installation smoke test | Pass |
| Multi-server E2E | `integration.multi-server-sessions` passed in every compiler, sanitizer, and package test job; it covers concurrent servers, failure isolation, server restart, daemon restart, and stable client identity | Pass |

The same suites cover tunnel registration, Worker Pool assignment, raw TCP relay,
half-close behavior, backpressure, connection quotas, graceful shutdown, install
layout, reconnects, and recovery. The Linux-focused acceptance rerun also passed all
six high-risk integration tests: multi-server sessions, tunnel registration, Worker
Pool, TCP relay, stability, and installation layout.

## Package artifact verification

The uploaded artifacts were downloaded rather than taken from a local build. Their
manifests verified successfully, package metadata reports version 0.1.0 and the expected
x86_64 architecture, and clean Ubuntu and Fedora containers passed installation,
reinstallation/upgrade, removal, and the documented state-retention or purge policy.

```text
84d1d6bcb4bca47a422120773d7435674fef07ca302c79c32ac4da3c8f7d7fa8  minitun-client-0.1.0-linux-amd64.deb
eccf04546fd846b933b5308eb6951214ac983fce1eea7d4b26cc2836323460b4  minitun-server-0.1.0-linux-amd64.deb
eb4f8fddd94c99e96d2576f5e04e99cc53e04cf612c66afb7de4011154afec10  minitun-client-0.1.0-linux-x86_64.rpm
f71c6d808d8174aadcf21c64a71fc8eaf3eb2262aa0b8e83ae71af631414af9f  minitun-server-0.1.0-linux-x86_64.rpm
```

## Documentation and release readiness

The three CLI help surfaces, installed paths, systemd units, sysusers definitions,
manual pages, security boundaries, package lifecycle, CI workflows, and release
instructions were cross-checked against the implementation. GitHub Actions workflow
syntax and embedded shell passed actionlint 1.7.12 and ShellCheck 0.11.0. Repository
shell scripts, local Markdown links, release-tag validation, whitespace, and the
installed DEB systemd units also pass their dedicated checks.

The release workflow accepts only `vMAJOR.MINOR.PATCH` and
`vMAJOR.MINOR.PATCH-rc.NUMBER`, requires the base version to match `CMakeLists.txt`,
reuses both tested package jobs, verifies the four expected assets and consolidated
SHA-256 manifest, and publishes only after those jobs succeed.

Final acceptance does not create a public tag or GitHub Release. After choosing the
release point, an operator can publish the already validated 0.1.0 path with:

```bash
git tag -a v0.1.0 -m "MiniTun v0.1.0"
git push origin v0.1.0
```

## Decision

**Pass.** Every stage-16 requirement and the project completion criteria are satisfied.
MiniTun 0.1.0 is ready for an operator-approved version tag and release.
