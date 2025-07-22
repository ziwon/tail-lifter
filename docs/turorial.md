# Tail-lifter eBPF Tutorial: A DevOps Guide

This tutorial explains how the Tail-lifter eBPF program works from a DevOps perspective. We'll focus on the practical aspects of how it integrates with Kubernetes and Tailscale, without getting too deep into the eBPF code itself.

## The Problem: Connecting Tailscale to Kubernetes Services

Imagine you have a Kubernetes cluster and you're using [Tailscale](https://tailscale.com/) to create a secure network between your machines. You want to access a service running inside your cluster from a machine on your Tailscale network.

Normally, you would expose your service using a `LoadBalancer` or a `NodePort`. However, this can be complex to set up and might expose your service to the public internet.

## The Solution: Tail-lifter

Tail-lifter provides a more elegant solution. It uses an eBPF program to transparently connect your Tailscale network to your Kubernetes services. This means you can access your services using their internal `ClusterIP` addresses, as if you were inside the cluster.

### How it Works: The Magic of eBPF

eBPF is a powerful technology that allows you to run custom programs inside the Linux kernel. This is like having a programmable network card that can inspect and modify network packets on the fly.

Tail-lifter uses an eBPF program that attaches to the `tailscale0` network interface. This program does two main things:

1.  **Ingress (Incoming Traffic):** When a packet arrives on the `tailscale0` interface, the eBPF program inspects it. If the packet is destined for a `ClusterIP` address, the program changes the destination IP to the `PodIP` of one of the pods backing that service. This is called **Destination Network Address Translation (DNAT)**.

2.  **Egress (Outgoing Traffic):** When a packet leaves the `tailscale0` interface, the eBPF program inspects it again. If the packet is coming from a pod that received a DNAT'd packet, the program changes the source IP back to the original `ClusterIP`. This is called **Source Network Address Translation (SNAT)**.

This all happens inside the kernel, so it's extremely fast and efficient.

### The Role of the Go Controller

The eBPF program needs to know which `ClusterIP`s map to which `PodIP`s. This is where the Go controller comes in.

The controller is a small program that runs inside the cluster. It watches the Kubernetes API for changes to `Endpoints` resources. When a new service is created or a pod is added or removed, the controller updates a special eBPF map called `svc_map`. This map is shared between the controller and the eBPF program, and it contains the mapping between `ClusterIP`s and `PodIP`s.

### The `daemonset.yaml` File

The `deploy/daemonset.yaml` file is a Kubernetes manifest that deploys the Tail-lifter controller and the eBPF program to every node in your cluster. It consists of two containers:

1.  **`loader` (Init Container):** This container runs once when the pod starts. It loads the eBPF program into the kernel and creates the `svc_map` and `ct_map` eBPF maps.

2.  **`populator` (Main Container):** This container runs the Go controller, which continuously watches for changes to `Endpoints` and updates the `svc_map`.

## Summary

Tail-lifter is a powerful tool that simplifies the process of connecting Tailscale to Kubernetes services. It uses eBPF to perform on-the-fly DNAT and SNAT, allowing you to access your services using their internal `ClusterIP` addresses. The Go controller keeps the eBPF program up-to-date with the latest service information from the Kubernetes API.
