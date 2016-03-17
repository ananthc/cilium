#include <node_config.h>
#include <lxc_config.h>

#include <iproute2/bpf_api.h>

#include <linux/icmpv6.h>
#include <sys/socket.h>

#include <stdint.h>
#include <stdio.h>

#include "lib/common.h"
#include "lib/ipv6.h"
#include "lib/icmp6.h"
#include "lib/eth.h"
#include "lib/dbg.h"
#include "lib/l3.h"
#include "lib/lxc.h"
#include "lib/nat46.h"

#ifndef DISABLE_PORT_MAP
static inline void map_lxc_out(struct __sk_buff *skb, int off)
{
	int i;
	__u8 nexthdr = 0;
	struct portmap local_map[] = {
#ifdef LXC_PORT_MAPPINGS
		LXC_PORT_MAPPINGS
#endif
	};

	if (ipv6_load_nexthdr(skb, off, &nexthdr) < 0)
		return;

	off += sizeof(struct ipv6hdr);

#define NR_PORTMAPS (sizeof(local_map) / sizeof(local_map[0]))

#pragma unroll
	for (i = 0; i < NR_PORTMAPS; i++)
		do_port_map_out(skb, off, nexthdr, &local_map[i]);
}
#endif /* DISABLE_PORT_MAP */

static inline int __inline__ do_l3_from_lxc(struct __sk_buff *skb, int nh_off)
{
	union v6addr dst = {};
	__u32 node_id;

	printk("L3 from lxc: skb %p len %d\n", skb, skb->len);

	if (verify_src_mac(skb) || verify_src_ip(skb, nh_off) ||
	    verify_dst_mac(skb))
		return TC_ACT_SHOT;

	load_ipv6_daddr(skb, nh_off, &dst);
	node_id = derive_node_id(&dst);

#ifndef DISABLE_PORT_MAP
	map_lxc_out(skb, nh_off);
#else
	printk("Port mapping disabled, skipping.\n");
#endif /* DISABLE_PORT_MAP */

	printk("node_id %x local %x\n", node_id, NODE_ID);

#ifdef HOST_IFINDEX
	if (1) {
		union v6addr host_ip = { . addr = HOST_IP };
		int ret;

		/* Packets to the host are punted to a dummy device */
		if (compare_ipv6_addr(&dst, &host_ip) == 0) {
			union macaddr router_mac = NODE_MAC, host_mac = HOST_IFINDEX_MAC;

			ret = __do_l3(skb, nh_off, (__u8 *) &router_mac.addr, (__u8 *) &host_mac.addr);
			if (ret == TC_ACT_REDIRECT || ret == -1)
				return ret;

			printk("Redirecting to host ifindex %d\n", HOST_IFINDEX);

			return redirect(HOST_IFINDEX, 0);
		}
	}
#endif

	if (node_id != NODE_ID) {
#ifdef ENCAP_IFINDEX
		return do_encapsulation(skb, node_id, LXC_SECLABEL_NB);
#else
		union macaddr router_mac = NODE_MAC;
		int ret;

		ret = __do_l3(skb, nh_off, NULL, (__u8 *) &router_mac.addr);
		if (ret == TC_ACT_REDIRECT || ret == -1)
			return ret;

		ipv6_store_flowlabel(skb, nh_off, LXC_SECLABEL_NB);

		/* Pass down to stack */
		return TC_ACT_OK;
#endif
	} else {
		ipv6_store_flowlabel(skb, nh_off, LXC_SECLABEL_NB);
		return do_l3(skb, nh_off, &dst, LXC_SECLABEL);
	}
}

__section("from-container")
int handle_ingress(struct __sk_buff *skb)
{
	int ret, nh_off = ETH_HLEN;
	__u8 nexthdr;

#ifdef ENABLE_NAT46
	/* First try to do v46 nat */
	if (skb->protocol == __constant_htons(ETH_P_IP)) {
		union v6addr sp = NAT46_SRC_PREFIX;
		union v6addr dp = NAT46_DST_PREFIX;
		ret = ipv4_to_ipv6(skb, nh_off, &sp, &dp);
		if (ret == -1) {
			printk("ipv4_to_ipv6 failed\n");
			return ret;
		}
		skb->tc_index = 1;
	}
#endif

	if (likely(skb->protocol == __constant_htons(ETH_P_IPV6))) {
		nexthdr = load_byte(skb, nh_off + offsetof(struct ipv6hdr, nexthdr));
		if (unlikely(nexthdr == IPPROTO_ICMPV6)) {
			ret = icmp6_handle(skb, nh_off);
			if (ret != LXC_REDIRECT)
				return ret;
		}

		return do_l3_from_lxc(skb, nh_off);
	}

	return TC_ACT_UNSPEC;
}

__BPF_MAP(LXC_POLICY_MAP, BPF_MAP_TYPE_HASH, 0, sizeof(__u32), sizeof(struct policy_entry), PIN_GLOBAL_NS, 1024);

__section_tail(CILIUM_MAP_JMP, LXC_SECLABEL) int handle_policy(struct __sk_buff *skb)
{
	struct policy_entry *policy;
	__u32 src_label = skb->cb[0];
	int ifindex = skb->cb[1];

#ifdef ENABLE_NAT46
	if (skb->tc_index == 0 &&
	    skb->protocol == __constant_htons(ETH_P_IPV6)) {
		int ret;
		union v6addr sp = NAT46_SRC_PREFIX;
		union v6addr dp = NAT46_DST_PREFIX;
		ret = ipv6_to_ipv4(skb, 14, &sp, &dp);
		if (ret == -1)
			printk("v64 nat failed\n");
	}
	skb->tc_index = 0;
#endif

	printk("Handle policy %d %d\n", src_label, ifindex);

	policy = map_lookup_elem(&LXC_POLICY_MAP, &src_label);
	if (!policy) {
		printk("Denied!\n");
		//return TC_ACT_SHOT;
		return redirect(ifindex, 0);
	}

	return redirect(ifindex, 0);
}

BPF_LICENSE("GPL");
