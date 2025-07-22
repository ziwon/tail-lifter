# Variables
BPF_OUT      := "bpf/tail_lifter.bpf.o"
CLANG        := "clang"
CLANG_FLAGS  := "-O2 -target bpf -Wall -g -I/usr/include/x86_64-linux-gnu"
KIND_CLUSTER := "tail-lifter-dev"
IMAGE        := "ghcr.io/ziwon/tail-lifter:latest"

# Default recipe
default:
    @just --list

# Build the BPF object
build:
    {{CLANG}} {{CLANG_FLAGS}} -c bpf/tail_lifter.bpf.c -o {{BPF_OUT}}

# Load into a local veth for quick smoke test
dev-load build:
    sudo tc qdisc add dev veth0 clsact 2>/dev/null || true
    sudo tc filter replace dev veth0 ingress bpf obj {{BPF_OUT}} sec tc/ingress
    sudo tc filter replace dev veth0 egress  bpf obj {{BPF_OUT}} sec tc/egress

# Spin up a KIND cluster with Cilium
kind-up:
    kind create cluster --name {{KIND_CLUSTER}} --config deploy/kind.yaml
    helm repo add cilium https://helm.cilium.io
    helm upgrade --install cilium cilium/cilium \
        --namespace kube-system --set hostServices.enabled=true

# Destroy KIND cluster
kind-down:
    kind delete cluster --name {{KIND_CLUSTER}}

# Build & push the production image
image-push: build 
    docker push {{IMAGE}}

# Build only the production image (no push)
image-build:
    docker build -t {{IMAGE}} .

# Install the DaemonSet on the current context
deploy image:
    envsubst < deploy/daemonset.yaml | kubectl apply -f -

# Clean everything
clean:
    rm -f {{BPF_OUT}}
    sudo tc qdisc del dev veth0 clsact 2>/dev/null || true
    kubectl delete -f deploy/daemonset.yaml 2>/dev/null || trueu