# Contributing

Contributions are welcome — especially:

- Support for additional RK3588 boards (FriendlyElec CM3588, Radxa Rock 5B, Orange Pi 5 Plus)
- Updated component versions (newer Talos, rknpu driver, librknnrt)
- Bug reports via GitHub Issues
- Fixes for known limitations (see [BUGS.md](BUGS.md))

## Guidelines

- Follow the [Conventional Commits](https://www.conventionalcommits.org/) spec
- Test changes with at least one full boot cycle on real hardware (UART/dmesg log encouraged)
- For kernel module builds: verify the module loads and `init_runtime()` succeeds before submitting
- License: all contributions are subject to the [MIT License](LICENSE)

## Development Setup

See [README — Quick Start](README.md#3-quick-start) for full prerequisites.
Minimum: `talosctl`, `kubectl`, `gh` CLI, and a Turing Pi 2 with RK1 (or compatible RK3588 board).

Builds run in GitHub Actions — see [README — Building](README.md#6-building) for how to trigger them.

## Reporting Bugs

Open a [GitHub Issue](https://github.com/schwankner/rockchip-rk3588-npu-k8s/issues) with:

- Talos version, kernel version, rknpu/rknn-libs version
- Full `talosctl dmesg` output (or relevant excerpt)
- Steps to reproduce
- Board model
