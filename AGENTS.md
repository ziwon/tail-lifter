# AGENTS.md

This file provides guidance to AI Agent when working with code in this repository.

## Project Overview

TailLifter is a lightweight eBPF TC program that glues Tailscale VPN clients to Kubernetes ClusterIPs. Attached to the Tailscale interface, it performs on-the-fly DNAT (and optional SNAT) so VPN traffic aimed at a service IP is transparently steered to the correct pod and returned along the same path—no NodePorts, LoadBalancers or per-service configs required.

The project consists of:

1. **eBPF Program** (`bpf/tail_lifter.bpf.c`): TC-attached BPF program for ingress/egress packet processing
2. **Go Controller** (`cmd/tail-lifter/main.go`): Kubernetes controller that manages service mappings via eBPF maps
3. **Kubernetes Deployment**: DaemonSet configuration for cluster-wide deployment

## Architecture

- **eBPF Component**: Attaches to network interfaces via TC (clsact qdisc) for packet interception and forwarding
- **Controller Component**: Watches Kubernetes Endpoints and updates BPF maps (`svc_map`, `ct_map`) for service discovery
- **Deployment Model**: Runs as a DaemonSet on each node, typically attaching to Tailscale interface

## Development Commands

All commands use `just` (Justfile) for task automation:

```bash
# Build BPF object file
just build

# Local development testing (requires veth0 interface)
just dev-load

# KIND cluster management
just kind-up    # Create cluster with Cilium
just kind-down  # Destroy cluster

# Container image operations
just image      # Build and push Docker image
just deploy     # Deploy DaemonSet to current kubectl context

# Cleanup
just clean      # Remove BPF objects and tc rules
```

## Key Technical Details

- **BPF Map Management**: Uses pinned maps at `/sys/fs/bpf/` for inter-process communication
- **Network Interface**: Defaults to `tailscale0` interface (configurable via `IFACE` env var)
- **Service Discovery**: Naive IP allocation using `10.8.43.x` range based on service name/namespace hash
- **Build Requirements**: Requires `clang`, `llvm`, and `bpftool` for BPF compilation

## File Structure

- `bpf/` - eBPF C source code and compiled objects
- `cmd/tail-lifter/` - Go controller application
- `deploy/` - Kubernetes manifests (DaemonSet, RBAC, KIND config)
- `hack/install.sh` - Container entrypoint script for BPF program loading
- `justfile` - Task automation and build commands

## Development Environment

This project requires:
- Go 1.22+ for controller development
- Clang/LLVM for BPF compilation  
- KIND/Kubernetes cluster for testing
- Cilium for CNI (installed via Helm in kind-up)

The controller expects to run in-cluster with appropriate RBAC permissions to watch Endpoints resources.