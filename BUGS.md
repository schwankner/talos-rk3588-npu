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
Then run the custom imager (no `--base-installer-image` needed, or use default siderolabs installer-base). See `scripts/build-installer.sh` for the full implementation.

**Key insight:** The signing key is embedded in the kernel at compile time. Our bldr build at `PKGS_COMMIT=a92bed5` uses the same pre-committed signing key as the siderolabs build (the key in `certs/signing_key.pem` is committed to the pkgs source tree, making the build reproducible). This means our module signing key and the kernel's embedded key are the **same key** — the actual fix needed is ensuring the module is signed with `sha512` (matching `CONFIG_MODULE_SIG_SHA512=y`), not sha256.

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

**Root cause (hypothesis):** The two-pass NPU installer combines:
1. Our custom kernel + NPU extensions (rknpu, rknn-libs) in the UKI initramfs
2. The sbc-rockchip v0.2.0 overlay U-Boot/DTBs

When the combined installer runs on upgrade, it installs U-Boot with the sbc-rockchip DTB that
adds NPU nodes (`rockchip,rk3588-rknn-core` compatible, per Bug 7's rknn.patch). During the
subsequent maintenance mode boot, udevd triggers 3 `modprobe rknpu` attempts for the 3 NPU
cores (fdab0000, fdac0000, fdad0000). Each attempt fails with `Loading of module with unavailable
key is rejected` — our rknpu.ko is built and signed, but the signing key doesn't match the
running kernel's key (see Bug 10). This causes 3 NPU devices to remain unprobed, keeping their
PM domain in `sync_state() pending`. Whether this specifically blocks the Ethernet power domain
sync is unconfirmed, but the Ethernet never initializes.

**Evidence:** UART from NPU installer boot shows:
```
[ 8.160074] Loading of module with unavailable key is rejected
[ 8.257573] Loading of module with unavailable key is rejected
[ 8.307102] Loading of module with unavailable key is rejected
[16.116525] rockchip-pm-domain fd8d8000.power-management:power-controller: sync_state() pending due to fdab0000.npu
[16.128207] rockchip-pm-domain fd8d8000.power-management:power-controller: sync_state() pending due to fdac0000.npu
[16.139879] rockchip-pm-domain fd8d8000.power-management:power-controller: sync_state() pending due to fdad0000.npu
[16.237890] rockchip-pm-domain fd8d8000.power-management:power-controller: sync_state() pending due to fe1c0000.ethernet
```
No network events after this; Ethernet never probes.

**Recovery:** Use `tpi flash` to write the standard Talos metal image (same schematic as
`worker2.yaml`'s `install.image`), then re-apply the machine config:
```bash
tpi flash -n 4 --image-path /private/tmp/talos-turingrk1-v1.12.6.raw \
  --host 10.0.70.2 --user root --password turing
tpi power off -n 4 --host 10.0.70.2 --user root --password turing
tpi power on  -n 4 --host 10.0.70.2 --user root --password turing
# Wait ~90s for maintenance mode (node gets 10.0.70.x via DHCP)
talosctl apply-config --insecure --nodes 10.0.70.17 -f worker2.yaml
```

**Fix (pending):** Build the NPU installer WITHOUT the sbc-rockchip overlay step (Pass 2 and
combine step). Without the overlay, `talosctl upgrade` does NOT repartition the eMMC (Bug 13),
so STATE is preserved, the machine config is applied on boot, and VLAN 60 is configured before
NTP/DNS is needed. The existing U-Boot+DTBs (already on eMMC from the working Talos install)
are preserved as-is. The NPU DTB overlay will be added via the `rockchip-rknpu` extension
rather than relying on the sbc-rockchip rknn.patch (see Bug 7).

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

*Add new bugs above this line, most recent first.*
