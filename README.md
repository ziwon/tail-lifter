# tail-lifter

[![CI](https://github.com/ziwon/tail-lifter/actions/workflows/ci.yml/badge.svg)](https://github.com/ziwon/tail-lifter/actions/workflows/ci.yml)

**Transparently connect Tailscale VPN clients to Kubernetes ClusterIPs using eBPF.**

## The Problem

When using **Cilium CNI** with a **Tailscale subnet router**, Kubernetes Service IPs (which are virtual IPs) are unreachable due to asymmetric routing. Pod IPs work, but Service IPs do not allow direct connections from Tailscale.

## The Solution

`tail-lifter` uses eBPF to perform on-the-fly DNAT/SNAT, translating Service IPs to Pod IPs, enabling seamless connectivity over Tailscale.

```
[Tailscale Client] ⟶ [Service IP] ⟶ [eBPF DNAT] ⟶ [Pod IP] ⟶ [eBPF SNAT] ⟶ [Service IP] ⟶ [Tailscale Client]
```

## How It Works

1.  **eBPF Program**: Attached to the `tailscale0` interface, it intercepts and translates traffic between Service IPs and Pod IPs.
2.  **Go Controller**: Watches Kubernetes Endpoints and updates eBPF maps with `ServiceIP → PodIP` mappings.

## Further Reading
- [eBPF Tutorial: A DevOps Guide](docs/turorial.md)
- [eBPF Program Technical Deep Dive](docs/ebpf.md)
- [Local Testing Guide](docs/local.md)

## Quick Start

**Prerequisites:**
- Kubernetes cluster with Cilium CNI
- Tailscale subnet router configured

**Deploy:**
```bash
# Build and deploy
just image-build
kind load docker-image ghcr.io/ziwon/tail-lifter:latest --name your-cluster
kubectl apply -f deploy/daemonset.yaml

# Advertise your Service IPs via Tailscale
tailscale up --advertise-routes=10.96.0.0/16
```

**Test:**
```bash
# This should now work from any Tailscale client
curl your-service-ip:port
```

## Requirements

- **CNI**: Cilium (Calico doesn't need this)
- **Tailscale**: Subnet router mode
- **Kubernetes**: Service discovery enabled

## License

Apache 2.0 - see [LICENSE](./LICENSE)