// SPDX-License-Identifier: GPL-2.0-only
#ifndef CAKE_TC_PLUGIN_ABI_H
#define CAKE_TC_PLUGIN_ABI_H

#include <stdio.h>
#include <sys/socket.h>
#include <linux/rtnetlink.h>

/* qdisc_util layout from the pinned firmware's iproute2/tc/tc_util.h. */
struct qdisc_util {
    struct qdisc_util *next;
    const char *id;
    int (*parse_qopt)(struct qdisc_util *, int, char **, struct nlmsghdr *);
    int (*print_qopt)(struct qdisc_util *, FILE *, struct rtattr *);
    int (*print_xstats)(struct qdisc_util *, FILE *, struct rtattr *);
    int (*parse_copt)(struct qdisc_util *, int, char **, struct nlmsghdr *);
    int (*print_copt)(struct qdisc_util *, FILE *, struct rtattr *);
};

#endif
