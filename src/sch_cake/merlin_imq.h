#ifndef CAKE_MERLIN_IMQ_H
#define CAKE_MERLIN_IMQ_H

#include <linux/skbuff.h>
#include <linux/netfilter.h>
#include <linux/imq.h>
#include <net/sch_generic.h>

static inline void cake_free_skb(struct sk_buff *skb)
{
	/* IMQ's queued-drop destructor omits netfilter's hook module reference. */
	if ((skb->imq_flags & IMQ_F_ENQUEUE) && skb->nf_info &&
	    skb->destructor && !skb_shared(skb)) {
		struct nf_info *info = skb->nf_info;

		skb->destructor = NULL;
		skb->nf_info = NULL;
		skb->imq_flags = 0;
		nf_reinject(skb, info, NF_DROP);
		return;
	}

	/* A shared skb rejected during enqueue is still owned by imq_nf_queue. */
	kfree_skb(skb);
}

static inline int cake_qdisc_drop(struct sk_buff *skb, struct Qdisc *sch)
{
	cake_free_skb(skb);
	sch->qstats.drops++;
	return NET_XMIT_DROP;
}

#endif
