#ifndef CAKE_MERLIN_COMPAT_H
#define CAKE_MERLIN_COMPAT_H

#include <linux/random.h>
#include <linux/ipv6.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <asm/pgtable.h>
#include <asm/highmem.h>
#include <linux/imq.h>
#include "merlin_imq.h"

#define __rcu
#define BIT(nr) (1UL << (nr))
#define ETH_P_8021AD 0x88a8
#define TCPOPT_EXP 254
#define NLA_S32 NLA_U32
#define __NET_XMIT_BYPASS 0x10000
#define __NET_XMIT_STOLEN 0x20000
#define rcu_dereference_bh(p) rcu_dereference(p)
#define prandom_u32() random32()
#define consume_skb(skb) cake_free_skb(skb)
#define qdisc_dev(sch) ((sch)->dev)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))

#define CAKE_DEFINED_1 0,
#define CAKE_SECOND(ignored, value, ...) value
#define CAKE_DEFINED_STEP(arg) CAKE_SECOND(arg 1, 0)
#define CAKE_DEFINED_EXPAND(value) CAKE_DEFINED_STEP(CAKE_DEFINED_##value)
#define config_enabled(option) CAKE_DEFINED_EXPAND(option)

typedef unsigned long netdev_features_t;

static inline int is_vmalloc_addr(const void *ptr)
{
	unsigned long addr = (unsigned long)ptr;
	return addr >= VMALLOC_START && addr < VMALLOC_END;
}

static inline void *vzalloc(unsigned long size)
{
	void *ptr = vmalloc(size);
	if (ptr)
		memset(ptr, 0, size);
	return ptr;
}

struct cake_legacy_skb_cb {
	u8 protocol_private[24];
	u32 pkt_len;
	u32 padding;
	u8 data[16];
};

static inline struct cake_legacy_skb_cb *qdisc_skb_cb(const struct sk_buff *skb)
{
	BUILD_BUG_ON(sizeof(struct cake_legacy_skb_cb) > sizeof(skb->cb));
	BUILD_BUG_ON(sizeof(struct inet_skb_parm) > 24);
	BUILD_BUG_ON(sizeof(struct inet6_skb_parm) > 24);
	return (struct cake_legacy_skb_cb *)skb->cb;
}

#define qdisc_cb_private_validate(skb, size) \
	BUILD_BUG_ON((size) > sizeof(((struct cake_legacy_skb_cb *)0)->data))
#define qdisc_pkt_len(skb) ((skb)->len)

static inline void qdisc_bstats_update(struct Qdisc *sch,
				       const struct sk_buff *skb)
{
	sch->bstats.bytes += skb->len;
	sch->bstats.packets++;
}

static inline ktime_t ns_to_ktime(u64 ns)
{
	return ktime_add_ns(ktime_set(0, 0), ns);
}

static inline int ktime_compare(ktime_t a, ktime_t b)
{
	s64 na = ktime_to_ns(a), nb = ktime_to_ns(b);
	return (na > nb) - (na < nb);
}

static inline void qdisc_watchdog_schedule_ns(struct qdisc_watchdog *wd, u64 ns)
{
	qdisc_watchdog_schedule(wd, PSCHED_NS2US(ns));
}

#define qdisc_tree_reduce_backlog(sch, packets, bytes) \
	qdisc_tree_decrease_qlen(sch, packets)

static inline u32 get_unaligned_be32(const void *ptr)
{
	__be32 value;
	memcpy(&value, ptr, sizeof(value));
	return be32_to_cpu(value);
}

static inline s32 nla_get_s32(struct nlattr *nla)
{
	return (s32)nla_get_u32(nla);
}

static inline int nla_put_s32(struct sk_buff *skb, int type, s32 value)
{
	return nla_put_u32(skb, type, (u32)value);
}

/* Force segmentation and checksum work into software on the legacy stack. */
static inline netdev_features_t netif_skb_features(const struct sk_buff *skb)
{
	return 0;
}

/* Legacy IMQ segments would share a non-refcounted nf_info owner. */
static inline bool cake_may_split_gso(const struct sk_buff *skb)
{
	return !(skb->imq_flags & IMQ_F_ENQUEUE);
}

struct flow_keys {
	__be32 src, dst;
	union {
		__be32 ports;
		__be16 port16[2];
	};
	u8 ip_proto;
};

static inline bool skb_flow_dissect(const struct sk_buff *skb,
				   struct flow_keys *keys)
{
	unsigned int offset = skb_network_offset(skb);
	__be16 proto = skb->protocol;
	__be32 portbuf;
	const __be32 *ports;
	unsigned int depth;

	memset(keys, 0, sizeof(*keys));
	for (depth = 0; depth < 4; depth++) {
		struct vlan_hdr buffer;
		const struct vlan_hdr *vlan;
		if (proto != htons(ETH_P_8021Q) && proto != htons(ETH_P_8021AD))
			break;
		vlan = skb_header_pointer(skb, offset, sizeof(buffer), &buffer);
		if (!vlan)
			return false;
		proto = vlan->h_vlan_encapsulated_proto;
		offset += sizeof(buffer);
	}
	if (proto == htons(ETH_P_IP)) {
		struct iphdr buffer;
		const struct iphdr *ip;
		ip = skb_header_pointer(skb, offset, sizeof(buffer), &buffer);
		if (!ip || ip->version != 4 || ip->ihl < 5)
			return false;
		keys->src = ip->saddr;
		keys->dst = ip->daddr;
		keys->ip_proto = ip->protocol;
		if (ip->frag_off & htons(IP_MF | IP_OFFSET))
			return true;
		offset += ip->ihl * 4;
	} else if (proto == htons(ETH_P_IPV6)) {
		struct ipv6hdr buffer;
		const struct ipv6hdr *ip;
		u8 nexthdr;
		ip = skb_header_pointer(skb, offset, sizeof(buffer), &buffer);
		if (!ip || ip->version != 6)
			return false;
		keys->src = (__force __be32)jhash(&ip->saddr, sizeof(ip->saddr), 0);
		keys->dst = (__force __be32)jhash(&ip->daddr, sizeof(ip->daddr), 0);
		nexthdr = ip->nexthdr;
		offset += sizeof(buffer);
		for (depth = 0; depth < 16; depth++) {
			struct ipv6_opt_hdr buffer;
			const struct ipv6_opt_hdr *ext;
			if (nexthdr == IPPROTO_FRAGMENT) {
				keys->ip_proto = nexthdr;
				return true;
			}
			if (nexthdr != IPPROTO_HOPOPTS && nexthdr != IPPROTO_ROUTING &&
			    nexthdr != IPPROTO_DSTOPTS && nexthdr != IPPROTO_AH)
				break;
			ext = skb_header_pointer(skb, offset, sizeof(buffer), &buffer);
			if (!ext)
				return false;
			offset += nexthdr == IPPROTO_AH ? (ext->hdrlen + 2) * 4 :
				(ext->hdrlen + 1) * 8;
			nexthdr = ext->nexthdr;
		}
		if (depth == 16)
			return false;
		keys->ip_proto = nexthdr;
	} else {
		return false;
	}
	if (keys->ip_proto != IPPROTO_TCP && keys->ip_proto != IPPROTO_UDP &&
	    keys->ip_proto != IPPROTO_SCTP && keys->ip_proto != IPPROTO_DCCP)
		return true;
	ports = skb_header_pointer(skb, offset, sizeof(portbuf), &portbuf);
	if (!ports)
		return false;
	keys->ports = *ports;
	return true;
}

#define nf_ct_tuple(ct, dir) (&(ct)->tuplehash[dir].tuple)

#endif
