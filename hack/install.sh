#!/bin/sh
set -e

IFACE=${IFACE:-tailscale0}

# Create clsact qdisc if not exists
tc qdisc add dev "$IFACE" clsact 2>/dev/null || true

# Load ingress & egress programs
tc filter replace dev "$IFACE" ingress \
  bpf obj /opt/tail_lifter.bpf.o sec tc/ingress direct-action
tc filter replace dev "$IFACE" egress \
  bpf obj /opt/tail_lifter.bpf.o sec tc/egress direct-action

# Pin maps so the Go controller can find them
bpftool map pin id $(bpftool map show | awk '/svc_map/ {print $1}') /sys/fs/bpf/svc_map
bpftool map pin id $(bpftool map show | awk '/ct_map/ {print $1}') /sys/fs/bpf/ct_map

