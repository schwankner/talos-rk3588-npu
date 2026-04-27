package main

import (
	"context"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	v1beta1 "k8s.io/kubelet/pkg/apis/deviceplugin/v1beta1"
)

const (
	resourceName = "rockchip.com/npu"

	// npuCores is the number of virtual device slots advertised to Kubernetes.
	// The RK3588 NPU has 3 physical cores; all slots map to the same /dev/rknpu
	// device node. The RKNN runtime and kernel driver handle concurrent access
	// and distribute work across cores internally, so up to npuCores pods can
	// run inference simultaneously without blocking each other.
	npuCores = 3

	// rknpu.ko registers a misc device at /dev/rknpu when
	// CONFIG_ROCKCHIP_RKNPU_DMA_HEAP is set (our build config).
	// librknnrt.so v2.3.x opens this device and submits inference jobs via
	// BSP ioctl numbers (type='r'). There are no DRM render nodes in this mode.
	rknpuMiscDev = "/dev/rknpu"

	// CDI device reference — containerd looks up this name in the CDI spec
	// installed by the rockchip-rknpu system extension at /etc/cdi/rockchip-npu.yaml.
	// All virtual slots reference the same CDI entry because they share the
	// single /dev/rknpu device node.
	cdiDeviceRef = "rockchip.com/npu=0"

	kubeletSock = "/var/lib/kubelet/device-plugins/kubelet.sock"
	pluginSock  = "/var/lib/kubelet/device-plugins/rk3588-npu.sock"

	// cdiSpecDest is the path where the CDI spec is written at startup.
	// Talos containerd is configured with cdi_spec_dirs = ['/run/cdi'] only;
	// the system extension installs the spec at /etc/cdi/ (persistent) but
	// containerd does not watch that path, and /etc/cdi is an overlay dir
	// not directly accessible from the kubelet host-path volume namespace.
	// The spec content is embedded here so no host file access is required;
	// the plugin writes it to /var/run/cdi/ (= /run/cdi/) on every start.
	cdiSpecDest = "/var/run/cdi/rockchip-npu.yaml"

	// cdiSpec is the static CDI spec content, matching the file installed
	// by the rockchip-rknpu system extension at /etc/cdi/rockchip-npu.yaml.
	// Must be kept in sync with rockchip-rknpu/files/rockchip-npu.cdi.yaml.
	//
	// Both /dev/rknpu and /dev/dma_heap/system are declared as deviceNodes so
	// containerd adds them to the container device list, creates the nodes
	// (bind-mount for pre-existing devices), and updates the cgroupv2 device
	// eBPF allowlist automatically. librknnrt.so is a regular file and uses a
	// mount entry.
	cdiSpec = `cdiVersion: "0.6.0"
kind: "rockchip.com/npu"
devices:
  - name: "0"
    containerEdits:
      deviceNodes:
        - path: "/dev/rknpu"
          permissions: "rw"
        - path: "/dev/dma_heap/system"
          permissions: "rw"
      mounts:
        - hostPath: "/usr/lib/librknnrt.so"
          containerPath: "/usr/lib/librknnrt.so"
          options: ["ro", "bind"]
`

	pollInterval = 10 * time.Second
)

// ---------------------------------------------------------------------------
// CDI spec management
// ---------------------------------------------------------------------------

// writeCDISpec writes the embedded CDI spec to the containerd CDI watch
// directory (/var/run/cdi/).  Talos containerd is configured with
// cdi_spec_dirs = ['/run/cdi']; the system extension installs the spec at
// /etc/cdi/ (persistent), but /etc/cdi is an overlay dir not reachable
// via kubelet hostPath volumes, and /run/cdi is a tmpfs cleared on reboot.
// Writing the spec from the embedded constant on every start is the simplest
// approach that works without any machine config changes.
func writeCDISpec() error {
	if err := os.MkdirAll(filepath.Dir(cdiSpecDest), 0o755); err != nil {
		return fmt.Errorf("mkdir %s: %w", filepath.Dir(cdiSpecDest), err)
	}
	if err := os.WriteFile(cdiSpecDest, []byte(cdiSpec), 0o644); err != nil {
		return fmt.Errorf("write %s: %w", cdiSpecDest, err)
	}
	return nil
}

// ---------------------------------------------------------------------------
// NPU device discovery (DMA_HEAP mode)
// ---------------------------------------------------------------------------

// discoverNPU checks that the rknpu misc device exists. In DMA_HEAP build
// mode rknpu.ko calls misc_register() and creates /dev/rknpu; there are no
// DRM render or card nodes.
func discoverNPU() error {
	if _, err := os.Stat(rknpuMiscDev); err != nil {
		return fmt.Errorf("%s not found: %w — is rknpu.ko loaded?", rknpuMiscDev, err)
	}
	return nil
}

// ---------------------------------------------------------------------------
// Device plugin server
// ---------------------------------------------------------------------------

type npuPlugin struct {
	v1beta1.UnimplementedDevicePluginServer
	grpcServer  *grpc.Server
	serverError chan error // receives fatal gRPC server errors
}

func (p *npuPlugin) GetDevicePluginOptions(_ context.Context, _ *v1beta1.Empty) (*v1beta1.DevicePluginOptions, error) {
	return &v1beta1.DevicePluginOptions{}, nil
}

// unhealthyThreshold is the number of consecutive poll failures required
// before ListAndWatch reports the device as Unhealthy. A single transient
// glitch must not cause kubelet to kill running pods. At pollInterval=10s
// this gives a 30-second grace window before health transitions to Unhealthy.
const unhealthyThreshold = 3

// npuDevices builds the list of virtual device slots. All npuCores slots map
// to the same physical /dev/rknpu node and share the same health status.
func npuDevices(health string) []*v1beta1.Device {
	devs := make([]*v1beta1.Device, npuCores)
	for i := range devs {
		devs[i] = &v1beta1.Device{
			ID:     fmt.Sprintf("rknpu%d", i),
			Health: health,
		}
	}
	return devs
}

// ListAndWatch polls for /dev/rknpu on every tick so health status reflects
// module load/unload without requiring a pod restart. It advertises npuCores
// virtual slots so Kubernetes can schedule up to npuCores pods concurrently;
// the RKNN runtime distributes work across the 3 NPU cores internally.
func (p *npuPlugin) ListAndWatch(_ *v1beta1.Empty, stream v1beta1.DevicePlugin_ListAndWatchServer) error {
	log.Printf("ListAndWatch: starting (%d virtual slots)", npuCores)
	var failCount int

	for {
		err := discoverNPU()
		if err != nil {
			failCount++
			log.Printf("ListAndWatch: device unavailable (%d/%d): %v", failCount, unhealthyThreshold, err)
		} else {
			if failCount > 0 {
				log.Printf("ListAndWatch: device recovered")
			}
			failCount = 0
		}

		health := v1beta1.Healthy
		if failCount >= unhealthyThreshold {
			health = v1beta1.Unhealthy
		}

		if err := stream.Send(&v1beta1.ListAndWatchResponse{
			Devices: npuDevices(health),
		}); err != nil {
			log.Printf("ListAndWatch: send error: %v", err)
			return err
		}

		time.Sleep(pollInterval)
	}
}

func (p *npuPlugin) Allocate(_ context.Context, req *v1beta1.AllocateRequest) (*v1beta1.AllocateResponse, error) {
	if err := discoverNPU(); err != nil {
		return nil, fmt.Errorf("allocate: NPU not available: %w", err)
	}

	resp := &v1beta1.AllocateResponse{}
	for range req.ContainerRequests {
		// Pure CDI allocation: containerd resolves "rockchip.com/npu=0" against
		// /run/cdi/rockchip-npu.yaml and injects /dev/rknpu, /dev/dma_heap/system
		// (both as deviceNodes → cgroup allowlist + bind-mount), and
		// /usr/lib/librknnrt.so (mount). No redundant DeviceSpec needed.
		resp.ContainerResponses = append(resp.ContainerResponses,
			&v1beta1.ContainerAllocateResponse{
				CDIDevices: []*v1beta1.CDIDevice{{Name: cdiDeviceRef}},
			},
		)
	}
	return resp, nil
}

func (p *npuPlugin) PreStartContainer(_ context.Context, _ *v1beta1.PreStartContainerRequest) (*v1beta1.PreStartContainerResponse, error) {
	return &v1beta1.PreStartContainerResponse{}, nil
}

// ---------------------------------------------------------------------------
// gRPC server lifecycle
// ---------------------------------------------------------------------------

func (p *npuPlugin) start() error {
	if err := os.Remove(pluginSock); err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("remove stale socket: %w", err)
	}

	lis, err := net.Listen("unix", pluginSock)
	if err != nil {
		return fmt.Errorf("listen %s: %w", pluginSock, err)
	}

	p.grpcServer = grpc.NewServer()
	p.serverError = make(chan error, 1)
	v1beta1.RegisterDevicePluginServer(p.grpcServer, p)

	go func() {
		if err := p.grpcServer.Serve(lis); err != nil {
			log.Printf("gRPC server error: %v", err)
			p.serverError <- err
		}
	}()

	log.Printf("Plugin server listening on %s", pluginSock)
	return nil
}

func (p *npuPlugin) register() error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	conn, err := grpc.NewClient(
		"unix://"+kubeletSock,
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		return fmt.Errorf("dial kubelet: %w", err)
	}
	defer conn.Close()

	client := v1beta1.NewRegistrationClient(conn)
	if _, err := client.Register(ctx, &v1beta1.RegisterRequest{
		Version:      v1beta1.Version,
		Endpoint:     filepath.Base(pluginSock),
		ResourceName: resourceName,
	}); err != nil {
		return fmt.Errorf("register: %w", err)
	}

	log.Printf("Registered %s with kubelet", resourceName)
	return nil
}

func (p *npuPlugin) stop() {
	if p.grpcServer != nil {
		p.grpcServer.Stop()
	}
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

func main() {
	log.SetPrefix("[rk3588-npu-device-plugin] ")
	log.Println("Starting")

	// Wait up to 60 s for rknpu.ko to load and register /dev/rknpu.
	// This handles the case where the DaemonSet pod starts before udevd
	// has finished processing the module load event.
	var err error
	for i := 0; i < 12; i++ {
		err = discoverNPU()
		if err == nil {
			break
		}
		log.Printf("Waiting for /dev/rknpu (%d/12): %v", i+1, err)
		time.Sleep(5 * time.Second)
	}
	if err != nil {
		log.Fatalf("/dev/rknpu not found after 60 s: %v", err)
	}
	log.Printf("Discovered NPU: %s — advertising %d virtual slots", rknpuMiscDev, npuCores)

	// Copy CDI spec from /etc/cdi/ (extension-installed, persistent) to
	// /var/run/cdi/ (containerd watch dir, tmpfs cleared on reboot).
	if err := writeCDISpec(); err != nil {
		log.Fatalf("Failed to write CDI spec: %v", err)
	}
	log.Printf("CDI spec written to %s", cdiSpecDest)

	plugin := &npuPlugin{}

	if err := plugin.start(); err != nil {
		log.Fatalf("Failed to start plugin server: %v", err)
	}

	if err := plugin.register(); err != nil {
		log.Fatalf("Failed to register with kubelet: %v", err)
	}

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	select {
	case <-sig:
		log.Println("Shutting down")
	case err := <-plugin.serverError:
		log.Fatalf("gRPC server terminated unexpectedly: %v", err)
	}
	plugin.stop()
}
