// tail-lifter.c
// SPDX-License-Identifier: GPL-2.0

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <linux/pkt_cls.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

/* ---------- Maps --------------------------------------------------------- */

/* Service map: ClusterIP -> PodIP (populated from userspace) */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 512);
    __type(key, __u32);   /* ClusterIP (network byte order) */
    __type(value, __u32); /* PodIP    (network byte order) */
} svc_map SEC(".maps");

/* Conntrack: 5-tuple -> original tuple (for SNAT on the way back) */
struct ct_key {
    __be32 saddr, daddr;
    __be16 sport, dport;
    __u8   proto;
};

struct ct_val {
    __be32 orig_daddr;
    __be16 orig_dport;
};

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 4096);
    __type(key, struct ct_key);
    __type(value, struct ct_val);
} ct SEC(".maps");

/* ---------- Helpers ------------------------------------------------------ */

static __always_inline __u16 csum_fold(__u32 csum)
{
    csum = (csum & 0xffff) + (csum >> 16);
    return ~(__u16)(csum + (csum >> 16));
}

static __always_inline void fix_csum(__u16 *csum,
                                     __be32 old, __be32 new_)
{
    __u32 delta = bpf_csum_diff(&old, 4, &new_, 4, ~(*csum));
    *csum = csum_fold(delta);
}

/* ---------- Ingress DNAT ------------------------------------------------- */

SEC("tc/ingress")
int tl_ingress(struct __sk_buff *skb)
{
    void *data_end = (void *)(long)skb->data_end;
    void *data     = (void *)(long)skb->data;

    struct ethhdr *eth = data;
    if (data + sizeof(*eth) > data_end)
        return TC_ACT_OK;

    if (eth->h_proto != bpf_htons(ETH_P_IP))
        return TC_ACT_OK;

    struct iphdr *iph = data + sizeof(*eth);
    if (data + sizeof(*eth) + sizeof(*iph) > data_end)
        return TC_ACT_OK;

    __be32 *pod_ip = bpf_map_lookup_elem(&svc_map, &iph->daddr);
    if (!pod_ip)
        return TC_ACT_OK;

    /* store original tuple for SNAT on egress */
    struct ct_key ck = {
        .saddr = iph->saddr,
        .daddr = *pod_ip,
        .sport = 0, /* filled below */
        .dport = 0,
        .proto = iph->protocol,
    };
    struct ct_val cv = {
        .orig_daddr = iph->daddr,
        .orig_dport = 0,
    };

    /* parse L4 ports */
    if (iph->protocol == IPPROTO_TCP) {
        struct tcphdr *tcp = (void *)iph + sizeof(*iph);
        if (data + sizeof(*eth) + sizeof(*iph) + sizeof(*tcp) > data_end)
            return TC_ACT_OK;
        ck.sport = tcp->source;
        ck.dport = tcp->dest;
        cv.orig_dport = tcp->dest;
    } else if (iph->protocol == IPPROTO_UDP) {
        struct udphdr *udp = (void *)iph + sizeof(*iph);
        if (data + sizeof(*eth) + sizeof(*iph) + sizeof(*udp) > data_end)
            return TC_ACT_OK;
        ck.sport = udp->source;
        ck.dport = udp->dest;
        cv.orig_dport = udp->dest;
    } else {
        return TC_ACT_OK;
    }

    bpf_map_update_elem(&ct, &ck, &cv, BPF_ANY);

    /* DNAT */
    __be32 old_dst = iph->daddr;
    iph->daddr = *pod_ip;
    fix_csum(&iph->check, old_dst, *pod_ip);

    /* L4 checksum update (TCP/UDP) */
    if (iph->protocol == IPPROTO_TCP) {
        struct tcphdr *tcp = (void *)iph + sizeof(*iph);
        fix_csum(&tcp->check, old_dst, *pod_ip);
    } else if (iph->protocol == IPPROTO_UDP) {
        struct udphdr *udp = (void *)iph + sizeof(*iph);
        __u16 old_csum = udp->check;
        if (old_csum != 0) {
            __u32 delta = bpf_csum_diff(&old_dst, 4, pod_ip, 4, ~old_csum);
            udp->check = csum_fold(delta);
        }
    }

    return bpf_redirect_neigh(skb->ifindex, NULL, 0, 0);
}

/* ---------- Egress SNAT -------------------------------------------------- */

SEC("tc/egress")
int tl_egress(struct __sk_buff *skb)
{
    void *data_end = (void *)(long)skb->data_end;
    void *data     = (void *)(long)skb->data;

    struct ethhdr *eth = data;
    if (data + sizeof(*eth) > data_end)
        return TC_ACT_OK;

    if (eth->h_proto != bpf_htons(ETH_P_IP))
        return TC_ACT_OK;

    struct iphdr *iph = data + sizeof(*eth);
    if (data + sizeof(*eth) + sizeof(*iph) > data_end)
        return TC_ACT_OK;

    struct ct_key ck = {
        .saddr = iph->saddr,
        .daddr = iph->daddr,
        .sport = 0,
        .dport = 0,
        .proto = iph->protocol,
    };

    if (iph->protocol == IPPROTO_TCP) {
        struct tcphdr *tcp = (void *)iph + sizeof(*iph);
        if (data + sizeof(*eth) + sizeof(*iph) + sizeof(*tcp) > data_end)
            return TC_ACT_OK;
        ck.sport = tcp->source;
        ck.dport = tcp->dest;
    } else if (iph->protocol == IPPROTO_UDP) {
        struct udphdr *udp = (void *)iph + sizeof(*iph);
        if (data + sizeof(*eth) + sizeof(*iph) + sizeof(*udp) > data_end)
            return TC_ACT_OK;
        ck.sport = udp->source;
    } else {
        return TC_ACT_OK;
    }

    struct ct_val *cv = bpf_map_lookup_elem(&ct, &ck);
    if (!cv)
        return TC_ACT_OK;

    /* SNAT */
    __be32 old_src = iph->saddr;
    iph->saddr = cv->orig_daddr;
    fix_csum(&iph->check, old_src, cv->orig_daddr);

    if (iph->protocol == IPPROTO_TCP) {
        struct tcphdr *tcp = (void *)iph + sizeof(*iph);
        fix_csum(&tcp->check, old_src, cv->orig_daddr);
    } else if (iph->protocol == IPPROTO_UDP) {
        struct udphdr *udp = (void *)iph + sizeof(*iph);
        __u16 old_csum = udp->check;
        if (old_csum != 0) {
            __u32 delta = bpf_csum_diff(&old_src, 4, &cv->orig_daddr, 4, ~old_csum);
            udp->check = csum_fold(delta);
        }
    }

    return TC_ACT_OK;
}

char _license[] SEC(".license") = "GPL";