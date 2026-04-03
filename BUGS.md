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

*Add new bugs above this line, most recent first.*
