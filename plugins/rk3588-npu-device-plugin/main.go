package main

import (
	"context"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"gopkg.in/yaml.v3"
	v1beta1 "k8s.io/kubelet/pkg/apis/deviceplugin/v1beta1"
)

const (
	resourceName = "rockchip.com/npu"
	deviceID     = "rknpu0"

	// The w568w/rknpu-module driver registers the NPU as a DRM device under this
	// sysfs path. All RK3588 variants share the same base address (fdab0000).
	// The DRM subsystem populates subdirectories named "renderD<N>" and "card<N>"
	// here after the driver binds; we discover them at runtime so the plugin is
	// not sensitive to minor-number assignment order.
	// Note: the platform driver name is "RKNPU" (uppercase) as registered by
	// w568w/rknpu-module.
	npuSysFSBase = "/sys/bus/platform/drivers/RKNPU/fdab0000.rknpu/drm"

	// librknnrt.so is installed by the rockchip-rknn-libs Talos extension.
	librknnrt = "/usr/lib/librknnrt.so"

	cdiKind      = "rockchip.com/npu"
	cdiDeviceRef = "rockchip.com/npu=0"
	cdiSpecDir   = "/var/run/cdi"
	cdiSpecFile  = "/var/run/cdi/rockchip-npu.yaml"

	kubeletSock = "/var/lib/kubelet/device-plugins/kubelet.sock"
	pluginSock  = "/var/lib/kubelet/device-plugins/rk3588-npu.sock"

	pollInterval = 10 * time.Second
)

// ---------------------------------------------------------------------------
// CDI spec structures (CDI spec v0.6.0)
// ---------------------------------------------------------------------------

type cdiSpec struct {
	CDIVersion string      `yaml:"cdiVersion"`
	Kind       string      `yaml:"kind"`
	Devices    []cdiDevice `yaml:"devices"`
}

type cdiDevice struct {
	Name           string        `yaml:"name"`
	ContainerEdits containerEdit `yaml:"containerEdits"`
}

type containerEdit struct {
	DeviceNodes []deviceNode `yaml:"deviceNodes,omitempty"`
	Mounts      []mount      `yaml:"mounts,omitempty"`
}

type deviceNode struct {
	Path        string `yaml:"path"`
	Permissions string `yaml:"permissions,omitempty"`
}

type mount struct {
	HostPath      string   `yaml:"hostPath"`
	ContainerPath string   `yaml:"containerPath"`
	Options       []string `yaml:"options,omitempty"`
}

// ---------------------------------------------------------------------------
// DRM node discovery
// ---------------------------------------------------------------------------

// npuNodes holds the discovered DRM device nodes for the NPU.
type npuNodes struct {
	renderNode string // e.g. /dev/dri/renderD129 — used by inference workloads
	cardNode   string // e.g. /dev/dri/card1      — DRM master node
}

// healthy reports whether the primary render node exists and is accessible.
func (n *npuNodes) healthy() bool {
	if n.renderNode == "" {
		return false
	}
	_, err := os.Stat(n.renderNode)
	return err == nil
}

// discoverNPUNodes reads the rknpu sysfs entry to find the DRM render and card
// nodes created by the driver. Returns an error if the driver is not loaded or
// has not yet bound to the device.
func discoverNPUNodes() (npuNodes, error) {
	entries, err := os.ReadDir(npuSysFSBase)
	if err != nil {
		return npuNodes{}, fmt.Errorf("rknpu sysfs not found (%s): %w — is rknpu.ko loaded?", npuSysFSBase, err)
	}

	var nodes npuNodes
	for _, e := range entries {
		name := e.Name()
		switch {
		case strings.HasPrefix(name, "renderD"):
			nodes.renderNode = "/dev/dri/" + name
		case strings.HasPrefix(name, "card"):
			nodes.cardNode = "/dev/dri/" + name
		}
	}

	if nodes.renderNode == "" {
		return npuNodes{}, fmt.Errorf("no renderD* node found under %s", npuSysFSBase)
	}
	return nodes, nil
}

// ---------------------------------------------------------------------------
// CDI spec management
// ---------------------------------------------------------------------------

func writeCDISpec(nodes npuNodes) error {
	if err := os.MkdirAll(cdiSpecDir, 0755); err != nil {
		return fmt.Errorf("create cdi dir: %w", err)
	}

	edits := containerEdit{}

	// Primary: DRM render node (no DRM master required — safe for unprivileged use).
	edits.DeviceNodes = append(edits.DeviceNodes,
		deviceNode{Path: nodes.renderNode, Permissions: "rw"},
	)

	// Card node — some RKNN operations (e.g. power-state query) use the master fd.
	if nodes.cardNode != "" {
		edits.DeviceNodes = append(edits.DeviceNodes,
			deviceNode{Path: nodes.cardNode, Permissions: "rw"},
		)
	}

	// dma_heap is intentionally omitted from CDI deviceNodes.
	// CDI creates device nodes via mknod (not bind-mount), which produces mode
	// 0600 owned by uid 65534 (host uid 0 is unmapped in user namespaces) —
	// that blocks O_RDWR. Instead, dma_heap is exposed via the kubelet DeviceSpec
	// in Allocate(), which bind-mounts from the host and preserves the 0666 mode.

	// librknnrt.so — installed by rockchip-rknn-libs Talos extension.
	// Bind-mount into the container so the application does not need to bundle it.
	if _, err := os.Stat(librknnrt); err == nil {
		edits.Mounts = append(edits.Mounts, mount{
			HostPath:      librknnrt,
			ContainerPath: librknnrt,
			Options:       []string{"ro", "bind"},
		})
	}

	spec := cdiSpec{
		CDIVersion: "0.6.0",
		Kind:       cdiKind,
		Devices:    []cdiDevice{{Name: "0", ContainerEdits: edits}},
	}

	data, err := yaml.Marshal(&spec)
	if err != nil {
		return fmt.Errorf("marshal cdi spec: %w", err)
	}
	if err := os.WriteFile(cdiSpecFile, data, 0644); err != nil {
		return fmt.Errorf("write cdi spec: %w", err)
	}

	log.Printf("CDI spec written to %s (render=%s card=%s)", cdiSpecFile, nodes.renderNode, nodes.cardNode)
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
// sysfs glitch (e.g. DRM power-management during active NPU inference) must
// not cause kubelet to kill running pods. At pollInterval=10s this gives a
// 30-second grace window before health transitions to Unhealthy.
const unhealthyThreshold = 3

// ListAndWatch rediscovers the DRM nodes on every poll so the health status
// reflects module load/unload without requiring a pod restart.
// Health transitions use hysteresis: the device is only reported Unhealthy
// after unhealthyThreshold consecutive failures, and recovers immediately on
// a single successful poll.
func (p *npuPlugin) ListAndWatch(_ *v1beta1.Empty, stream v1beta1.DevicePlugin_ListAndWatchServer) error {
	log.Println("ListAndWatch: starting")
	var lastNodes npuNodes
	var failCount int

	for {
		nodes, err := discoverNPUNodes()
		if err != nil {
			failCount++
			log.Printf("ListAndWatch: device unavailable (%d/%d): %v", failCount, unhealthyThreshold, err)
		} else if !nodes.healthy() {
			failCount++
			log.Printf("ListAndWatch: render node %s not accessible (%d/%d)", nodes.renderNode, failCount, unhealthyThreshold)
		} else {
			failCount = 0
		}

		health := v1beta1.Healthy
		if failCount >= unhealthyThreshold {
			health = v1beta1.Unhealthy
		}

		// Rewrite CDI spec if the node assignment changed (e.g. after module reload).
		if nodes.renderNode != lastNodes.renderNode && nodes.renderNode != "" {
			if err := writeCDISpec(nodes); err != nil {
				log.Printf("ListAndWatch: CDI spec update failed: %v", err)
			}
			lastNodes = nodes
		}

		if err := stream.Send(&v1beta1.ListAndWatchResponse{
			Devices: []*v1beta1.Device{{ID: deviceID, Health: health}},
		}); err != nil {
			log.Printf("ListAndWatch: send error: %v", err)
			return err
		}

		time.Sleep(pollInterval)
	}
}

func (p *npuPlugin) Allocate(_ context.Context, req *v1beta1.AllocateRequest) (*v1beta1.AllocateResponse, error) {
	nodes, err := discoverNPUNodes()
	if err != nil {
		return nil, fmt.Errorf("allocate: NPU not available: %w", err)
	}

	resp := &v1beta1.AllocateResponse{}
	for range req.ContainerRequests {
		cr := &v1beta1.ContainerAllocateResponse{
			// CDI path: containerd reads /var/run/cdi/rockchip-npu.yaml and injects
			// the full device list (render node, card node, dma_heap, librknnrt.so).
			CDIDevices: []*v1beta1.CDIDevice{{Name: cdiDeviceRef}},
			// DeviceSpec: kubelet uses this to bind-mount device nodes into the
			// container and add them to the cgroupv2 device eBPF allowlist.
			// We list all three device nodes here so kubelet handles cgroup access
			// regardless of whether CDI is fully functional.
			Devices: []*v1beta1.DeviceSpec{
				{HostPath: nodes.renderNode, ContainerPath: nodes.renderNode, Permissions: "rw"},
			},
		}
		if nodes.cardNode != "" {
			cr.Devices = append(cr.Devices, &v1beta1.DeviceSpec{
				HostPath: nodes.cardNode, ContainerPath: nodes.cardNode, Permissions: "rw",
			})
		}
		// dma_heap: librknnrt.so uses dma_buf for zero-copy CPU↔NPU transfers.
		// Include it in DeviceSpec (not just CDI) so kubelet handles the cgroup
		// device allowlist — CDI alone leaves the file mode wrong (mknod vs bind).
		if _, err := os.Stat("/dev/dma_heap/system"); err == nil {
			cr.Devices = append(cr.Devices, &v1beta1.DeviceSpec{
				HostPath:      "/dev/dma_heap/system",
				ContainerPath: "/dev/dma_heap/system",
				Permissions:   "rw",
			})
		}
		resp.ContainerResponses = append(resp.ContainerResponses, cr)
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

	// Wait up to 60 s for the rknpu driver to bind and create DRM nodes.
	// This handles the case where the DaemonSet pod starts before udevd
	// has finished processing the module load event.
	var nodes npuNodes
	var err error
	for i := 0; i < 12; i++ {
		nodes, err = discoverNPUNodes()
		if err == nil {
			break
		}
		log.Printf("Waiting for rknpu DRM nodes (%d/12): %v", i+1, err)
		time.Sleep(5 * time.Second)
	}
	if err != nil {
		log.Fatalf("rknpu device not found after 60 s: %v", err)
	}
	log.Printf("Discovered NPU: render=%s card=%s", nodes.renderNode, nodes.cardNode)

	// The kernel creates /dev/dma_heap/system with mode 0600. The udev rule in
	// the rockchip-rknpu extension overrides this to 0666, but until the extension
	// is rebuilt we set it here. Running as uid 0 (file owner) so no CAP_FOWNER needed.
	if err := os.Chmod("/dev/dma_heap/system", 0o666); err != nil {
		log.Printf("Warning: chmod /dev/dma_heap/system: %v (device may be inaccessible in user-namespace pods)", err)
	} else {
		log.Println("chmod 0666 /dev/dma_heap/system")
	}

	if err := writeCDISpec(nodes); err != nil {
		log.Fatalf("Failed to write CDI spec: %v", err)
	}

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
