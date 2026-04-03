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
	"gopkg.in/yaml.v3"
	v1beta1 "k8s.io/kubelet/pkg/apis/deviceplugin/v1beta1"
)

const (
	resourceName = "rockchip.com/npu"
	deviceID     = "rknpu0"
	devicePath   = "/dev/rknpu"
	librknnrt    = "/usr/lib/librknnrt.so"

	cdiKind       = "rockchip.com/npu"
	cdiDeviceRef  = "rockchip.com/npu=0"
	cdiSpecDir    = "/var/run/cdi"
	cdiSpecFile   = "/var/run/cdi/rockchip-npu.yaml"

	kubeletSock = "/var/lib/kubelet/device-plugins/kubelet.sock"
	pluginSock  = "/var/lib/kubelet/device-plugins/rk3588-npu.sock"

	pollInterval = 30 * time.Second
)

// ---------------------------------------------------------------------------
// CDI spec structures (CDI spec v0.6.0)
// ---------------------------------------------------------------------------

type cdiSpec struct {
	CDIVersion    string      `yaml:"cdiVersion"`
	Kind          string      `yaml:"kind"`
	Devices       []cdiDevice `yaml:"devices"`
	ContainerEdits []containerEdit `yaml:"containerEdits,omitempty"`
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
// Device plugin server
// ---------------------------------------------------------------------------

type npuPlugin struct {
	v1beta1.UnimplementedDevicePluginServer
	grpcServer *grpc.Server
}

func newPlugin() *npuPlugin {
	return &npuPlugin{}
}

func (p *npuPlugin) GetDevicePluginOptions(_ context.Context, _ *v1beta1.Empty) (*v1beta1.DevicePluginOptions, error) {
	return &v1beta1.DevicePluginOptions{}, nil
}

func (p *npuPlugin) ListAndWatch(_ *v1beta1.Empty, stream v1beta1.DevicePlugin_ListAndWatchServer) error {
	log.Println("ListAndWatch: starting")
	for {
		health := v1beta1.Healthy
		if _, err := os.Stat(devicePath); err != nil {
			health = v1beta1.Unhealthy
			log.Printf("ListAndWatch: %s not found, marking unhealthy", devicePath)
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
	resp := &v1beta1.AllocateResponse{}
	for range req.ContainerRequests {
		resp.ContainerResponses = append(resp.ContainerResponses,
			&v1beta1.ContainerAllocateResponse{
				// CDI injection: containerd reads /var/run/cdi/rockchip-npu.yaml
				CDIDevices: []*v1beta1.CDIDevice{{Name: cdiDeviceRef}},
				// Fallback for runtimes that do not support CDI
				Devices: []*v1beta1.DeviceSpec{{
					ContainerPath: devicePath,
					HostPath:      devicePath,
					Permissions:   "rw",
				}},
			},
		)
	}
	return resp, nil
}

func (p *npuPlugin) PreStartContainer(_ context.Context, _ *v1beta1.PreStartContainerRequest) (*v1beta1.PreStartContainerResponse, error) {
	return &v1beta1.PreStartContainerResponse{}, nil
}

// ---------------------------------------------------------------------------
// CDI spec management
// ---------------------------------------------------------------------------

func writeCDISpec() error {
	if err := os.MkdirAll(cdiSpecDir, 0755); err != nil {
		return fmt.Errorf("create cdi dir: %w", err)
	}

	edits := containerEdit{
		DeviceNodes: []deviceNode{
			{Path: devicePath, Permissions: "rw"},
		},
	}

	// Inject librknnrt.so if present on the host (installed by rockchip-rknn-libs extension)
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
		Devices: []cdiDevice{
			{Name: "0", ContainerEdits: edits},
		},
	}

	data, err := yaml.Marshal(&spec)
	if err != nil {
		return fmt.Errorf("marshal cdi spec: %w", err)
	}

	if err := os.WriteFile(cdiSpecFile, data, 0644); err != nil {
		return fmt.Errorf("write cdi spec: %w", err)
	}

	log.Printf("CDI spec written to %s", cdiSpecFile)
	return nil
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
	v1beta1.RegisterDevicePluginServer(p.grpcServer, p)

	go func() {
		if err := p.grpcServer.Serve(lis); err != nil {
			log.Fatalf("gRPC server error: %v", err)
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

	if err := writeCDISpec(); err != nil {
		log.Fatalf("Failed to write CDI spec: %v", err)
	}

	plugin := newPlugin()

	if err := plugin.start(); err != nil {
		log.Fatalf("Failed to start plugin server: %v", err)
	}

	if err := plugin.register(); err != nil {
		log.Fatalf("Failed to register with kubelet: %v", err)
	}

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig

	log.Println("Shutting down")
	plugin.stop()
}
