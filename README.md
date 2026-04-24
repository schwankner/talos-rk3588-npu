# talos-rockchip-rk3588-npu

Talos Linux system extensions and Kubernetes device plugin for the Rockchip RK3588 NPU.

Runs RKNN inference in Kubernetes pods **without `privileged: true`** by combining
Talos Linux's mainline kernel 6.18+ with the Container Device Interface (CDI).

Inspired by [talos-jetson-orin-nx](https://github.com/schwankner/talos-jetson-orin-nx)
and [talos-sbc-rk3588](https://github.com/milas/talos-sbc-rk3588).

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
10. [Benchmark Results](#10-benchmark-results)
11. [Known Issues](#11-known-issues)
12. [Contributing](#12-contributing)
13. [License](#13-license)

---

## 1. Supported Hardware

| Board | SoC | Status |
|-------|-----|--------|
| [Turing RK1](https://turingpi.com/product/turing-rk1/) (on Turing Pi 2) | RK3588 | ✅ Tested |
| [FriendlyElec CM3588](https://www.friendlyelec.com/index.php?route=product/product&product_id=299) | RK3588 | 🔧 Planned |
| Radxa Rock 5B | RK3588 | 🔧 Planned |
| Orange Pi 5 Plus | RK3588S | 🔧 Planned |

The NPU system extension and device plugin are **board-agnostic** — only the Talos
installer image (U-Boot + DTB) is board-specific.

---

## 2. How It Works

### The Non-Privileged Problem — Solved

Running RKNN inference in Kubernetes normally requires `privileged: true` for two reasons:

1. The RKNN runtime reads `/proc/device-tree/compatible` to detect the SoC. Kubernetes
   masks `/proc` in all containers by default.
2. The NPU device node (`/dev/rknpu`) and DMA heap (`/dev/dma_heap/system`) need to be
   injected into the container, along with `librknnrt.so`.

On the stock Rockchip BSP kernel (6.1), there is no non-privileged workaround. On
**Talos Linux with mainline kernel 6.18+**, both problems are solved:

```
Mainline kernel 6.18+
  → MOUNT_ATTR_IDMAP for tmpfs (requires kernel 6.3+)
  → spec.hostUsers: false  +  securityContext.procMount: Unmasked
  → /proc/device-tree/compatible readable in container ✅

CDI device plugin (rockchip.com/npu: "1" in resources.limits)
  → /dev/rknpu injected (misc device, rknpu.ko)
  → /dev/dma_heap/system injected (zero-copy DMA buffers)
  → /usr/lib/librknnrt.so bind-mounted from host
  → privileged: false ✅
```

### Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  Talos Linux Node (RK3588, mainline kernel 6.18)             │
│                                                              │
│  System Extensions (baked into installer, loaded at boot):   │
│  ├── rockchip-rknpu      rknpu.ko (out-of-tree kernel module)│
│  └── rockchip-rknn-libs  librknnrt.so (Rockchip RKNN SDK)   │
│                                                              │
│  DaemonSet:                                                  │
│  └── rk3588-npu-device-plugin                               │
│      ├── advertises: rockchip.com/npu: "1"                  │
│      └── writes: /var/run/cdi/rockchip-npu.yaml             │
└──────────────────────────────────────────────────────────────┘
                          │
                          ▼  CDI injection at pod start (no privileged)
┌──────────────────────────────────────────────────────────────┐
│  Pod                                                         │
│                                                              │
│  spec:                                                       │
│    hostUsers: false                                          │
│    securityContext:                                          │
│      procMount: Unmasked                                     │
│    containers:                                               │
│      resources:                                              │
│        limits:                                               │
│          rockchip.com/npu: "1"                               │
│                                                              │
│  Injected by CDI:                                            │
│  ├── /dev/rknpu            (NPU misc device, rknpu.ko)      │
│  ├── /dev/dma_heap/system  (DMA buffer allocator)           │
│  └── /usr/lib/librknnrt.so (RKNN runtime, bind-mount)       │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. Quick Start

```bash
# 1. Upgrade node to the NPU installer image (custom kernel + signed rknpu.ko)
talosctl upgrade \
  --nodes <NODE_IP> \
  --talosconfig ./npu-test-talosconfig \
  --image ghcr.io/schwankner/talos-rk3588-npu-installer-base:installer-v1.12.6 \
  --preserve

# 2. Apply machine config (adds NPU extensions + CDI containerd config)
talosctl apply-config \
  --nodes <NODE_IP> \
  --talosconfig ./npu-test-talosconfig \
  -f machine.yaml

# 3. Deploy device plugin
kubectl --kubeconfig npu-test-kubeconfig \
  apply -f deploy/device-plugin.yaml

# 4. Verify NPU is advertised
kubectl --kubeconfig npu-test-kubeconfig \
  get nodes -o json \
  | jq '.items[].status.allocatable | with_entries(select(.key | startswith("rockchip")))'
# → { "rockchip.com/npu": "1" }
```

---

## 4. Repository Structure

```
talos-rk3588-npu/
│
├── rockchip-rknpu/              # Talos system extension: rknpu.ko kernel module
│   ├── pkg.yaml                 # bldr build spec
│   ├── manifest.yaml            # Talos extension metadata
│   └── files/                   # Kbuild sources, udev rules, rknpu_mem.c fix
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
├── boards/                      # Board-specific: U-Boot + DTB only
│   └── turing-rk1/
│
├── deploy/                      # Kubernetes manifests
│   └── device-plugin.yaml
│
├── kernel/                      # Kernel config fragments
│   └── config-arm64-rk3588-npu.fragment
│
├── scripts/
│   ├── common.sh                # All pinned version variables (single source of truth)
│   ├── build-extensions.sh
│   ├── build-installer.sh
│   └── build-usb-image.sh
│
├── test/rknn-bench/             # NPU vs CPU benchmark container
│   ├── Dockerfile               # Multi-stage: compiles ResNet50 ONNX → RKNN
│   ├── bench.py                 # Benchmark harness (resnet18 / resnet50 / yolov5s)
│   ├── bench-npu.yaml           # Kubernetes Job: ResNet50 on NPU
│   └── bench-cpu.yaml           # Kubernetes Job: ResNet50 on CPU (baseline)
│
├── .github/workflows/
│   ├── ci.yaml                  # Shellcheck, go vet, YAML validation
│   ├── build-extensions.yaml    # Builds OCI extension images
│   ├── build-device-plugin.yaml # Builds device plugin container
│   ├── build-installer.yaml     # Builds custom Talos installer
│   ├── build-bench.yaml         # Builds RKNN bench container
│   ├── auto-tag.yaml            # Tags on version-file changes
│   ├── release.yaml             # Full release pipeline
│   └── check-talos.yaml         # Daily check for new Talos releases
│
├── BUGS.md                      # Hard-won solutions, detailed root causes
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
| rknpu driver | 0.9.10 | [w568w/rknpu-module](https://github.com/w568w/rknpu-module), mainline-compatible |
| librknnrt.so | 2.3.2 | [airockchip/rknn-toolkit2](https://github.com/airockchip/rknn-toolkit2) |

---

## 6. Building

All production builds run in **GitHub Actions** on native `ubuntu-24.04-arm` runners —
no QEMU or cross-compilation needed.

### Trigger a build manually

```bash
# Build system extensions (rknpu.ko + librknnrt.so)
gh workflow run "Build Extensions" \
  --repo schwankner/talos-rk3588-npu --ref main

# Build custom Talos installer (required when Talos or kernel version changes)
gh workflow run "Build Installer" \
  --repo schwankner/talos-rk3588-npu --ref main
```

### Automatic tagging and releases

Push to `main` with a change to `scripts/common.sh`:

- `ci.yaml` — shellcheck, go vet, YAML validation
- `auto-tag.yaml` — creates git tag `v<talos>-rknpu<rknpu>` if versions changed
- Tag triggers `release.yaml` → builds extensions + device plugin + installer → GitHub Release

### Produced images

| Image | Tag format | Example |
|-------|-----------|---------|
| `ghcr.io/schwankner/rockchip-rknpu` | `<rknpu>-<kernel>` | `0.9.10-6.18.18-talos` |
| `ghcr.io/schwankner/rockchip-rknn-libs` | `<rknn>-<kernel>` | `2.3.2-6.18.18-talos` |
| `ghcr.io/schwankner/rk3588-npu-device-plugin` | `v<release>` | `v1.12.6-rknpu0.9.10` |
| `ghcr.io/schwankner/talos-rk3588-npu-installer-base` | `installer-v<talos>` | `installer-v1.12.6` |

> **Note:** Extensions are **baked into the installer** as squashfs blobs at build time.
> They are not pulled from the registry at node boot. Always rebuild the installer after
> rebuilding extensions.

---

## 7. Installation

### Prerequisites

- Talos Linux v1.12.6 on your RK3588 board (via [factory.talos.dev](https://factory.talos.dev) with `sbc-rockchip` overlay)
- `talosctl` v1.12.6, `kubectl`
- Images are public on GHCR; no authentication needed to pull

### Step 1 — Upgrade to the NPU installer

The custom installer embeds the kernel whose module-signing key was used to build
`rknpu.ko`. This key-match is required by Talos's `module.sig_enforce=1`.

```bash
talosctl upgrade \
  --nodes <NODE_IP> \
  --talosconfig ./your-talosconfig \
  --image ghcr.io/schwankner/talos-rk3588-npu-installer-base:installer-v1.12.6 \
  --preserve
```

### Step 2 — Add system extensions and CDI to machine config

```yaml
machine:
  install:
    extensions:
      - image: ghcr.io/schwankner/rockchip-rknpu:0.9.10-6.18.18-talos
      - image: ghcr.io/schwankner/rockchip-rknn-libs:2.3.2-6.18.18-talos

  files:
    - path: /etc/cri/conf.d/20-customization.part
      op: create
      permissions: 0o644
      content: |
        [plugins."io.containerd.cri.v1.runtime"]
          enable_cdi_devices = true
          cdi_spec_dirs = ["/var/run/cdi"]

  sysctls:
    user.max_user_namespaces: "15000"
```

Apply and let the node reboot:

```bash
talosctl apply-config \
  --nodes <NODE_IP> \
  --talosconfig ./your-talosconfig \
  -f machine.yaml
```

### Step 3 — Deploy the device plugin

```bash
kubectl apply -f deploy/device-plugin.yaml
```

### Step 4 — Verify

```bash
# Extensions loaded
talosctl get extensions \
  --nodes <NODE_IP> --talosconfig ./your-talosconfig
# → rockchip-rknpu, rockchip-rknn-libs listed

# rknpu.ko in dmesg
talosctl dmesg \
  --nodes <NODE_IP> --talosconfig ./your-talosconfig | grep rknpu

# NPU resource advertised by device plugin
kubectl get node -o json \
  | jq '.items[].status.allocatable | with_entries(select(.key | startswith("rockchip")))'
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
      resources:
        limits:
          rockchip.com/npu: "1"
```

### What CDI injects

When `rockchip.com/npu: "1"` is in `resources.limits`, the device plugin's CDI spec
causes containerd to inject:

| Path in container | Source on host | Purpose |
|-------------------|----------------|---------|
| `/dev/rknpu` | `/dev/rknpu` | NPU misc device (rknpu.ko) — inference job submission |
| `/dev/dma_heap/system` | `/dev/dma_heap/system` | Zero-copy CPU↔NPU DMA buffer allocation |
| `/usr/lib/librknnrt.so` | `/usr/lib/librknnrt.so` | RKNN runtime (bind-mount from Talos extension) |

### Why `hostUsers: false` + `procMount: Unmasked`

`librknnrt.so` calls `open("/proc/device-tree/compatible", O_RDONLY)` during
`init_runtime()` to identify the SoC. Without `procMount: Unmasked`, this path is
hidden by a read-only empty tmpfs and `init_runtime()` fails with a generic error.

`procMount: Unmasked` requires user namespaces (`hostUsers: false`), which in turn
requires mainline kernel 6.3+ (`MOUNT_ATTR_IDMAP` for tmpfs). The Talos sbc-rockchip
overlay ships kernel 6.18 which satisfies this.

> **This does not work on BSP kernel 6.1** (Orange Pi stock, Radxa stock images) —
> no workaround exists on that kernel.

---

## 9. Device Plugin Details

### Resource name

`rockchip.com/npu` — advertises exactly 1 unit per node. The RK3588 NPU is a
single shared resource (multiple pods may serialize, not parallelize).

### Device discovery

The plugin checks for `/dev/rknpu` at startup, then polls every 10 s. The NPU
is exposed as a BSP misc device by `rknpu.ko` (`CONFIG_ROCKCHIP_RKNPU_DMA_HEAP=y`).
No DRM render nodes (`/dev/dri/renderD*`) are involved in this architecture.

### udev rules

`rknpu.ko` ships a udev rule (`90-rknpu.rules`) that:
- Sets `/dev/rknpu` to `0666` (unprivileged container reads/writes)
- Sets `/dev/dma_heap/system` to `0666` (required for DMA buffer allocation)

---

## 10. Benchmark Results

Tested on **Turing RK1 (RK3588)** running in a Kubernetes pod (no `privileged: true`,
CDI-injected devices, `procMount: Unmasked`).

All models run through `rknn-toolkit-lite2` + `librknnrt.so 2.3.2`.
CPU mode uses the ARM Cortex-A76 fallback path in `librknnrt.so`.

### ResNet18 — 224×224, batch 1 (~1.8 GFLOPS)

| Mode | Throughput | Latency | Speedup |
|------|-----------|---------|---------|
| NPU (RKNPU v2, NPU_CORE_AUTO) | 146.8 fps | 6.81 ms | 1.0× (baseline) |
| CPU (ARM Cortex-A76 NEON)     | 152.7 fps | 6.55 ms | 1.04× |

ResNet18 is small enough that the A76 NEON path fully saturates the 6 TOPS NPU
at batch-1. Both modes are compute-bound by memory bandwidth at this scale.

### ResNet50 — 224×224, fp16, batch 1 (~8.2 GFLOPS)

Model compiled with `rknn-toolkit2 2.3.2` (matching runtime), `do_quantization=False`
(fp16). 200 NPU iterations / 30 CPU iterations, 10 warmup each.

| Mode | Throughput | Latency | Speedup |
|------|-----------|---------|---------|
| NPU (RKNPU v2, NPU_CORE_AUTO) | 29.3 fps | 34.16 ms | 1.19× |
| CPU (ARM Cortex-A76 NEON)     | 24.7 fps | 40.44 ms | 1.0× (baseline) |

At fp16 without INT8 quantization the NPU advantage is modest (1.2×) — the A76 NEON
path handles fp16 convolutions efficiently. INT8 quantized models typically show
**10–30× NPU speedup**, which is the intended production use case.

> The key result is not the speedup ratio — it is that the full stack works
> end-to-end: `rknpu 0.9.10`, `librknnrt 2.3.2`, CDI device injection, `procMount:
> Unmasked`, Talos 1.12.6 / kernel 6.18.18, **no `privileged: true`**.

---

## 11. Known Issues

See [BUGS.md](BUGS.md) for documented issues and solutions. Selected entries:

| Bug | Summary |
|-----|---------|
| Bug 43 | `virt_dev` suspended after probe, NPU PM domain not reachable |
| Bug 44 | `nputop` genpd domain not attached on mainline DT |
| Bug 45/46 | NPU IOMMU clock gating causes AXI lockup in `init_runtime()` |
| Bug 47 | `RKNPU_MEM_CREATE` ioctl unimplemented — fixed with `dma_alloc_coherent` |
| Bug 47 rev 2 | Wrong `obj_addr` returned from `RKNPU_MEM_CREATE` caused silent 3×60 s timeout |

---

## 12. Contributing

Contributions welcome — especially:

- Support for additional RK3588 boards (CM3588, Rock 5B, Orange Pi 5 Plus)
- Updated component versions (newer Talos, rknpu driver, librknnrt)
- INT8 quantization benchmarks (expected 10–30× NPU speedup)
- Bug reports via GitHub Issues

---

## 13. License

MIT — see [LICENSE](LICENSE).
