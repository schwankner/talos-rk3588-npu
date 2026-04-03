# talos-rockchip-rk3588-npu

Talos Linux system extension and Kubernetes device plugin for the Rockchip RK3588 NPU.

Runs the RK3588 NPU in Kubernetes pods **without `privileged: true`** by leveraging Talos Linux's mainline kernel 6.18+ and the Container Device Interface (CDI).

Inspired by and structurally based on [talos-jetson-orin-nx](https://github.com/schwankner/talos-jetson-orin-nx).

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
│  ├── /dev/rknpu                                     │
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

- ARM64 build runner (native, no cross-compilation)
- Docker with BuildKit enabled
- A local OCI registry for intermediate images

## Building

```bash
# Build NPU system extensions (board-agnostic, ~30 min cold)
make extensions

# Build a flashable USB image for a specific board
make usb BOARD=turing-rk1

# Build all board images
make all
```

## Installation

### 1. Flash Talos to your board

```bash
dd if=dist/talos-turing-rk1.raw of=/dev/sdX bs=4M status=progress
```

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

## Known Issues

See [BUGS.md](BUGS.md) for documented issues and their root causes.

## License

MIT
