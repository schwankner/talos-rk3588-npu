# talos-rockchip-rk3588-npu

Talos Linux system extension and Kubernetes device plugin for the Rockchip RK3588 NPU.

Runs RKNN inference in Kubernetes pods **without `privileged: true`** by combining Talos Linux's mainline kernel 6.18+ with the Container Device Interface (CDI).

Inspired by [talos-jetson-orin-nx](https://github.com/schwankner/talos-jetson-orin-nx) and [talos-sbc-rk3588](https://github.com/milas/talos-sbc-rk3588).

---

## Table of Contents

1. [Supported Hardware](#1-supported-hardware)
2. [How It Works](#2-how-it-works)
3. [Quick Start](#3-quick-start)
4. [Repository Structure](#4-repository-structure)
5. [Component Versions](#5-component-versions)
6. [Building](#6-building)
7. [Installation](#7-installation)
8. [Running NPU Pods](#8-running-npu-pods)
9. [Device Plugin Details](#9-device-plugin-details)
10. [Known Issues](#10-known-issues)
11. [Contributing](#11-contributing)
12. [License](#12-license)

---

## 1. Supported Hardware

| Board | SoC | Status |
|-------|-----|--------|
| [Turing RK1](https://turingpi.com/product/turing-rk1/) (on Turing Pi 2) | RK3588 | ✅ Tested |
| [FriendlyElec CM3588](https://www.friendlyelec.com/index.php?route=product/product&product_id=299) | RK3588 | 🔧 Planned |
| Radxa Rock 5B | RK3588 | 🔧 Planned |
| Orange Pi 5 Plus | RK3588S | 🔧 Planned |

The NPU system extension and device plugin are **board-agnostic** — only the Talos installer image (U-Boot + DTB) is board-specific.

---

## 2. How It Works

### The Non-Privileged Problem — Solved

Running RKNN inference in Kubernetes normally requires `privileged: true` for two reasons:

1. The RKNN runtime reads `/proc/device-tree/compatible` to detect the SoC. Kubernetes masks `/proc` in all containers by default.
2. The NPU device nodes (`/dev/dri/renderD*`, `/dev/dma_heap/system`) need to be injected into the container.

On the stock Rockchip BSP kernel (6.1), there is no non-privileged workaround. On **Talos Linux with mainline kernel 6.18+**, both problems are solved:

```
Mainline kernel 6.18+
  → MOUNT_ATTR_IDMAP for tmpfs (requires 6.3+)
  → spec.hostUsers: false  +  securityContext.procMount: Unmasked
  → /proc/device-tree/compatible readable in container ✅
  → CDI injects /dev/dri/renderD*, /dev/dma_heap/system, librknnrt.so
  → privileged: false ✅
```

### Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Talos Linux Node (RK3588, mainline kernel 6.18)        │
│                                                         │
│  System Extensions (installed via machine config):      │
│  ├── rockchip-rknpu      rknpu.ko (OOT kernel module)  │
│  └── rockchip-rknn-libs  librknnrt.so (Rockchip SDK)   │
│                                                         │
│  DaemonSet:                                             │
│  └── rk3588-npu-device-plugin                          │
│      ├── advertises: rockchip.com/npu: 1               │
│      └── writes: /var/run/cdi/rockchip-npu.yaml        │
└─────────────────────────────────────────────────────────┘
                          │
                          ▼  CDI injection at pod start (no privileged)
┌─────────────────────────────────────────────────────────┐
│  Pod                                                    │
│                                                         │
│  spec:                                                  │
│    hostUsers: false                                     │
│    securityContext:                                     │
│      procMount: Unmasked                                │
│    containers:                                          │
│      resources:                                         │
│        limits:                                          │
│          rockchip.com/npu: "1"                          │
│                                                         │
│  Injected by CDI:                                       │
│  ├── /dev/dri/renderD129  (NPU DRM render node)        │
│  ├── /dev/dri/card1       (NPU DRM master node)        │
│  ├── /dev/dma_heap/system (DMA buffer allocator)       │
│  └── /usr/lib/librknnrt.so (bind-mount from host)      │
└─────────────────────────────────────────────────────────┘
```

---

## 3. Quick Start

```bash
# 1. Flash node4 (Turing RK1) with the NPU installer
talosctl upgrade \
  --nodes 10.0.10.42 \
  --talosconfig ./npu-test-talosconfig \
  --image ghcr.io/schwankner/talos-rk3588-npu-installer-base:installer-v1.12.6 \
  --preserve

# 2. Apply machine config (adds NPU extensions + CDI containerd config)
talosctl apply-config \
  --nodes 10.0.10.42 \
  --talosconfig ./npu-test-talosconfig \
  -f worker.yaml

# 3. Deploy device plugin
kubectl --kubeconfig npu-test-kubeconfig \
  apply -f deploy/device-plugin.yaml

# 4. Verify NPU is advertised
kubectl --kubeconfig npu-test-kubeconfig \
  get nodes -o json | jq '.items[].status.allocatable | with_entries(select(.key | startswith("rockchip")))'
# → { "rockchip.com/npu": "1" }
```

---

## 4. Repository Structure

```
talas-rockchip-rk3588-npu/
│
├── rockchip-rknpu/              # Talos system extension: rknpu.ko kernel module
│   ├── pkg.yaml                 # bldr build spec
│   ├── manifest.yaml            # Talos extension metadata
│   └── files/                  # Kbuild patch, udev rules
│
├── rockchip-rknn-libs/          # Talos system extension: librknnrt.so
│   ├── pkg.yaml
│   └── manifest.yaml
│
├── plugins/
│   └── rk3588-npu-device-plugin/  # Kubernetes CDI device plugin (Go)
│       ├── main.go
│       ├── Dockerfile
│       └── go.mod
│
├── boards/                      # Board-specific: U-Boot + DTB overlays only
│   └── turing-rk1/
│
├── deploy/                      # Kubernetes manifests
│   └── device-plugin.yaml
│
├── kernel/                      # Kernel config fragments
│   └── config-arm64-rk3588-npu.fragment
│
├── scripts/
│   ├── common.sh                # All pinned version variables
│   ├── build-extensions.sh      # Builds rockchip-rknpu + rockchip-rknn-libs
│   ├── build-installer.sh       # Builds custom Talos installer image
│   ├── build-uki.sh             # Assembles Talos UKI with extensions
│   └── build-usb-image.sh       # Creates flashable .raw image per board
│
├── .github/workflows/
│   ├── ci.yaml                  # Shellcheck, go vet, YAML validation
│   ├── build-extensions.yaml    # Builds OCI extension images
│   ├── build-device-plugin.yaml # Builds device plugin container
│   ├── build-installer.yaml     # Builds custom Talos installer
│   ├── auto-tag.yaml            # Tags on version-file changes
│   ├── release.yaml             # Full release pipeline
│   └── check-talos.yaml         # Daily check for new Talos releases
│
├── BUGS.md                      # Documented hard problems and their solutions
└── CHANGELOG.md
```

---

## 5. Component Versions

All versions are pinned in `scripts/common.sh`.

| Component | Version | Notes |
|-----------|---------|-------|
| Talos Linux | v1.12.6 | |
| Linux kernel | 6.18.18-talos | Mainline, OE4T patches |
| siderolabs/pkgs | a92bed5 | Pinned to Talos v1.12.6 |
| rknpu driver | 0.9.8 | [w568w/rknpu-module](https://github.com/w568w/rknpu-module), mainline-compatible |
| librknnrt.so | 2.3.2 | [airockchip/rknn-toolkit2](https://github.com/airockchip/rknn-toolkit2) |

### NPU Performance (Turing RK1)

| Metric | Value |
|--------|-------|
| NPU cores | 3× 2 TOPS = 6 TOPS total |
| ResNet18 inference | ~3 ms / inference (NPU_CORE_AUTO) |
| ResNet18 throughput | ~330 fps |

---

## 6. Building

All production builds run in **GitHub Actions** — never locally. The `ubuntu-24.04-arm` runner provides a native ARM64 build environment; no QEMU or cross-compilation is needed.

### Trigger a build manually

```bash
# Build system extensions (rknpu.ko + librknnrt.so)
gh workflow run "Build Extensions" \
  --repo schwankner/rockchip-rk3588-npu-k8s --ref main

# Build custom Talos installer (needed when Talos version changes)
gh workflow run "Build Installer" \
  --repo schwankner/rockchip-rk3588-npu-k8s --ref main
```

### Build on version change

Push to `main` with a change to `scripts/common.sh` or `rockchip-rknpu/pkg.yaml`:
- `ci.yaml` runs shellcheck + go vet + YAML validation
- `auto-tag.yaml` creates a git tag `v<talos>-rknpu<rknpu>` if versions changed
- The tag triggers `release.yaml` → builds extensions + device plugin + installer → creates a GitHub Release

### Produced images

| Image | Tag format |
|-------|-----------|
| `ghcr.io/schwankner/rockchip-rknpu` | `0.9.8-6.18.18-talos` |
| `ghcr.io/schwankner/rockchip-rknn-libs` | `2.3.2-6.18.18-talos` |
| `ghcr.io/schwankner/rk3588-npu-device-plugin` | `v<tag>` |
| `ghcr.io/schwankner/talos-rk3588-npu-installer-base` | `installer-v1.12.6` |

---

## 7. Installation

### Prerequisites

- Talos Linux v1.12.6 installed on your RK3588 board (via [factory.talos.dev](https://factory.talos.dev) with `sbc-rockchip` overlay)
- `talosctl` v1.12.6
- `kubectl`
- GHCR access (images are public)

### Step 1 — Upgrade to the NPU installer

The custom installer includes a kernel with the same module-signing key used to build `rknpu.ko`. This key-match is required by Talos's `module.sig_enforce=1`.

```bash
talosctl upgrade \
  --nodes <NODE_IP> \
  --talosconfig ./your-talosconfig \
  --image ghcr.io/schwankner/talos-rk3588-npu-installer-base:installer-v1.12.6 \
  --preserve
```

### Step 2 — Add system extensions to machine config

Add to your Talos machine configuration under `machine.install.extensions`:

```yaml
machine:
  install:
    extensions:
      - image: ghcr.io/schwankner/rockchip-rknpu:0.9.8-6.18.18-talos
      - image: ghcr.io/schwankner/rockchip-rknn-libs:2.3.2-6.18.18-talos
```

Also enable CDI in containerd (required for device injection):

```yaml
machine:
  files:
    - path: /etc/cri/conf.d/20-customization.part
      op: create
      permissions: 0o644
      content: |
        [plugins."io.containerd.cri.v1.runtime"]
          enable_cdi_devices = true
          cdi_spec_dirs = ["/var/run/cdi"]
```

Apply and reboot:

```bash
talosctl apply-config \
  --nodes <NODE_IP> \
  --talosconfig ./your-talosconfig \
  -f machine.yaml
```

### Step 3 — Enable user namespaces

Required for `procMount: Unmasked`:

```yaml
machine:
  sysctls:
    user.max_user_namespaces: "15000"
```

### Step 4 — Deploy the device plugin

```bash
kubectl apply -f deploy/device-plugin.yaml
```

### Step 5 — Verify

```bash
# Check extensions loaded
talosctl get extensions --nodes <NODE_IP> --talosconfig ./your-talosconfig
# Should show: rockchip-rknpu, rockchip-rknn-libs

# Check rknpu.ko loaded
talosctl dmesg --nodes <NODE_IP> --talosconfig ./your-talosconfig | grep rknpu

# Check NPU resource advertised
kubectl get node -o json | jq '.items[].status.allocatable | with_entries(select(.key | startswith("rockchip")))'
# → { "rockchip.com/npu": "1" }
```

---

## 8. Running NPU Pods

### Minimal pod spec

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: rknn-inference
spec:
  hostUsers: false
  securityContext:
    procMount: Unmasked
  containers:
    - name: inference
      image: your-rknn-app:latest
      securityContext:
        runAsNonRoot: false
      resources:
        limits:
          rockchip.com/npu: "1"
```

### What CDI injects

When `rockchip.com/npu: 1` is in `resources.limits`, the device plugin's CDI spec causes containerd to inject:

| Path in container | Source on host |
|-------------------|----------------|
| `/dev/dri/renderD129` | NPU DRM render node |
| `/dev/dri/card1` | NPU DRM master node |
| `/dev/dma_heap/system` | DMA buffer heap |
| `/usr/lib/librknnrt.so` | RKNN runtime library (bind-mount) |

### Why `hostUsers: false` + `procMount: Unmasked`

The RKNN runtime calls `open("/proc/device-tree/compatible", O_RDONLY)` during `init_runtime()` to identify the SoC. Without `procMount: Unmasked`, this path is masked by a read-only empty tmpfs and `init_runtime()` fails. The unmasked `/proc` requires user namespaces (`hostUsers: false`), which in turn requires mainline kernel 6.3+ for `MOUNT_ATTR_IDMAP` on tmpfs.

### BSP kernel note

This approach does **not** work on BSP kernels (e.g., Rockchip 6.1). It requires mainline 6.18+. The Talos sbc-rockchip overlay uses the mainline kernel.

---

## 9. Device Plugin Details

### Resource name

`rockchip.com/npu` — advertises exactly 1 unit per node (the RK3588 NPU is a single shared resource).

### Device discovery

The plugin discovers the NPU DRM render node at runtime via sysfs:

```
/sys/bus/platform/drivers/RKNPU/fdab0000.rknpu/drm/renderD*/
```

This avoids hardcoding `/dev/dri/renderD128` vs `renderD129`, which varies by probe order.

### udev rule

`rknpu.ko` ships a udev rule (`90-rknpu.rules`) that sets `/dev/rknpu` to `0666` and `/dev/dma_heap/system` to `0666`, allowing unprivileged container access via CDI.

### Module autoload

`rknpu.ko` includes a `MODULE_DEVICE_TABLE(of, rknpu_of_match)` entry for `rockchip,rk3588-rknpu`, so udev autoloads it when the NPU platform device is discovered. Without this, the NPU PM domain's `sync_state()` callback would stay pending indefinitely, blocking Ethernet initialization (see [BUGS.md](BUGS.md) — Bug 16).

---

## 10. Known Issues

See [BUGS.md](BUGS.md) for documented issues and solutions. Key entries:

| Bug | Summary |
|-----|---------|
| Bug 16 | Missing `MODULE_DEVICE_TABLE` blocks Ethernet |
| Bug 24 | `dma_heap/system` must be bind-mounted, not CDI mknod |
| Bug 25 | `pm_runtime_resume_and_get` on genpd devices crashes ATF at EL3 on RK3588 |

---

## 11. Contributing

Contributions welcome — especially:

- Support for additional RK3588 boards (CM3588, Rock 5B, Orange Pi 5 Plus)
- Updated component versions (newer Talos, rknpu driver, librknnrt)
- Bug reports via GitHub Issues

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

---

## 12. License

MIT — see [LICENSE](LICENSE).
