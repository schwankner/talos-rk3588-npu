# talos-rockchip-rk3588-npu

Talos Linux system extension and Kubernetes device plugin for the Rockchip RK3588 NPU.

Runs the RK3588 NPU in Kubernetes pods **without `privileged: true`** by leveraging Talos Linux's mainline kernel 6.18+ and the Container Device Interface (CDI).

Inspired by [talos-jetson-orin-nx](https://github.com/schwankner/talos-jetson-orin-nx) and [talos-sbc-rk3588](https://github.com/milas/talos-sbc-rk3588).

## Supported Hardware

| Board | SoC | Status |
|---|---|---|
| [Turing RK1](https://turingpi.com/product/turing-rk1/) | RK3588 | primary target |
| [FriendlyElec CM3588](https://www.friendlyelec.com/index.php?route=product/product&product_id=299) | RK3588 | planned |
| Radxa Rock 5B | RK3588 | planned |
| Orange Pi 5 Plus | RK3588S | planned |

The NPU system extension and device plugin are **board-agnostic** — only the Talos installer image (U-Boot + DTB) is board-specific.

## How It Works

### The Non-Privileged Problem — Solved

Running RKNN inference in Kubernetes normally requires `privileged: true` because the RKNN runtime reads `/proc/device-tree/compatible` to identify the SoC. Kubernetes masks `/proc` in all containers by default.

On the stock Rockchip BSP kernel (6.1), there is no workaround. On **Talos Linux with mainline kernel 6.18+**, this is solvable:

```
Mainline kernel 6.18
  → MOUNT_ATTR_IDMAP for tmpfs supported (requires 6.3+)
  → hostUsers: false works
  → procMount: Unmasked works
  → /proc/device-tree/compatible readable in container ✅
  → privileged: false ✅
```

### Architecture

```
┌─────────────────────────────────────────────────────┐
│  Talos Linux Node (RK3588, mainline kernel 6.18)    │
│                                                     │
│  System Extensions:                                 │
│  ├── rockchip-rknpu      (rknpu.ko kernel module)  │
│  └── rockchip-rknn-libs  (librknnrt.so)            │
│                                                     │
│  DaemonSet:                                         │
│  └── rk3588-npu-device-plugin                      │
│      ├── advertises: rockchip.com/npu: 1           │
│      └── writes: /var/run/cdi/rockchip-npu.yaml    │
└─────────────────────────────────────────────────────┘
                        │
                        ▼ CDI injection (no privileged)
┌─────────────────────────────────────────────────────┐
│  Pod spec                                           │
│                                                     │
│  spec:                                              │
│    hostUsers: false                                 │
│    securityContext:                                 │
│      procMount: Unmasked                            │
│    containers:                                      │
│      resources:                                     │
│        limits:                                      │
│          rockchip.com/npu: "1"                      │
│                                                     │
│  Injected by CDI:                                   │
│  ├── /dev/dri/renderD129  (NPU DRM render node)    │
│  ├── /dev/dri/card1       (NPU DRM master node)    │
│  ├── /dev/dma_heap/system                           │
│  └── /usr/lib/librknnrt.so (bind-mount)            │
└─────────────────────────────────────────────────────┘
```

## Repository Structure

```
talos-rockchip-rk3588-npu/
│
├── rockchip-rknpu/              # Talos system extension: rknpu kernel module
│   └── pkg.yaml
│
├── rockchip-rknn-libs/          # Talos system extension: librknnrt.so
│   └── pkg.yaml
│
├── plugins/
│   └── rk3588-npu-device-plugin/  # Kubernetes CDI device plugin
│       ├── main.go
│       ├── Dockerfile
│       └── go.mod
│
├── boards/                      # Board-specific: U-Boot + DTB only
│   ├── turing-rk1/
│   └── cm3588/
│
├── scripts/
│   ├── common.sh                # Shared version vars
│   ├── build-extensions.sh      # Builds rknpu + rknn-libs OCI extensions
│   ├── build-uki.sh             # Assembles Talos UKI with extensions
│   └── build-usb-image.sh       # Creates flashable .raw image per board
│
├── BUGS.md                      # Documented hard problems and their solutions
└── CHANGELOG.md
```

## Component Versions

See `scripts/common.sh` for all pinned versions. Key components:

| Component | Version |
|---|---|
| Talos Linux | see common.sh |
| Linux kernel | see common.sh |
| RKNN Runtime (librknnrt) | see common.sh |
| rknpu driver | see common.sh |

## Prerequisites

- ARM64 native build runner (no cross-compilation)
- Docker with BuildKit enabled (Podman also supported)
- GHCR write access (or a local OCI registry — set `REGISTRY`)

## Building

```bash
# 1. Build kernel + NPU system extensions (~30 min cold, ~5 min cached)
REGISTRY=ghcr.io/<org> ./scripts/build-extensions.sh

# 2. Build the Talos installer image (combines kernel, extensions, sbc-rockchip overlay)
REGISTRY=ghcr.io/<org> CONTAINER_RUNTIME=docker ./scripts/build-installer.sh
```

The installer image is pushed to `$REGISTRY/talos-rk3588-npu-installer-base:installer-v<talos-version>`.

## Installation

### 1. Flash base Talos to your board

Flash the standard Talos metal image first (via TPI, dd, or USB), then upgrade:

```bash
# Apply machine config (the node comes up in maintenance mode)
talosctl apply-config --insecure --nodes <ip> -f your-worker.yaml

# Upgrade to the NPU installer
talosctl upgrade --nodes <ip> \
  --image ghcr.io/<org>/talos-rk3588-npu-installer-base:installer-v<talos-version> \
  --preserve
```

After upgrade the sbc-rockchip overlay repartitions the eMMC (see BUGS.md Bug 13),
wiping STATE. Re-apply your machine config after the node reboots into maintenance mode.

### 2. Deploy the NPU device plugin

```bash
# Label NPU-capable nodes
kubectl label node <node-name> rockchip.com/npu-capable=true

# Deploy device plugin DaemonSet
kubectl apply -f deploy/rk3588-npu-device-plugin.yaml
```

### 3. Verify NPU is available

```bash
kubectl get nodes -o json | jq '.items[].status.allocatable | with_entries(select(.key | startswith("rockchip")))'
# Expected: { "rockchip.com/npu": "1" }
```

### 4. Run a pod with NPU access

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: rknn-test
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

## Relation to milas/talos-sbc-rk3588

[`milas/talos-sbc-rk3588`](https://github.com/milas/talos-sbc-rk3588) provides the Talos overlay for RK3588 boards (U-Boot, kernel build pipeline) but has no NPU support. This repo adds the NPU layer on top and is designed to eventually work alongside it.

The kernel configuration (`config-arm64`) is derived from that project.

## NPU Device Node

The `w568w/rknpu-module` driver (used here for mainline kernel compatibility) registers
the NPU as a **DRM device**, not a misc character device:

| Node | Purpose |
|------|---------|
| `/dev/dri/renderD129` | DRM render node — mount this into inference containers |
| `/dev/dri/card1` | DRM master node |

The exact minor numbers depend on probe order and vary by board configuration.
The device plugin discovers the correct node at runtime via
`/sys/bus/platform/drivers/RKNPU/fdab0000.rknpu/drm/` — do not hardcode the minor number.

## Known Issues

See [BUGS.md](BUGS.md) for documented issues and their root causes.

## License

MIT
