# CI and release automation

MiniTun keeps build, security, package, and publication concerns in separate GitHub
Actions workflows. Every workflow uses minimal token permissions and cancels obsolete
runs for the same ref.

| Workflow | Triggers | Result |
| --- | --- | --- |
| `ci.yml` | pushes to `main`/`develop`, pull requests, manual dispatch | GCC Debug, GCC Release, and Clang Debug builds with all CTest and CLI smoke tests |
| `sanitizers.yml` | pushes to `main`, pull requests, manual dispatch | Clang ASan+UBSan, separate TSan, and bounded libFuzzer smoke jobs |
| `package.yml` | package-related pushes to `main`, manual dispatch, reusable workflow calls | tested amd64 DEB and x86_64 RPM artifacts plus SHA-256 manifests |
| `release.yml` | validated `v*.*.*` tags | tested packages, consolidated `SHA256SUMS`, and a GitHub Release |

The Actions themselves are pinned to stable major versions. Dependabot checks the
`github-actions` ecosystem weekly and groups updates.

All compiler-matrix jobs use distribution packages, including Ubuntu 22.04's supported
Asio 1.18 baseline.

## Package artifacts

The DEB job builds on Ubuntu and installs the result in a clean Ubuntu 22.04 amd64
container. The RPM job builds inside Fedora and installs the result in a second clean
Fedora x86_64 container. Both jobs run the full test suite before CPack and inspect
package metadata, file lists, installation, repeated installation, and removal.

Uploaded package files use release-oriented names:

```text
minitun-client-0.1.0-linux-amd64.deb
minitun-server-0.1.0-linux-amd64.deb
minitun-client-0.1.0-linux-x86_64.rpm
minitun-server-0.1.0-linux-x86_64.rpm
SHA256SUMS
```

The first release architecture is x86_64. The package jobs are isolated by architecture
so a native ARM64 runner or a later cross-compilation job can be added without changing
the x86_64 publication contract.

## Create a release

Set the CMake project version to the intended stable base version and ensure every local
check passes. Create and push either a stable tag or a release-candidate tag:

```bash
git tag -a v0.1.0 -m "MiniTun v0.1.0"
git push origin v0.1.0

git tag -a v0.2.0-rc.1 -m "MiniTun v0.2.0-rc.1"
git push origin v0.2.0-rc.1
```

Accepted tags are `vMAJOR.MINOR.PATCH` and `vMAJOR.MINOR.PATCH-rc.NUMBER`. The base
version must match `project(VERSION ...)` in `CMakeLists.txt`. A stable tag creates a
normal latest release; an RC tag creates a prerelease. Publication only runs after both
package jobs and their clean-container smoke tests succeed.

The release job downloads the two package artifacts, verifies all four expected files,
regenerates a consolidated `SHA256SUMS`, checks it, generates release notes, and uploads
the packages and manifest with the repository-scoped GitHub token.
