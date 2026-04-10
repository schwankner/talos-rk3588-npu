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

## Bug 9: rknpu_devfreq.c fails to compile / undefined symbols at modpost

**Symptom (phase 1):** `src/rknpu_devfreq.c:14:10: fatal error: linux/devfreq-governor.h: No such file or directory`

**Symptom (phase 2, after removing devfreq.o from Kbuild):** modpost fails: `"rknpu_devfreq_init" [rknpu.ko] undefined!` (and 5 other devfreq symbols).

**Root cause:** The Talos 1.12.x `kernel-build` stage has `CONFIG_PM_DEVFREQ=y` in `autoconf.h` BUT does not export `include/linux/devfreq-governor.h` (it is an internal-use header not installed by `modules_prepare`). As a result:
- `rknpu_devfreq.c` cannot be compiled (header missing).
- `rknpu_devfreq.h` emits real *declarations* (not inline stubs) because `CONFIG_PM_DEVFREQ` is defined.
- Removing `rknpu_devfreq.o` from Kbuild while CONFIG is set causes modpost to fail: functions are declared but not provided.

**Solution:** Ship a patched `Kbuild` in `files/Kbuild` that:
1. Omits `src/rknpu_devfreq.o` from `rknpu-y`.
2. Adds `-DRKNPU_NO_DEVFREQ` to `ccflags-y`.

`RKNPU_NO_DEVFREQ` is an intentional escape hatch in `rknpu_devfreq.h`: when defined, it forces the header to emit inline no-op stubs regardless of `CONFIG_PM_DEVFREQ`, so all callers link cleanly.

**Why a file in `files/` instead of a `sed` in `prepare`:** bldr caches the `prepare` layer based on source checksums. A `sed` command in `prepare` that modifies files is invisible to the cache — the next run reuses the old cached layer. Shipping `files/Kbuild` changes the build context, which forces proper cache invalidation.

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

## Bug 10: rknpu.ko fails to load — "key was rejected by service" (module.sig_enforce=1)

**Symptom:** After installing the rockchip-rknpu extension and adding `machine.kernel.modules: [{name: rknpu}]`, the module never loads. `dmesg` shows:
```
[talos] controller failed {"controller": "runtime.KernelModuleSpecController",
  "error": "error loading module \"rknpu\": load rknpu failed: key was rejected by service"}
```
`/dev/rknpu` never appears.

**Root cause:** Talos enforces `module.sig_enforce=1` on the kernel command line. Every `.ko` must be signed with the *same* private key used to build the running kernel. The OOT module build (`make -C /src M=$(pwd) modules`) compiles `rknpu.ko` but does **not** automatically sign it — `CONFIG_MODULE_SIG_ALL` is `n` in the Talos kernel config, so the kernel Makefile only signs built-in modules, not OOT ones.

**Fix:** Explicitly call `sign-file` in the `install:` step of `rockchip-rknpu/pkg.yaml`, using the key and certificate that the `kernel-build` stage keeps alongside the kernel source:

```bash
/src/scripts/sign-file sha256 \
  /src/certs/signing_key.pem \
  /src/certs/signing_key.x509 \
  rknpu.ko
```

This signs `rknpu.ko` with the exact key embedded in the running Talos kernel (since both the kernel and the OOT module are built from the same `PKGS_COMMIT`), so the kernel's built-in keyring accepts it.

**Verified:** Signing step added in pkg.yaml install phase; rebuild + re-deploy required.

---

## Bug 11: Talos imager ignores --base-installer-image vmlinuz; uses its own kernel

**Symptom:** After upgrading with a custom installer that was built with `--base-installer-image` pointing to our vmlinuz-patched base, the running kernel still shows `Sidero Labs, Inc.: Build time throw-away kernel key` in `/proc/keys`. The rknpu module fails to load with `key was rejected by service`.

**Root cause:** The Talos imager (`ghcr.io/siderolabs/imager`) has the Talos kernel binary embedded at `/usr/install/arm64/vmlinuz` **inside the imager image itself**. The `--base-installer-image` flag only controls the installer binary (grub, installer script) — the imager completely ignores any vmlinuz placed in the base installer image. The UKI is always built from the imager's own embedded kernel (with the Siderolabs ephemeral signing key).

**How to verify:** Inspect the imager image:
```bash
podman create --platform linux/arm64 ghcr.io/siderolabs/imager:v1.12.6 | xargs podman export | tar -t | grep vmlinuz
# shows: usr/install/arm64/vmlinuz  ← imager's own kernel
```

**Solution:** Build a **custom imager image** that replaces the imager's own kernel binary:
```dockerfile
FROM ghcr.io/siderolabs/imager:v1.12.6
COPY vmlinuz /usr/install/arm64/vmlinuz
```
Where `vmlinuz` comes from our own bldr build of the `kernel` target at `PKGS_COMMIT=a92bed5`.
See `scripts/build-extensions.sh` (`build_kernel` function) and `scripts/build-installer.sh`.

**Key insight:** The module signing key is **ephemeral** — it is generated at kernel build time
(`CONFIG_MODULE_SIG_KEY=""` in `kernel/build/certs/x509.genkey`) and **not committed** to the
pkgs source tree. The siderolabs CI generates its own ephemeral key K_SL; our bldr build
generates K1. `rknpu.ko` is signed with K1 (via `kernel-build` stage → `sign-file`). The
siderolabs imager's embedded kernel has K_SL in its built-in keyring. K1 ≠ K_SL → "key was
rejected by service".

The fix is to ensure both the running kernel AND rknpu.ko use the same key (K1) by building the
kernel with bldr and inserting it into the imager. Both builds share the `kernel-build` bldr
cache layer, which contains `certs/signing_key.pem` — so they always use the same key.

---

## Bug 12: `machine.install.extraKernelArgs` silently ignored by `talosctl upgrade` (Talos 1.12)

**Symptom:** After `talosctl upgrade`, the node boots with no network. Only loopback (127.0.0.1) is assigned. `talosctl dmesg` shows `network is unreachable` for every DNS/NTP attempt. The VLAN interface (`end0.60`) never appears.

**Root cause:** In Talos 1.12, `machine.install.extraKernelArgs` is treated as an **initial-install-only** parameter. During `talosctl upgrade`, the imager builds a new UKI from its embedded kernel and the cmdline baked into the installer image — it does **not** re-read `machine.install.extraKernelArgs` from the machine config. Any kernel arguments that were only specified there are silently absent from the new UKI.

**Concrete impact:** `vlan=end0.60:end0` and `ip=10.0.60.4::...::end0.60:off` were specified in `machine.install.extraKernelArgs`. After upgrade these were missing from the UKI cmdline, so the VLAN interface was never created and the node had no network access at boot — before Talos could apply the machine config that would have restored it.

**Solution:** Bake all required kernel arguments into the **installer image** itself using `--extra-kernel-arg` flags when running the imager:

```bash
"${CONTAINER_RUNTIME}" run --rm \
    -v "${INSTALLER_OUT}:/out" \
    "${CUSTOM_IMAGER_REF}" \
    installer \
    --arch arm64 \
    --extra-kernel-arg vlan=end0.60:end0 \
    --extra-kernel-arg "ip=10.0.60.4::10.0.60.254:255.255.255.0::end0.60:off" \
    --extra-kernel-arg talos.config=http://10.0.60.1:9090/worker.yaml \
    ...
```

Arguments passed via `--extra-kernel-arg` are embedded in the UKI cmdline at image build time and survive upgrades. See `scripts/build-installer.sh`.

**Note:** `machine.install.extraKernelArgs` is not entirely useless — it applies on the very first install (from maintenance mode). But for anything that must survive upgrades, the installer image is the only reliable location.

---

## Bug 13: turingrk1 sbc-rockchip overlay repartitions eMMC during `talosctl upgrade`, destroying STATE and META

**Symptom:** After `talosctl upgrade --preserve --image <custom-installer>`, the node reboots but Talos enters an install loop — it has no machine config, no cluster identity, and no installed extensions. `talosctl disks` on the node shows the eMMC has been completely repartitioned.

**Root cause:** The `ghcr.io/siderolabs/sbc-rockchip` overlay for `turingrk1` contains an `installers/turingrk1` binary that uses `*overlay.PartitionOptions`. This signals to the Talos installer that the overlay needs to control the partition layout. The Talos installer honors this by performing a **full disk repartition** regardless of the `--preserve` flag. Partitions p4 (META) and p5 (STATE) are wiped, destroying the machine config and cluster secrets.

**Why `--preserve` doesn't help:** `--preserve` prevents the installer from wiping the STATE filesystem contents, but only if STATE survives repartitioning. When the partition table itself is rewritten (as the turingrk1 overlay demands), STATE is destroyed before `--preserve` logic can act.

**Consequence (compound with Bug 12):** After STATE is wiped, the node has no machine config. On next boot it enters maintenance mode and fetches config from the URL embedded in the UKI cmdline (the schematic's `talos.config=` arg). If that URL points to the wrong config (e.g., an old non-NPU config), the node reinstalls with the wrong installer image, reverting all customization.

**Mitigation (until overlay is fixed):**
1. Bake `talos.config=http://<correct-server>/worker.yaml` into the installer via `--extra-kernel-arg` so that even after STATE is wiped, the node fetches the correct config.
2. Ensure the config server at that URL always serves the current NPU machine config.
3. Accept that each upgrade is effectively a full reinstall; the node will re-apply config automatically once it can reach the config server.

**Permanent fix (TODO):** Investigate whether a custom overlay without `PartitionOptions` can avoid repartitioning on upgrade while still laying down the correct U-Boot + DTB for the Turing RK1.

---

## Bug 14: sbc-rockchip overlay installer silently discards --system-extension-image flags

**Symptom:** After upgrading with a custom installer built with `--system-extension-image` flags and `--overlay-image sbc-rockchip --overlay-name turingrk1`, the node boots with only the schematic extension loaded. `talosctl get extensions` shows no rknpu or rknn-libs. `dmesg` shows only two loop devices at boot (schematic + rootfs); no extension squashfs loop devices appear. `modprobe rknpu` fails with "module not found".

**Root cause:** The `sbc-rockchip` overlay installer (v0.2.0) intercepts `--system-extension-image` flags in the Talos imager profile. Instead of following the standard Talos code path that embeds extension squashfs files inside the UKI's initramfs CPIO, it places them under `overlayInstaller.imageRefs` in the imager profile. The UKI built by this path contains only the schematic squashfs and the main Talos rootfs squashfs in its initramfs — no extension squashfs files are ever embedded.

**Confirmation:** Inspecting the installer image layers via the OCI registry API shows that `ghcr.io/siderolabs/sbc-rockchip:v0.2.0` in layer 2 contains only `usr/install/arm64/vmlinuz.efi` and `usr/install/arm64/systemd-boot.efi`. No `*.sqsh` files exist in any layer. At boot, only loop0 (schematic, 4 KB) and loop1 (rootfs, ~68 MB) are created — confirming the initramfs has no extension squashfs.

**Solution:** Two-pass imager build in `scripts/build-installer.sh`:

1. **Pass 1** (no overlay, with extensions): Run the imager with `--system-extension-image` flags but WITHOUT `--overlay-image`. The standard Talos imager code path correctly embeds extension squashfs files inside the UKI initramfs. Also pass all `--extra-kernel-arg` flags so the cmdline is baked into this UKI.

2. **Pass 2** (with overlay, no extensions): Run the imager with `--overlay-image sbc-rockchip --overlay-name turingrk1` but WITHOUT `--system-extension-image`. Produces the board-support artifacts (U-Boot, DTBs, overlay installer binary).

3. **Combine**: Extract `vmlinuz.efi` from the pass-1 installer (has extension squashfs + correct cmdline in initramfs). Build a new image `FROM` the pass-2 installer (has board support), replacing its bare `vmlinuz.efi` with the extension-bearing one via `COPY`.

The combined installer has both extension squashfs files embedded in the UKI initramfs AND the correct U-Boot/DTB board support artifacts.

**Note:** This is a bug in sbc-rockchip v0.2.0. A future sbc-rockchip release may fix this; at that point the two-pass workaround can be simplified back to a single imager invocation.

---

## Bug 15: Baking `vlan=` / static `ip=` into installer kernel args breaks maintenance mode

**Symptom:** Node boots into Talos maintenance mode but is unreachable — machined logs only
`"waiting for network to be ready"` forever. 10.0.60.4 never responds to ping. The UART shows
`"network is unreachable"` for 8.8.8.8 every second with no progress.

**Root cause:** `build-installer.sh` was writing `--extra-kernel-arg vlan=end0.60:end0` and
`--extra-kernel-arg "ip=10.0.60.4::10.0.60.254:255.255.255.0::end0.60:off"` into the grub.cfg
baked into the installer image. When the node lands in maintenance mode (no STATE partition),
machined cannot configure the VLAN because:
1. The Turing Pi 2's internal switch does not pass VLAN 60 tagged frames on node ports until the
   machine config (VLANSpec) is applied by machined — a chicken-and-egg problem.
2. machined needs to receive the machine config first (via maintenance API or talos.config URL)
   to know about VLAN 60, but without VLAN 60 it cannot reach the talos.config server on
   10.0.60.x.

Additionally, `ip=dhcp` (via `CONFIG_IP_PNP_DHCP`) triggers kernel-level DHCP which hangs for
300–350 s on RK3588 due to `sync_state() pending due to fe1c0000.ethernet` (PM domain not ready
at early boot), blocking machined from starting during that window.

**Solution:**
1. Remove all `vlan=`, `ip=`, and `talos.config=` args from both imager passes in
   `build-installer.sh`. Network config belongs in the machine config, not the installer.
2. To re-provision a node stuck in maintenance mode, flash a clean Talos metal image and apply
   the machine config via talosctl:
   ```bash
   # Flash standard metal image (gets DHCP on management VLAN 10.0.70.x)
   tpi flash -n 4 --image-path talos-turing-rk1-v1.12.6.raw --host 10.0.70.2 --user root --password turing
   # Power cycle
   tpi power off -n 4 --host 10.0.70.2 --user root --password turing
   tpi power on  -n 4 --host 10.0.70.2 --user root --password turing
   # Wait for maintenance mode (~20s), find IP from UART, then:
   talosctl apply-config --insecure --nodes 10.0.70.17 -f worker.yaml
   ```
3. The management DHCP (10.0.70.x) works with the standard metal image because the standard
   grub.cfg has no VLAN args — machined does DHCP on the bare end0 interface. After the machine
   config is applied, machined configures VLAN 60 and the switch starts passing tagged frames.

**Key insight:** Installer kernel args persist in grub.cfg after install. Any arg that interferes
with maintenance mode DHCP will make the node unrecoverable without physical intervention.

---

## Bug 16: NPU installer breaks Ethernet in maintenance mode — node unreachable after upgrade

**Symptom:** After `talosctl upgrade --image <npu-installer>`, the node reboots, enters
maintenance mode (due to Bug 13 repartitioning) and then never responds to ping or talosctl on
any IP address. The UART shows Talos booted successfully and entered maintenance mode, but only
NTP failures (`network is unreachable`) fill the log indefinitely. No DHCP is obtained. The
standard `tpi flash` + `apply-config` recovery cycle works fine (standard siderolabs kernel gets
DHCP on 10.0.70.x within 30s).

**Root cause (confirmed — two independent issues):**

**Issue A — initramfs module signing key mismatch (primary, caused no-network):**
The siderolabs imager ships `/usr/install/arm64/initramfs.xz` whose `lib/modules/<kver>/`
tree is signed with the Siderolabs ephemeral build key. Our bldr-built kernel runs with
`module.sig_enforce=1` and rejects any module signed with a foreign key. `dwmac-rk.ko`
(the RK3588 GMAC/ethernet driver) ships as a loadable module, NOT built-in. When udevd
tries to `modprobe dwmac_rk` for `fe1c0000.ethernet`, the kernel rejects it silently.
No ethernet driver → no DHCP → node appears dead on the network.

The earlier `build-installer.sh` replaced `/usr/install/arm64/vmlinuz` with our
bldr-built kernel, but left the siderolabs-signed modules in `initramfs.xz` untouched.

UART evidence (custom kernel boot, before fix):
```
[8.907292] Loading of module with unavailable key is rejected
[8.908194] Loading of module with unavailable key is rejected
```
(Two rejections: `dwmac-rk.ko` once per modprobe attempt; ethernet never probes.)

**Issue B — DTB `assigned-clocks` in vendor NPU node (secondary, caused PM domain delay):**
The initial DTB patch inserted `assigned-clocks = <0x0a 0x06>` and
`assigned-clock-rates = <0xbebc200>` into the vendor `rknpu@fdab0000` node. The SCMI
firmware processes these during probe, which interferes with the PM domain sync_state
accounting and delays/blocks `fe1c0000.ethernet` probe in some boot paths. Fixed by
omitting both properties from NEW_NODE.

**Evidence:** UART from earlier NPU installer boot with wrong DTB + wrong initramfs:
```
[ 8.160074] Loading of module with unavailable key is rejected
[16.237890] rockchip-pm-domain ...: sync_state() pending due to fe1c0000.ethernet
```
Ethernet never probed. With patched DTB + vanilla kernel (correct signing), ethernet probed
at t=8.7s and node received DHCP. This isolated Issue B from Issue A.

**Recovery:** Use `tpi flash` to write the standard Talos metal image, then re-apply config:
```bash
tpi flash -n 4 --image-path /private/tmp/talos-turingrk1-v1.12.6.raw \
  --host 10.0.10.40 --user root --password turing
tpi power off -n 4 --host 10.0.10.40 --user root --password turing
tpi power on  -n 4 --host 10.0.10.40 --user root --password turing
# Wait ~90s for maintenance mode (node gets 10.0.10.x via DHCP)
talosctl apply-config --insecure --nodes <dhcp-ip> -f worker2.yaml
```

**Fix (Issue A):** `scripts/build-installer.sh` patches the kernel modules inside
the imager's `initramfs.xz` before building `CUSTOM_IMAGER_REF`.

Important: `initramfs.xz` is a Zstandard-compressed (not XZ despite the name) CPIO
containing only two entries: `/init` (the Talos init binary) and `/rootfs.sqsh` (a
squashfs of the full Talos rootfs). The kernel modules live **inside** `rootfs.sqsh`
at `lib/modules/<kver>/`, NOT as loose files in the CPIO.

The fix unpacks the CPIO, unsquashes `rootfs.sqsh`, replaces `lib/modules/<kver>/`
with our bldr-signed modules, resquashes, and repacks the CPIO. The imager's
`installer` command then runs depmod against our signed modules, producing a
`modules.dep` system extension squashfs that our kernel accepts. (Step: "Patching
kernel modules inside rootfs.sqsh in initramfs.xz")

Implementation note: all unsquash/resquash work runs INSIDE a container (not via
volume mount) to avoid macOS/colima filesystem permission errors with root-owned Talos
rootfs files (related to Bug 17). The approach: `docker run -d alpine:3.21 sleep 3600`,
`docker cp` files in, `docker exec` to do the work, `docker cp` result out.
`mksquashfs -comp xz` is used (not zstd) since Alpine's squashfs-tools ships with xz.

**Fix (Issue B):** `NEW_NODE` in `scripts/build-installer.sh` omits `assigned-clocks` and
`assigned-clock-rates`. These were derived from the vendor DTS but are not required for
rknpu.ko to probe and trigger SCMI clock setup independently.

**Status: VERIFIED FIXED** (2026-04-09) — after upgrade to NPU installer, both
`dwmac_rk` and `rknpu` load without rejection. `dmesg` confirms:
```
[drm] Initialized rknpu 0.9.8 for fdab0000.rknpu on minor 1
rk_gmac-dwmac fe1c0000.ethernet end0: Link is Up - 1Gbps/Full
```
Node gets DHCP, Talos API responds on 50000. `/modules.dep` extension carries
bldr-signed modules that the bldr-built kernel accepts.

---

## Bug 17: Colima virtiofs — Docker volume mounts only work with /private/tmp

**Symptom:** `docker run -v /var/folders/xxx:/out <image>` or `-v ~/some/path:/out` appears to
work (the container writes to `/out`), but the output files are invisible to the macOS host after
the container exits. `docker load -i ~/some/path/file.tar` fails with "no such file or directory".

**Root cause:** Colima mounts the macOS home directory into the Lima VM via virtiofs, but writes
from Docker containers go into the colima VM's overlay filesystem, NOT back to the macOS
filesystem. Only paths explicitly listed in `~/.colima/default/colima.yaml` under `mounts` are
properly bidirectionally synced. The macOS temp directory (`/var/folders/...`, used by `mktemp
-d`) is NOT mounted by default.

**Solution:**
1. Add `/private/tmp` to colima's mounts in `~/.colima/default/colima.yaml`:
   ```yaml
   mounts:
     - location: /private/tmp
       writable: true
   ```
2. Restart colima: `colima stop && colima start`
3. Use `/private/tmp/...` as the base for all build work directories instead of `mktemp -d`.

In `scripts/build-installer.sh`, `setup_pkgs_tree()` now uses:
```bash
PKGS_WORK_DIR="/private/tmp/talos-build-work-$$"
mkdir -p "${PKGS_WORK_DIR}"
```

**Note:** `scripts/build-extensions.sh` uses the buildx layer cache (no output to host), so it
is not affected. Only scripts that use `-v <host-path>:/out` volume mounts are affected.

---

## Bug 18: `docker buildx rm` does NOT clear the BuildKit local layer cache — rknpu.ko uses stale signing key

**Symptom:** After running `docker buildx rm <builder> && docker buildx create --name <builder>`,
the new builder's `docker buildx build --target rockchip-rknpu --cache-from build-cache/kernel`
still shows ALL stages CACHED (including `rockchip-rknpu:build-0`). The rknpu.ko is signed with
the old key (K_old), not the fresh K3 key that was just built into `build-cache/kernel`. The node
continues to get `Loading of module with unavailable key is rejected` after upgrade.

**Root cause:** `docker buildx rm` removes the builder *container* but does **not** delete the
named Docker volume that holds the BuildKit layer store:
```
buildx_buildkit_<builder-name>0_state   (15–20 GB)
```
When `docker buildx create --name <same-name>` is called, Docker creates a new builder container
that **reuses this existing volume** (because the volume name is deterministic from the builder
name). The new builder inherits all cached layers from the old builder, including K_old
`rockchip-rknpu` layers. When `--cache-from build-cache/kernel` is used for the rknpu build,
BuildKit finds K_new kernel-build layers in the registry cache, but then finds K_old rknpu
compilation layers in the **local** BuildKit cache (which takes priority) — even though the
cache key should differ (K_new parent ≠ K_old parent).

**Why local cache beats registry cache-from:** BuildKit evaluates layer cache keys from the
current build inputs. If the local cache was populated by a prior build that used K_old
kernel-build → K_old rknpu, and the current build with `--cache-from build-cache/kernel` resolves
kernel-build to K_old (because build-cache/kernel still contains K_old — see root cause below),
then ALL rknpu layers are a local cache hit.

**The deeper root cause — kernel image vs. cache divergence:** The `--no-cache` kernel build
writes K3 to `build-cache/kernel` (registry) but the **kernel image**
(`talos-rk3588-kernel:6.18.18-talos`) may not be updated if the `--push` fails silently when
`--cache-to mode=max` exhausts the Docker data disk. With `disk: 60 GiB` in colima.yaml and a
15 GiB pre-existing builder volume + 17 GiB Docker images, a fresh `--no-cache` kernel build
accumulates ~20 GiB in the new builder volume, then `--cache-to mode=max` tries to export ~20 GiB
to the registry — exceeding the free space and aborting with
`ResourceExhausted: mkdir .../ingest/...: no space left on device`.
When this happens, `--push` may also be aborted, leaving the kernel image at K_old.

**Full failure chain:**
1. `docker buildx rm old-builder` → removes container, keeps volume (K_old data)
2. `docker buildx create --name same-name` → new container, REUSES volume (K_old still inside)
3. `docker buildx build --no-cache --target kernel --cache-to build-cache/kernel,mode=max` →
   builds K3 kernel, exports cache, **fails** with no-space → kernel IMAGE push may also fail
4. `docker buildx build --target rockchip-rknpu --cache-from build-cache/kernel` →
   kernel-build: registry K3 cache HIT (if export completed) → BUT local volume K_old wins →
   rknpu compiled with K_old → K_old rknpu.ko pushed
5. Installer uses K_old/K3 kernel image + K3/K_old rknpu.ko → MISMATCH → "key rejected"

**Solution:** Three-step fix:

1. **Explicitly delete the builder volume** before recreating:
   ```bash
   docker buildx rm talos-rk3588-builder 2>/dev/null || true
   docker volume rm buildx_buildkit_talos-rk3588-builder0_state 2>/dev/null || true
   ```

2. **NO `--cache-to` on kernel build** to avoid disk exhaustion. The local BuildKit cache
   (inside the new builder volume) is the only cache needed for the shared-key guarantee:
   ```bash
   docker buildx build --no-cache --target kernel --push --tag kernel:version \
       # NO --cache-to: avoids 20+ GiB registry export that exhausts 60 GiB colima disk
   ```

3. **Build rknpu WITHOUT `--cache-from` and WITHOUT `--no-cache`**, relying solely on the local
   BuildKit cache from step 2.

   **⚠️ THIS STEP-3 APPROACH DOES NOT WORK — see Bug 19.** BuildKit's `--no-cache` in step 2
   stores results with random cache IDs that step 3 cannot find via content-addressed lookup.
   The kernel-build stage in step 3 runs fresh (generates a new key K6 ≠ K5), resulting in
   another mismatch. Use `docker buildx bake` instead (Bug 19).

**Implemented in:** `/private/tmp/rebuild-correct.sh` (BROKEN — use rebuild-bake.sh instead).

---

## Bug 19: `--no-cache` builds store results with random cache IDs — subsequent builds cannot reuse them

**Symptom:** After rebuilding the kernel with `--no-cache` (step 2 of the Bug 18 fix), the
subsequent rknpu build (step 3, without `--no-cache`) runs `kernel-build:build-0` for **~22
minutes** instead of getting a local cache HIT. The rknpu.ko ends up signed with a DIFFERENT key
than the kernel image. Bug 16 (no Ethernet) persists after upgrade.

Confirmed in the build log:
```
#40 kernel-build:build-0
#40 DONE 1329.6s     ← 22 minutes: definitely NOT a cache hit
```

**Root cause:** BuildKit's `--no-cache` flag forces re-execution of all steps. The results **are**
written to the local BuildKit content store, but with **random cache IDs** (not the normal
content-addressed hash of instruction + parent). A subsequent build (without `--no-cache`) computes
normal content-addressed cache keys to look up results, which do **not** match the random IDs stored
by the `--no-cache` run. The result: every step is a cache miss, a fresh kernel-build runs, and a
new ephemeral signing key K_new2 ≠ K5 is generated.

Additional symptom in the step-3 log:
```
#14 kernel-prepare:cksum-verify
#14 ERROR: failed to load ref: m8yxmcz6gly34s6ghos3nb8m7: not found
```
BuildKit's cache metadata from the `--no-cache` run references content blobs that were stored under
random IDs; subsequent normal builds cannot resolve those refs → cache miss → fresh kernel-build.

**FAILED attempt — `docker buildx bake --no-cache`:** Both bake targets get their own independent
kernel-build execution. Confirmed in bake2 log:
```
#54 [rockchip-rknpu] kernel-build:build-0   ← rockchip-rknpu's OWN kernel-build
#55 [kernel] kernel-build:build-0            ← kernel's OWN kernel-build
```
Two runs → two signing keys → mismatch. The bake DAG deduplication only applies within a single
target's graph; cross-target sharing does not occur when `--no-cache` is used.

**FAILED attempt — `--cache-to type=local,mode=max`:** With a fresh builder, build kernel with
`--no-cache --cache-to type=local,dest=<dir>,mode=max`. The local cache export took 503.6s then
failed with `no space left on device` on the Docker VM disk (filled by intermediate kernel compile
artifacts). The exported cache had only 1 layer (64MB) — insufficient for a kernel-build cache hit.

**Correct solution:** Build kernel and rknpu in TWO sequential `docker buildx build` calls on the
**SAME fresh builder**, both WITHOUT `--no-cache`:

1. Delete builder AND its volume (`buildx_buildkit_<name>0_state`) — ensures empty local cache
2. Create fresh builder — forced full rebuild on next build (no old cache)
3. Build kernel **without `--no-cache`** — empty cache forces fresh build, stores with
   **content-addressed IDs** (not random)
4. Build rknpu **without `--no-cache`** — kernel-build stage computes the same content-addressed
   keys → **LOCAL CACHE HIT** → uses K_fresh → rknpu.ko signed with K_fresh

No `--cache-to` or `--cache-from` needed. No disk space overhead. The builder's local content store
(in the Docker VM) holds the shared kernel-build layers between the two sequential invocations.

```bash
# Step 0: delete builder + volume (ensures no stale K_old)
docker buildx rm talos-rk3588-builder 2>/dev/null || true
docker volume rm buildx_buildkit_talos-rk3588-builder0_state 2>/dev/null || true
docker buildx create --name talos-rk3588-builder --driver docker-container --use

# Step 1: build kernel (no --no-cache, content-addressed IDs, fresh build forced by empty cache)
docker buildx build --builder talos-rk3588-builder \
    --file pkgs/Pkgfile --target kernel --platform linux/arm64 \
    --build-arg TAG=6.18.18-talos --build-arg PKGS=a92bed5 \
    --tag ghcr.io/schwankner/talos-rk3588-kernel:6.18.18-talos --push pkgs/

# Step 2: build rknpu (same builder, kernel-build = LOCAL CACHE HIT → K_fresh)
docker buildx build --builder talos-rk3588-builder \
    --file pkgs/Pkgfile --target rockchip-rknpu --platform linux/arm64 \
    --build-arg TAG=6.18.18-talos --build-arg PKGS=a92bed5 \
    --tag ghcr.io/schwankner/rockchip-rknpu:0.9.8-6.18.18-talos --push pkgs/
```

**Implemented in:** `/private/tmp/rebuild-shared-cache.sh`.

---

## Bug 20: `build-installer.sh` uses stale Docker-cached kernel/rknpu images — installer has mismatched keys

**Symptom:** After a correct `docker buildx bake --push` that pushes K_fresh kernel + K_fresh rknpu
to GHCR, `build-installer.sh` still produces an installer with MISMATCHED keys (K_old kernel +
K_old rknpu). Bug 16 (no Ethernet) persists after upgrade with the new installer.

**Root cause:** `build-installer.sh` uses `docker create` and `docker run` to extract vmlinuz from
the kernel image and run the imager with extension images. Docker resolves image references against
the LOCAL DAEMON CACHE first. If an image with the same tag was previously pulled (e.g., during an
earlier failed K5+K6 build), Docker reuses the stale cached image instead of pulling the fresh one
from GHCR — even if the GHCR tag has been updated with a new digest.

Confirmed: `docker images ghcr.io/schwankner/talos-rk3588-kernel:6.18.18-talos` showed an image
"10 hours ago" while the K_fresh bake had just completed. `docker pull` showed "Downloaded newer
image" — confirming the local cache was stale.

**Solution:** Explicitly pull kernel and rknpu images before running `build-installer.sh`:
```bash
docker pull ghcr.io/schwankner/talos-rk3588-kernel:6.18.18-talos
docker pull ghcr.io/schwankner/rockchip-rknpu:0.9.8-6.18.18-talos
REGISTRY=ghcr.io/schwankner CONTAINER_RUNTIME=docker ./scripts/build-installer.sh
```

**Long-term fix:** Add explicit `docker pull` / `podman pull` calls at the start of
`build-installer.sh` for the kernel and extension images. The imager runs inside a container and
pulls extension images itself (from the registry, bypassing local cache), but the vmlinuz
extraction (`docker create / cp`) bypasses the registry pull.

---

## Bug 21: `--cache-to type=local,mode=max` exhausts Docker VM disk during kernel build

**Symptom:** `docker buildx build --no-cache --cache-to type=local,dest=<dir>,mode=max` for the
kernel target exits with:
```
ERROR: failed to solve: ResourceExhausted: write /var/lib/buildkit/runc-overlayfs/content/ingest/...: no space left on device
```
The local cache directory on the Mac contains only 1 layer (64MB) — the final kernel image layer —
instead of all intermediate build stages. The rknpu build cannot get a kernel-build cache hit from
this incomplete cache.

**Root cause:** Even with `type=local`, the BuildKit daemon writes ALL cache layers to its own
internal content store (inside the Docker VM disk at `/var/lib/buildkit/runc-overlayfs/content/`)
before streaming them to the Mac client. The kernel build generates ~20-25GB of intermediate
artifacts (`.o` files, overlay FS diffs), which fill the 60GB Docker VM disk. The cache export
then fails because there is no room left in the daemon's ingest staging area.

`type=local` does NOT bypass the Docker VM disk — it merely delivers the final exported bytes to
the Mac, but the staging still happens on the VM disk.

**Solution:** Do not use `--cache-to` at all. Build kernel and rknpu in two sequential calls on
the SAME builder WITHOUT `--no-cache` (see Bug 19 for details). The builder's local content store
already holds the shared stages; no explicit export is needed. Before starting, free VM disk space
by pruning old images and removing the previous builder volume:

```bash
docker buildx rm talos-rk3588-builder 2>/dev/null || true
docker volume rm buildx_buildkit_talos-rk3588-builder0_state 2>/dev/null || true
docker image prune -af
docker buildx prune -af
```

This reclaims 15-35GB, leaving sufficient headroom for the kernel compile.

---

## Bug 22: w568w/rknpu-module creates a DRM device, not /dev/rknpu

**Symptom:** `rknpu.ko` loads successfully (`dmesg` shows "Initialized rknpu 0.9.8 for
fdab0000.rknpu on minor 1"), and `lsmod` shows `rknpu` Live, but `/dev/rknpu` does not exist.
`/sys/class/misc/rknpu` is absent.

**Root cause:** The `w568w/rknpu-module` port registers the NPU as a DRM device (using the DRM
subsystem) rather than as a misc character device. The driver calls `drm_dev_register()`, which
creates device nodes under `/dev/dri/`. The actual device nodes are:

```
/dev/dri/card1       — DRM master node
/dev/dri/renderD129  — DRM render node (the one to mount into NPU inference containers)
```

The exact minor numbers depend on what other DRM devices are registered first. On Turing RK1
running headless (no display), the NPU is the first DRM device and gets `card0`/`renderD128`.
This numbering is stable across reboots as long as the DT and module load order do not change,
but should not be hardcoded.

Note: the platform driver name as registered by w568w/rknpu-module is `RKNPU` (uppercase). The
sysfs path therefore uses uppercase:
```bash
ls /sys/bus/platform/drivers/RKNPU/fdab0000.rknpu/drm/
# → card0  renderD128   (headless Turing RK1)
```

**Affected items:**
- `rockchip-rknpu` extension udev rule (Bug 3): the rule for `/dev/rknpu` never fires because
  the misc device is never created. The DRM device has its own udev rule (built into udev).
- CDI device plugin: must expose `/dev/dri/renderD128` (and `/dev/dri/card0`), not `/dev/rknpu`.
- RKNN Toolkit / librknnrt.so: verifies the device by reading the rknpu version via DRM ioctl on
  the render node, not via a misc device open. The library works with the DRM render node.

**Solution:** Update CDI device spec and device plugin to dynamically discover the render node
from sysfs at runtime. The device plugin scans
`/sys/bus/platform/drivers/RKNPU/fdab0000.rknpu/drm/` for entries starting with `renderD` and
`card`, then constructs `/dev/dri/<name>`. This handles any minor-number assignment.

---

## Bug 23: hostUsers: false fails — user namespaces disabled (max_user_namespaces=0)

**Symptom:** Pod with `spec.hostUsers: false` stuck in `ContainerCreating`:
```
failed to start noop process for unshare: fork/exec /proc/self/exe: no space left on device
```

**Root cause:** `/proc/sys/user/max_user_namespaces` is 0 on the Talos node. User namespaces are
needed for `hostUsers: false`, which is in turn needed for `procMount: Unmasked` to work (making
`/proc/device-tree/compatible` readable without `privileged: true`). The "no space left on
device" error is the kernel's way of refusing a `clone(CLONE_NEWUSER)` call when the limit is 0.

**Fix:** Add to the Talos machine config:
```yaml
machine:
  sysctls:
    user.max_user_namespaces: "63359"
```

Apply with `talosctl patch mc` and reboot. The value 63359 matches the default in most
distributions that enable user namespaces.

**Verification:**
```bash
talosctl read /proc/sys/user/max_user_namespaces
# → 63359
```

Then a pod with `hostUsers: false` and `securityContext.procMount: Unmasked` starts normally and
`/proc/device-tree/compatible` is readable inside the container.

---

## Bug 24: CONFIG_DMABUF_HEAPS not set — librknnrt.so init_runtime fails

**Symptom:** `rknn.init_runtime()` returns -1 (`RKNN_ERR_FAIL`).  The error sequence is:
```
E RKNN: failed to open rknpu module, need to insmod rknpu dirver!
E RKNN: failed to open rknn device!
```
followed by:
```
Exception: RKNN init failed. error code: RKNN_ERR_FAIL
```

**Root cause:** `strings /usr/lib/librknnrt.so` reveals the library probes three device paths:
```
/dev/rknpu          ← BSP misc device (w568w DRM driver does not create this)
/dev/dri/renderD*   ← DRM render node — found at renderD128 ✓
/dev/dma_heap       ← DMA-BUF heap for zero-copy CPU↔NPU buffers — MISSING ✗
```

`/dev/dma_heap/system` does not exist because the Talos 1.12.x kernel is built with
`CONFIG_DMABUF_HEAPS=n`. librknnrt.so uses DMA-BUF heaps to allocate shared buffers
between the ARM CPU and the NPU. Without the heap device, `init_runtime` fails.

**Verification:**
```bash
talosctl read /proc/config.gz | zcat | grep CONFIG_DMABUF_HEAPS
# → CONFIG_DMABUF_HEAPS is not set
ls /dev/dma_heap/
# → lstat /dev/dma_heap: no such file or directory
```

**Fix:** Rebuild the kernel with:
```
CONFIG_DMABUF_HEAPS=y
CONFIG_DMABUF_HEAPS_SYSTEM=y
```
This creates `/dev/dma_heap/system` on boot, which librknnrt.so requires for NPU buffer
allocation.  The kernel build must be customised with a config overlay rather than relying
on the upstream Siderolabs pkgs kernel, which does not enable this option for ARM64.

After enabling, `/dev/dma_heap/system` should also be added to the CDI spec so it is
injected into NPU pods alongside the DRM render node.

## Bug 25: init_runtime() causes hard system hang — NPU genpd SCMI crash at EL3

**Symptom:** Calling `rknn.init_runtime()` from any Kubernetes pod (NPU or CPU mode)
causes the entire node to hang hard.  No kernel panic, `panic=30` never fires, system
is not pingable, requires BMC/power cycle.  `load_rknn()` completes successfully; the
crash is specific to `init_runtime()`.

**Root cause:** `rknpu_power_on()` in `rknpu_drv.c` calls
`pm_runtime_resume_and_get(genpd_dev_npu0/1/2)` at ioctl time.  These virtual genpd
devices represent the three NPU power domains (`npu0`, `npu1`, `npu2`).  At probe time
the power domains are already on (ATF keeps them on during boot), so `pm_runtime_resume_and_get`
is a no-op.  After probe, `rknpu_power_off()` calls `pm_runtime_put_sync(genpd_dev_npu*)`,
which drops the reference count to zero and suspends the genpd devices — triggering a
SCMI power-domain-off call into ATF, which succeeds.

On the **first ioctl** from userspace (`DRM_IOCTL_RKNPU_ACTION` / `RKNPU_ACTION_GET_HW_VERSION`),
`rknpu_power_on()` tries to re-enable the power domains via
`pm_runtime_resume_and_get(genpd_dev_npu0)`.  This calls the Rockchip PM-domain
`power_on()` callback, which makes a SCMI power-domain-on SMC call into ATF.  The ATF
on mainline RK3588 (Turing RK1 / Talos 1.12 / kernel 6.18) **crashes at EL3** when
this SCMI call is made at runtime — unlike during boot, where the NPU domain is already
powered and the call is a no-op.

**Investigation path:**
- `iommu.passthrough=1` added → ARM SMMU-v3 in identity/passthrough mode for NPU
  groups 7/8/9 → confirmed NOT the cause.
- `echo on > /sys/bus/platform/devices/fdab0000.rknpu/power/control` (forces DRM
  device always-on) → crash persists.  This only prevents the DRM device from
  suspending, not the genpd virtual devices.
- `fdab0000.npu` (noop device) has `iommu_group/type = identity` → ARM SMMU confirmed
  passthrough → not the cause.
- v8-loadonly test (`sys.exit()` before `init_runtime()`) → no crash → proves crash
  is entirely inside `init_runtime()`.
- Tracing `rknpu_power_on()` source: detects `multiple_domains = true` for RK3588
  (three NPU power domains npu0/npu1/npu2); calls `pm_runtime_resume_and_get()` on
  each genpd virtual device at every power-on; these were suspended by
  `rknpu_power_off()` at end of probe.
- ATF crash signature: hard hang at EL3 (not EL1), no kernel panic, panic=30 never
  fires, requires hardware reset.

**Fix:** In `rknpu_drv.c`, call `pm_runtime_get_noresume(virt_dev)` immediately after
`dev_pm_domain_attach_by_name()` for each NPU power domain.  This keeps the genpd
usage count ≥1 permanently, so `pm_runtime_put_sync()` in `rknpu_power_off()` never
suspends them.  The SCMI power-domain-on call at ioctl time is never made.  NPU power
domains remain on for the lifetime of the loaded module.

Applied in `rockchip-rknpu/pkg.yaml` as a Python source patch during extension build.

---

## Bug 26: build_graph() causes hard system hang — NPU device SCMI crash at EL3

**Symptom:** After the Bug 25 fix (genpd devices always-on), `init_runtime()` progresses
past `load_rknn()` and the DRM device open, but hangs the node when `build_graph()` is
called (RKNN step: sending model to NPU, loading firmware).  No kernel panic, requires
BMC reset.

**Root cause:** `rknpu_power_on()` in `rknpu_drv.c` has two layers of PM calls:

1. `pm_runtime_resume_and_get(genpd_dev_npu0/1/2)` — fixed by Bug 25
2. `pm_runtime_get_sync(dev)` — on the **NPU device itself** (`fdab0000.rknpu`), line 1024

Bug 25 only addressed the genpd virtual devices.  After probe, `rknpu_power_off_delay_work`
fires after 3 seconds of idle and calls `pm_runtime_put_sync(dev)`, dropping the NPU
device's own usage count to zero and suspending it.  On the next `rknpu_power_on()` call
(at `build_graph()`), `pm_runtime_get_sync(dev)` finds the device suspended and calls
the NPU's `runtime_resume` callback, which issues a SCMI power-domain-on SMC → EL3 crash
(same mechanism as Bug 25).

**Fix:** Call `pm_runtime_get_noresume(dev)` immediately after `pm_runtime_enable(dev)`
in the probe function.  This keeps the NPU device's own usage count ≥1 permanently,
so `pm_runtime_put_sync(dev)` in `rknpu_power_off()` never actually suspends the device.

**Note:** Bug 26 alone was not sufficient — a third SCMI path via `clk_bulk_disable_unprepare`
remained (see Bug 27).  All three fixes together are required to prevent EL3 crashes.

Applied in `rockchip-rknpu/pkg.yaml` as a Python source patch during extension build.

---

## Bug 31: node crashes at first ioctl despite Bug 30 — regulator SCMI at runtime

**Symptom:** After Bug 30, node still crashes within ~5 seconds of the bench pod starting.
The crash happens before any Python output, indicating it occurs at the first ioctl.

**Root cause:** Probe calls `rknpu_power_on()` (enables clocks, regulators) then at
line 1537 calls `rknpu_power_off()` directly.  Bug 30 left `clk_bulk_disable_unprepare()`
and `regulator_disable()` in `rknpu_power_off()`.  Bug 27's extra probe
`clk_bulk_prepare_enable()` keeps clock refcount at 2 after probe's power_on, so probe's
power_off decrements from 2→1 (no SCMI clock-disable).  BUT: regulators go from 1→0 in
probe's power_off.  The first ioctl calls `rknpu_power_on()` which calls
`regulator_enable()` with regulators at refcount 0 → actual SCMI enable SMC → hard hang.

**Solution:** Make `rknpu_power_off()` a complete no-op (return 0 immediately).  Probe
leaves clocks at refcount 2 (Bug 27 anchor) and regulators at refcount 1.  Every
subsequent runtime `rknpu_power_on()` call increments refcounts without hitting the
0→1 boundary → no SCMI ever sent at runtime.

Applied in `rockchip-rknpu/pkg.yaml` as a Python source patch (after Bug 30 patch).

---

## Bug 30: node crashes at build_graph() despite Bugs 25–29 — pm_runtime SCMI at runtime

**Symptom:** After applying all of Bugs 25–29, the node still crashes within ~5 seconds of
the bench pod starting.  `rknpu_power_on()` is called from the first ioctl (`build_graph`),
which calls `pm_runtime_resume_and_get()` on each of the three genpd virtual devices (npu0,
npu1, npu2) and `pm_runtime_get_sync()` on the main NPU device.  Despite Bug 25 keeping
genpd device usage counts ≥1 (status=RPM_ACTIVE after probe) and Bug 26 keeping the main
device usage count ≥1, these calls still issue SCMI power-domain SMCs into ATF at EL3.

**Root cause:** Even with RPM_ACTIVE status, `pm_runtime_resume_and_get()` and
`pm_runtime_get_sync()` on genpd-backed devices can trigger the rockchip power-domain
resume path under certain race conditions (e.g. `sync_state()` interactions with the power
domain controller).  The root cause is that ANY SCMI call from the rknpu driver at runtime
into the Turing RK1 ATF causes a hard hang — the ATF firmware does not handle runtime SCMI
power-domain transitions from the NPU driver after the initial probe window.

**Solution:** Remove ALL pm_runtime/genpd calls from `rknpu_power_on()` and
`rknpu_power_off()`.  The power domains are permanently anchored by:
- Bug 25: `pm_runtime_get_noresume(virt_dev)` per genpd device (count ≥1 forever)
- Bug 26: `pm_runtime_get_noresume(dev)` after `pm_runtime_enable` (count ≥1 forever)
- Probe's `rknpu_power_on()` calls still run the full PM path to initially power on hardware

At runtime, `rknpu_power_on/off` becomes clocks-only (+ regulators).  No pm_runtime ops,
no genpd resume/suspend, no SCMI.  The hardware stays powered on permanently.

Applied in `rockchip-rknpu/pkg.yaml` as a Python source patch.

---

## Bug 29: node crashes 30-45s after boot — devfreq initialization SCMI paths

**Symptom:** After applying Bugs 25–28, the node crashes spontaneously 30–45 seconds after
boot with NO bench activity running.  The step test pod was still in Pending/ContainerCreating
state when the node went unreachable.  This is a regression from earlier sessions where the
node only crashed when the NPU bench was actively running.

**Root cause:** Even with `npu_devfreq_target()` as a no-op (Bug 28), the
`rknpu_devfreq_init()` function performs OPP/devfreq framework setup that triggers SCMI
SMC calls independently of the governor callback:
- `devm_pm_opp_set_clkname(dev, "scmi_clk")` registers the SCMI clock with the OPP
  framework, which may query the current clock rate via `SCMI_CLOCK_RATE_GET`
- `devm_pm_opp_of_add_table(dev)` builds the OPP table and may call `clk_get_rate` on
  the registered SCMI clock
- `devm_devfreq_add_device(dev, ...)` starts the devfreq polling timer (50 ms interval)
  and may call `dev_pm_opp_set_rate` during initialization to set the initial OPP
- The devfreq framework's `resume_freq` path calls `dev_pm_opp_set_rate` independently
  when the device's runtime PM status changes

The NPU driver was crashing via one or more of these paths within a minute of boot,
completely independently of any NPU ioctl activity.

**Fix:** Replace `rknpu_devfreq_init()` body with `return 0`, skipping ALL devfreq/OPP
initialization.  `rknpu_dev->devfreq` remains NULL, so `devfreq_lock/unlock` in
`rknpu_power_on/off` become safe no-ops.  The NPU runs at its boot-time frequency
permanently.  SCMI-based DVFS is not viable on mainline RK3588 ATF.

Applied in `rockchip-rknpu/pkg.yaml` as a Python source patch to `src/rknpu_devfreq.c`
(replaces the Bug 28 partial fix — Bug 28's `npu_devfreq_target` no-op patch is superseded
by this more complete fix).

---

## Bug 28: build_graph() still hangs after Bug 27 fix — SCMI clock rate-set via devfreq

**Symptom:** After applying Bugs 25–27 (PM domains, NPU device PM, and clocks all pinned
always-on), `build_graph()` still hangs.  Node upgraded with the Bug 27 build, bench pod
deployed, node crashed again at the same STEP 5 point.

**Root cause:** A fourth SCMI path exists independently of `rknpu_power_on/off`.
The `rknpu_devfreq_init()` function registers a `rknpu_ondemand` devfreq governor which
calls `npu_devfreq_target()` to adjust NPU frequency based on load.  `npu_devfreq_target()`
calls `dev_pm_opp_set_rate(dev, *freq)` → `clk_set_rate(scmi_clk, freq)` → SCMI
clock-rate-set SMC into ATF at EL3 → hard hang.

This path fires from the devfreq framework's governor workqueue independently of any ioctl
or power-on/off cycle.  The `rknpu_power_on/off` PM fixes only address the power domain
and clock enable/disable paths; they do not affect `clk_set_rate` calls made by devfreq.

Even though `rknpu_devfreq_runtime_resume/suspend` are no-ops, the devfreq governor timer
fires immediately once the devfreq device is registered and can issue `clk_set_rate` at any
time.

**Fix (partial):** Replace `npu_devfreq_target()` body with a no-op `return 0` to prevent
explicit `dev_pm_opp_set_rate` calls from the governor.  However this proved insufficient —
see Bug 29 for the remaining crash paths within devfreq initialization.

Applied in `rockchip-rknpu/pkg.yaml` as a Python source patch to `src/rknpu_devfreq.c`.

---

## Bug 27: build_graph() still hangs after Bug 26 fix — SCMI clock path at EL3

**Symptom:** After applying both Bug 25 and Bug 26 fixes (all PM domains and the NPU device
itself pinned always-on), `build_graph()` still hangs the node.  Confirmed by deploying the
Bug 26 build and seeing the crash persist at the same STEP 5 point in bench output.

**Root cause:** A third SCMI path exists in `rknpu_power_on()` / `rknpu_power_off()`.
Bug 25 pinned the genpd virtual devices; Bug 26 pinned the NPU device's own runtime PM.
However `rknpu_power_off()` also calls `clk_bulk_disable_unprepare()` (line 1091 of
`rknpu_drv.c`), which disables all NPU clocks including `scmi_clk` — an SCMI-managed
clock.  The SCMI clock-disable issues an SMC into ATF at EL3.  On the next `build_graph()`
call, `rknpu_power_on()` calls `clk_bulk_prepare_enable()` to re-enable the SCMI clock —
another SMC into ATF → hard hang requiring BMC reset.

The `scmi_clk` clock is the first entry in the NPU's device-tree `clocks` list and is
managed via `devm_pm_opp_set_clkname(dev, "scmi_clk")` in `rknpu_devfreq.c`.  Every
enable/disable of this clock goes through the SCMI firmware interface and triggers a
synchronous SMC call into TF-A (Trusted Firmware-A) at EL3.  The ATF on mainline RK3588
boards (confirmed on Turing RK1) crashes at EL3 when these SCMI clock SMCs arrive at
runtime — same root cause as Bugs 25 and 26.

**Fix:** Call `clk_bulk_prepare_enable(rknpu_dev->num_clks, rknpu_dev->clks)` once extra
in probe, immediately after `rknpu_power_on()` returns.  This bumps the clock reference
count to 2.  The `clk_bulk_disable_unprepare()` in `rknpu_power_off()` then decrements
from 2 to 1 (clocks remain enabled — no SCMI SMC fired).  The next `rknpu_power_on()`
increments from 1 to 2 again.  Steady state: clock ref count oscillates between 1 and 2,
always ≥1, no SCMI clock-enable or clock-disable SMC is ever issued at runtime.

Combined with Bugs 25 and 26, all SCMI-triggering paths in `rknpu_drv.c` are now pinned:
- genpd virtual devices npu0/1/2: `pm_runtime_get_noresume(virt_dev)` after attach (Bug 25)
- NPU device `fdab0000.rknpu` itself: `pm_runtime_get_noresume(dev)` after `pm_runtime_enable` (Bug 26)
- NPU clocks including `scmi_clk`: extra `clk_bulk_prepare_enable` in probe (Bug 27)

**Note:** Bug 27 alone was not sufficient — the devfreq governor SCMI clock-rate-set path
remained (see Bug 28).  All four fixes together (Bugs 25–28) are required.

Applied in `rockchip-rknpu/pkg.yaml` as a Python source patch during extension build.

---

*Add new bugs above this line, most recent first.*
