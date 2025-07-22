# eBPF Program Technical Deep Dive

This document provides a technical explanation of the `bpf/tail_lifter.bpf.c` eBPF program that implements transparent DNAT/SNAT for Tailscale-to-Kubernetes connectivity.

## Program Architecture

The eBPF program consists of two TC (Traffic Control) programs that attach to the Tailscale interface:

- **`tl_ingress`**: Handles incoming packets (DNAT)
- **`tl_egress`**: Handles outgoing packets (SNAT)

## Data Structures

### Maps

#### Service Map (`svc_map`)
```c
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 512);
    __type(key, __u32);   /* ClusterIP (network byte order) */
    __type(value, __u32); /* PodIP    (network byte order) */
} svc_map SEC(".maps");
```
- Maps Kubernetes ClusterIPs to backend PodIPs
- Populated by the Go controller watching Endpoints
- Used for DNAT decision making

#### Connection Tracking Map (`ct`)
```c
struct ct_key {
    __be32 saddr, daddr;
    __be16 sport, dport;
    __u8   proto;
};

struct ct_val {
    __be32 orig_daddr;
    __be16 orig_dport;
};
```
- LRU hash map tracking active connections
- Stores original destination for return path SNAT
- Key: 5-tuple of connection after DNAT
- Value: Original ClusterIP and port

## Ingress Path (DNAT)

### Packet Processing Flow

1. **Ethernet Header Validation**
   ```c
   struct ethhdr *eth = data;
   if (data + sizeof(*eth) > data_end)
       return TC_ACT_OK;
   
   if (eth->h_proto != bpf_htons(ETH_P_IP))
       return TC_ACT_OK;
   ```

2. **IP Header Validation**
   ```c
   struct iphdr *iph = data + sizeof(*eth);
   if (data + sizeof(*eth) + sizeof(*iph) > data_end)
       return TC_ACT_OK;
   ```

3. **Service Lookup**
   ```c
   __be32 *pod_ip = bpf_map_lookup_elem(&svc_map, &iph->daddr);
   if (!pod_ip)
       return TC_ACT_OK;
   ```
   - Check if destination IP is a known ClusterIP
   - If not found, pass packet through unchanged

4. **Connection Tracking Setup**
   - Extract L4 ports (TCP/UDP)
   - Create connection tracking entry with:
     - Key: Post-DNAT 5-tuple (src_ip, pod_ip, src_port, dst_port, protocol)
     - Value: Original ClusterIP and port for return path

5. **DNAT Transformation**
   ```c
   __be32 old_dst = iph->daddr;
   iph->daddr = *pod_ip;
   fix_csum(&iph->check, old_dst, *pod_ip);
   ```
   - Replace destination IP with PodIP
   - Update IP header checksum

6. **L4 Checksum Update**
   - TCP: Update checksum using `fix_csum()`
   - UDP: Handle checksum (skip if zero, update otherwise)

7. **Packet Forwarding**
   ```c
   return bpf_redirect_neigh(skb->ifindex, NULL, 0, 0);
   ```
   - Use neighbor redirect for efficient forwarding

## Egress Path (SNAT)

### Packet Processing Flow

1. **Header Validation** (same as ingress)

2. **Connection Tracking Lookup**
   ```c
   struct ct_key ck = {
       .saddr = iph->saddr,
       .daddr = iph->daddr,
       .sport = ..., .dport = ...,
       .proto = iph->protocol,
   };
   
   struct ct_val *cv = bpf_map_lookup_elem(&ct, &ck);
   if (!cv)
       return TC_ACT_OK;
   ```
   - Look up connection in tracking table
   - If not found, packet doesn't need SNAT

3. **SNAT Transformation**
   ```c
   __be32 old_src = iph->saddr;
   iph->saddr = cv->orig_daddr;  // Restore original ClusterIP
   fix_csum(&iph->check, old_src, cv->orig_daddr);
   ```
   - Replace source IP with original ClusterIP
   - Update checksums (IP and L4)

4. **Pass Through**
   ```c
   return TC_ACT_OK;
   ```
   - Let kernel handle normal routing

## Helper Functions

### Checksum Calculation
```c
static __always_inline __u16 csum_fold(__u32 csum)
{
    csum = (csum & 0xffff) + (csum >> 16);
    return ~(__u16)(csum + (csum >> 16));
}
```

### Checksum Update
```c
static __always_inline void fix_csum(__u16 *csum, __be32 old, __be32 new_)
{
    __u32 delta = bpf_csum_diff(&old, 4, &new_, 4, ~(*csum));
    *csum = csum_fold(delta);
}
```
- Uses BPF helper `bpf_csum_diff()` for incremental checksum updates
- More efficient than recalculating entire checksum

## Protocol Support

### TCP
- Full checksum update required (always present)
- Port extraction from TCP header

### UDP
- Conditional checksum update (may be zero)
- Port extraction from UDP header

### Other Protocols
- Currently unsupported, packets pass through unchanged

## Security Considerations

1. **Bounds Checking**: All packet accesses are bounds-checked against `data_end`
2. **Map Size Limits**: Service map limited to 512 entries, CT map to 4096
3. **LRU Eviction**: Connection tracking uses LRU to prevent memory exhaustion

## Performance Characteristics

1. **Zero-copy**: Packet modifications done in-place
2. **Minimal Overhead**: Only processes packets destined for known ClusterIPs
3. **Kernel Bypass**: Uses `bpf_redirect_neigh()` for efficient forwarding
4. **Connection State**: Stateful tracking enables proper bidirectional NAT

## Integration Points

1. **Map Pinning**: Maps pinned to `/sys/fs/bpf/` for userspace access
2. **Controller Updates**: Go controller populates `svc_map` via pinned maps
3. **TC Attachment**: Programs attach to `clsact` qdisc on Tailscale interface

This eBPF program provides transparent, high-performance network address translation that seamlessly connects Tailscale VPN clients to Kubernetes services without requiring complex networking configuration.

## Userspace Controller Logic

The userspace component is a Go program that runs as a DaemonSet on each node. Its primary responsibility is to keep the `svc_map` eBPF map up-to-date.

- **Kubernetes API Watch**: It uses the `client-go` library to watch for changes to `Endpoints` resources in the cluster.
- **Map Population**: When an `Endpoints` object is added or updated, the controller iterates through the backend IP addresses. For each IP, it calculates a deterministic ClusterIP (based on the service name and namespace) and inserts a `ClusterIP -> PodIP` mapping into the `svc_map`.
- **Pinned Maps**: The controller accesses the eBPF map via the BPF filesystem, where the map is "pinned" to a well-known path (`/sys/fs/bpf/svc_map`). This allows the userspace program and the eBPF program to share data.

## Build and Loading Process

The eBPF program is built and loaded using the following steps:

1.  **Compilation**: The C source code in `bpf/tail_lifter.bpf.c` is compiled into eBPF bytecode using `clang`. The `justfile` defines the `build` recipe for this, specifying the correct target (`bpf`) and include paths.
2.  **Containerization**: The compiled eBPF bytecode is packaged into a container image along with the Go controller.
3.  **Loading**: When the pod starts, the Go controller is responsible for loading the eBPF bytecode from the file into the kernel.
4.  **Attachment**: The controller then attaches the `tl_ingress` and `tl_egress` programs to the `clsact` qdisc on the Tailscale network interface (`tailscale0`). This positions them to intercept all ingress and egress traffic on that interface.

## Packet Flow Diagram

Here is a high-level overview of the packet's journey:

```
Tailscale Client
      |
      v
+-----------------------+
| tailscale0 Interface  |
|      (DNAT)           |
|   tl_ingress eBPF     |
+-----------------------+
      |
      v
+-----------------------+
|  Kernel Networking    |
| (Forward to Pod)      |
+-----------------------+
      |
      v
+-----------------------+
|      Pod Network      |
+-----------------------+
      |
      v
   Pod Container
      ^
      | (Reply Packet)
      |
+-----------------------+
|      Pod Network      |
+-----------------------+
      ^
      |
+-----------------------+
|  Kernel Networking    |
+-----------------------+
      ^
      |
+-----------------------+
| tailscale0 Interface  |
|      (SNAT)           |
|    tl_egress eBPF     |
+-----------------------+
      ^
      |
      v
Tailscale Client
```