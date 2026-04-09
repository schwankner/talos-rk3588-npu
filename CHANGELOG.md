# Changelog

All notable changes to this project are documented in this file.

---

## [Unreleased]

---

## [v1.12.6-rknpu0.9.8] — 2026-04-09

### First functional release — Talos v1.12.6 + rknpu 0.9.8 + NPU inference verified

#### What works

- Talos Linux v1.12.6 on Turing RK1 (RK3588, SODIMM on Turing Pi 2)
- Custom Linux kernel 6.18.18-talos (OE4T patches, GCC toolchain)
- `rockchip-rknpu 0.9.8` — `rknpu.ko` OOT kernel module built against Talos kernel
- `rockchip-rknn-libs 2.3.2` — `librknnrt.so` packaged as Talos system extension
- CDI-based device plugin: `rockchip.com/npu: 1` resource, no `privileged: true`
- `procMount: Unmasked` + `hostUsers: false` — `/proc/device-tree/compatible` readable in pod
- ResNet18 inference over NPU verified end-to-end

#### Key technical fixes

| Fix | Bug | Root Cause |
|-----|-----|-----------|
| `pm_runtime_get_noresume` on genpd devices | Bug 25 | `pm_runtime_resume_and_get()` on NPU genpd virtual devices triggers SCMI power-domain-on SMC into ATF at EL3, causing a hang requiring BMC reset. Fix: keep usage count ≥1 after `dev_pm_domain_attach_by_name()`. |
| `dma_heap/system` via bind-mount | Bug 24 | CDI `deviceNodes` mknod for `/dev/dma_heap/system` fails because it is a char device created by the kernel's `dma_heap` driver, not a static node. Fix: use `DeviceSpec.mounts` bind-mount instead. |
| `MODULE_DEVICE_TABLE` for autoload | Bug 16 | Without an `of:` alias entry in `modules.alias`, udevd never triggers `modprobe rknpu`, leaving the NPU PM domain's `sync_state()` pending — which blocks Ethernet initialization. |
| CDI not enabled in containerd | Bug 23 | Talos default containerd config does not enable CDI. Requires `enable_cdi_devices = true` + `cdi_spec_dirs` in `/etc/cri/conf.d/20-customization.part`. |
| YAML block scalar indentation | — | Multiline Python in `pkg.yaml` block scalars must be indented; Python at column 0 terminates the block. Fix: base64-encode complex Python patches. |

#### Component versions

| Component | Version |
|-----------|---------|
| Talos Linux | v1.12.6 |
| Kubernetes | v1.35.0 |
| Linux kernel | 6.18.18-talos |
| siderolabs/pkgs | a92bed5 |
| rknpu driver | 0.9.8 |
| librknnrt.so | 2.3.2 |
