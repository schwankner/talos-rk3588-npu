# Known Issues & Solutions

Hard-won solutions to non-obvious problems. Updated as issues are discovered and resolved.

---

## Bug 1: RKNN runtime falls back to CPU in Kubernetes pod

**Symptom:** RKNN inference runs on CPU instead of NPU. No error is thrown.

**Root cause:** The RKNN runtime (`librknnrt.so`) reads `/proc/device-tree/compatible` at startup to detect the SoC platform (`rk3588`). Kubernetes masks `/proc` in all containers by default via the mount namespace. When the runtime cannot read this file, it silently falls back to CPU.

**Why BSP kernel 6.1 cannot fix this:** `procMount: Unmasked` requires `hostUsers: false` (user namespaces), which requires `MOUNT_ATTR_IDMAP` support for tmpfs. This was added in Linux 6.3. The Rockchip BSP kernel is 6.1 and cannot be upgraded without losing vendor driver support.

**Solution (Talos mainline kernel 6.18+):**
```yaml
spec:
  hostUsers: false
  securityContext:
    procMount: Unmasked
```
This unmasks `/proc` in the container without requiring `privileged: true`.

**References:**
- https://github.com/immich-app/immich/issues/25057
- https://github.com/rsJames-ttrpg/npu-device-plugin

---

## Bug 2: rknpu module fails to load — missing device tree node

**Symptom:** `modprobe rknpu` fails, dmesg shows no NPU cores initialized.

**Root cause:** The `rknpu` driver requires a device tree node with `compatible = "rockchip,rknpu"`. If the DTB loaded by U-Boot does not include this node (e.g. wrong DTB for the board, or a minimal DTB), the driver has no device to bind to.

**Solution:** Ensure the correct board DTB is embedded in the Talos UKI. Verify with:
```bash
cat /proc/device-tree/compatible
# Should contain: rockchip,rk3588
```

---

## Bug 3: /dev/rknpu not created after module load

**Symptom:** `rknpu.ko` loads successfully (dmesg shows NPU cores), but `/dev/rknpu` does not appear.

**Root cause:** Talos runs a minimal `udevd`. If the udev rules for the `rknpu` misc device are not included in the system extension, the device node is not created automatically.

**Solution:** Include a udev rule in the `rockchip-rknpu` extension:
```
# /etc/udev/rules.d/90-rknpu.rules
KERNEL=="rknpu", MODE="0666", GROUP="video"
```

**Status:** To be confirmed during initial bringup.

---

## Bug 4: librknnrt version mismatch with rknpu driver

**Symptom:** RKNN API calls return error `-1` (RKNN_ERR_FAIL) or `-9` (version mismatch).

**Root cause:** `librknnrt.so` and the `rknpu` kernel driver must be from the same RKNN SDK release. The library encodes the expected driver ABI version and refuses to communicate with a mismatched kernel module.

**Solution:** Pin both `RKNN_RUNTIME_VERSION` and `RKNPU_DRIVER_VERSION` to the same SDK release in `scripts/common.sh`. When updating, update both atomically.

---

## Bug 5: CDI device injection not working — containerd not picking up spec

**Symptom:** Pod starts but `/dev/rknpu` is not present in container. Device plugin allocate is called but CDI injection silently fails.

**Root cause:** Containerd 2.x requires CDI to be explicitly enabled and the spec directory to be watched. Talos may not enable CDI by default.

**Solution:** Add to Talos machine config:
```yaml
machine:
  files:
    - path: /etc/cri/conf.d/cdi.toml
      permissions: 0644
      op: create
      content: |
        [plugins."io.containerd.grpc.v1.cri"]
          enable_cdi = true
          cdi_spec_dirs = ["/var/run/cdi", "/etc/cdi"]
```

**Status:** To be confirmed during initial bringup.

---

## Bug 8: OOT module build fails — clang rejects GCC-only kernel CFLAGS

**Symptom:** `clang: error: unknown argument: '-fmin-function-alignment=8'`, `-fconserve-stack`, `-fsanitize=bounds-strict` when building rknpu.ko with `LLVM=1`.

**Root cause:** The Talos 1.12.x kernel (pkgs `a92bed5`) is compiled with GCC 15.2.0 (`toolchain-musl`). When `CONFIG_CC_IS_CLANG=n`, the kernel adds GCC-specific flags to `KBUILD_CFLAGS`. Building an OOT module with `LLVM=1` makes clang receive those GCC-only flags.

**Solution:** Remove `LLVM=1` and `LLVM_IAS=1` from the `make` invocation in `pkg.yaml`. Use the GCC from `stage: base` (same toolchain that built the kernel). No LLVM image dependency is needed.

```makefile
# Correct:
make -j $(nproc) -C /src M=$(pwd) ARCH=arm64 modules

# Wrong (only valid when kernel was also built with Clang):
make -j $(nproc) -C /src M=$(pwd) ARCH=arm64 LLVM=1 LLVM_IAS=1 modules
```

**Note:** Future Talos releases may switch to LLVM kernel builds. If the kernel `scripts/cc-version.sh` output shows `clang`, re-enable LLVM=1.

---

## Bug 6: procMount: Unmasked rejected — "no space left on device" in pod creation

**Symptom:** Pod fails to start with event `no space left on device` when `hostUsers: false` is set. Or: `procMount: Unmasked` is rejected if `hostUsers: false` is missing.

**Root cause (part 1):** Kubernetes enforces that `procMount: Unmasked` requires `hostUsers: false`. Without it, the API server rejects the pod spec.

**Root cause (part 2):** `hostUsers: false` requires user namespaces on the node. User namespaces are controlled by the kernel sysctl `user.max_user_namespaces`. On a freshly installed Talos node the default is `0` (disabled), which causes the "no space left on device" error when the kubelet tries to create the user namespace for the pod.

**Solution:**
```yaml
# patches/all.patch9.yaml — apply to all nodes, no reboot required
machine:
  sysctls:
    user.max_user_namespaces: "15000"
```
Apply with: `talosctl patch mc --patch @patches/all.patch9.yaml -n <ip>`

The pod spec must use both fields together:
```yaml
spec:
  hostUsers: false
  containers:
    - securityContext:
        procMount: Unmasked
```

**Note:** `procMount` is a **container-level** field (`containers[].securityContext`), not pod-level. Placing it under `spec.securityContext` causes a validation error.

---

## Bug 7: sbc-rockchip rknn.patch adds wrong compatible string for vendor rknpu driver

**Symptom:** After upgrading to sbc-rockchip v0.2.0 (Talos v1.12.6), dmesg shows NPU power domains active (`fdab0000.npu: Adding to iommu group 7/8/9`) but the vendor rknpu driver does not bind, and `/dev/rknpu` is never created.

**Root cause:** The sbc-rockchip rknn.patch (merged in v0.1.8) adds DT nodes with `compatible = "rockchip,rk3588-rknn-core"` — this is for the **mainline rocket driver** (`drivers/accel/rocket`), not the vendor rknpu driver. The vendor driver (`w568w/rknpu-module`) matches `compatible = "rockchip,rk3588-rknpu"`, which is absent from the sbc-rockchip DTB.

**Two drivers, two compatible strings:**
| Driver | compatible | device | LLM |
|--------|-----------|--------|-----|
| rocket (mainline) | `rockchip,rk3588-rknn-core` | `/dev/accel/accel0` | No |
| rknpu (vendor) | `rockchip,rk3588-rknpu` | `/dev/rknpu` | Yes (RKLLM) |

**Solution:** A DTB overlay that adds the rknpu-compatible node must be applied at boot. The overlay targets the same hardware addresses (fdab0000/fdac0000/fdad0000) but with the `rockchip,rk3588-rknpu` compatible. This overlay is provided by the `rockchip-rknpu` Talos extension and loaded via Talos `machine.kernel.modules` or an initramfs DT overlay hook.

**Status:** DTB overlay implementation pending — see `boards/` directory.

---

*Add new bugs above this line, most recent first.*
