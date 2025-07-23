package main

import (
	"encoding/binary"
	"fmt"
	"log"
	"net"
	"time"

	"github.com/cilium/ebpf"
	"github.com/cilium/ebpf/rlimit"
	corev1 "k8s.io/api/core/v1"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/rest"
)

const (
	bpfPathSvc = "/sys/fs/bpf/svc_map"
	bpfPathCt  = "/sys/fs/bpf/ct_map"
)

var (
	byteOrder = binary.LittleEndian
)

func ipToUint32(ip string) uint32 {
	return byteOrder.Uint32(net.ParseIP(ip).To4())
}

func main() {
	// allow locked memory for BPF maps
	if err := rlimit.RemoveMemlock(); err != nil {
		log.Fatalf("rlimit: %v", err)
	}

	cfg, err := rest.InClusterConfig()
	if err != nil {
		log.Fatalf("in-cluster config: %v", err)
	}
	clientset, err := kubernetes.NewForConfig(cfg)
	if err != nil {
		log.Fatalf("clientset: %v", err)
	}

	// open pinned maps
	svcMap, err := ebpf.LoadPinnedMap(bpfPathSvc, &ebpf.LoadPinOptions{})
	if err != nil {
		log.Fatalf("load svc_map: %v", err)
	}
	_ = svcMap // unused in this minimal populator

	factory := informers.NewSharedInformerFactory(clientset, 30*time.Second)
	epInformer := factory.Core().V1().Endpoints().Informer()

	// on every Endpoints add/update/delete, rebuild svc_map
	epInformer.AddEventHandler(&endpointsHandler{svcMap: svcMap})
	stop := make(chan struct{})
	defer close(stop)
	go factory.Start(stop)

	log.Println("tail-lifter-ctl started")
	<-stop
}

type endpointsHandler struct {
	svcMap *ebpf.Map
}

func (h *endpointsHandler) OnAdd(obj interface{}, isInInitialList bool) { h.sync(obj) }
func (h *endpointsHandler) OnUpdate(_, obj interface{})                 { h.sync(obj) }
func (h *endpointsHandler) OnDelete(obj interface{})                    { h.sync(obj) }

func (h *endpointsHandler) sync(obj interface{}) {
	ep := obj.(*corev1.Endpoints)
	for _, subset := range ep.Subsets {
		for _, addr := range subset.Addresses {
			for range subset.Ports {
				clusterIP := serviceIPFor(ep.Namespace, ep.Name)
				if clusterIP == "" {
					continue
				}
				key := ipToUint32(clusterIP)
				val := ipToUint32(addr.IP)
				if err := h.svcMap.Update(&key, &val, ebpf.UpdateAny); err != nil {
					log.Printf("update svc_map %s->%s: %v", clusterIP, addr.IP, err)
				}
			}
		}
	}
}

func serviceIPFor(ns, name string) string {
	// naive, but good enough for demo
	return fmt.Sprintf("10.8.43.%d", (len(ns)+len(name))%254+1)
}
