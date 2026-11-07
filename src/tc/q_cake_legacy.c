// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "tc_plugin_abi.h"
#include "sch_cake/pkt_sched.h"

#define CAKE_TC_MAX_MSG 16384

struct option_value {
    int width;
    uint64_t value;
};

static const struct {
    const char *name;
    int attribute;
    uint32_t value;
} keywords[] = {
    {"nat", TCA_CAKE_NAT, 1}, {"nonat", TCA_CAKE_NAT, 0},
    {"ingress", TCA_CAKE_INGRESS, 1}, {"egress", TCA_CAKE_INGRESS, 0},
    {"wash", TCA_CAKE_WASH, 1}, {"nowash", TCA_CAKE_WASH, 0},
    {"autorate-ingress", TCA_CAKE_AUTORATE, 1}, {"noautorate", TCA_CAKE_AUTORATE, 0},
    {"split-gso", TCA_CAKE_SPLIT_GSO, 1}, {"no-split-gso", TCA_CAKE_SPLIT_GSO, 0},
    {"raw", TCA_CAKE_RAW, 1},
    {"besteffort", TCA_CAKE_DIFFSERV_MODE, CAKE_DIFFSERV_BESTEFFORT},
    {"diffserv3", TCA_CAKE_DIFFSERV_MODE, CAKE_DIFFSERV_DIFFSERV3},
    {"diffserv4", TCA_CAKE_DIFFSERV_MODE, CAKE_DIFFSERV_DIFFSERV4},
    {"diffserv8", TCA_CAKE_DIFFSERV_MODE, CAKE_DIFFSERV_DIFFSERV8},
    {"precedence", TCA_CAKE_DIFFSERV_MODE, CAKE_DIFFSERV_PRECEDENCE},
    {"flowblind", TCA_CAKE_FLOW_MODE, CAKE_FLOW_NONE},
    {"srchost", TCA_CAKE_FLOW_MODE, CAKE_FLOW_SRC_IP},
    {"dsthost", TCA_CAKE_FLOW_MODE, CAKE_FLOW_DST_IP},
    {"hosts", TCA_CAKE_FLOW_MODE, CAKE_FLOW_HOSTS},
    {"flows", TCA_CAKE_FLOW_MODE, CAKE_FLOW_FLOWS},
    {"dual-srchost", TCA_CAKE_FLOW_MODE, CAKE_FLOW_DUAL_SRC},
    {"dual-dsthost", TCA_CAKE_FLOW_MODE, CAKE_FLOW_DUAL_DST},
    {"triple-isolate", TCA_CAKE_FLOW_MODE, CAKE_FLOW_TRIPLE},
    {"no-ack-filter", TCA_CAKE_ACK_FILTER, CAKE_ACK_NONE},
    {"ack-filter", TCA_CAKE_ACK_FILTER, CAKE_ACK_FILTER},
    {"ack-filter-aggressive", TCA_CAKE_ACK_FILTER, CAKE_ACK_AGGRESSIVE},
    {"noatm", TCA_CAKE_ATM, CAKE_ATM_NONE},
    {"atm", TCA_CAKE_ATM, CAKE_ATM_ATM},
    {"ptm", TCA_CAKE_ATM, CAKE_ATM_PTM}
};

static void usage(void)
{
    fprintf(stderr, "CAKE legacy plugin: bandwidth RATE[kbit|mbit|gbit] | unlimited\n"
        "  [besteffort|diffserv3|diffserv4|diffserv8|precedence]\n"
        "  [flowblind|flows|srchost|dsthost|hosts|dual-srchost|dual-dsthost|triple-isolate]\n"
        "  [nat|nonat] [ingress|egress] [rtt TIME[us|ms|s]] [target TIME[us|ms|s]]\n"
        "  [overhead BYTES] [mpu BYTES] [memlimit SIZE[b|kb|mb]] [fwmark MASK]\n"
        "  [wash|nowash] [autorate-ingress|noautorate] [split-gso|no-split-gso]\n"
        "  [no-ack-filter|ack-filter|ack-filter-aggressive] [raw|noatm|atm|ptm]\n");
}

static int scaled(const char *text, int kind, uint64_t *result)
{
    char *end;
    double factor = 1, limit = kind == 0 ? 1.0e12 : 4294967295.0;
    double value;
    errno = 0;
    value = strtod(text, &end);
    if (errno || end == text || !(value > 0))
        return -1;
    if (kind == 0) {
        if (!strcasecmp(end, "bit")) factor = 1.0 / 8;
        else if (!strcasecmp(end, "kbit")) factor = 1000.0 / 8;
        else if (!strcasecmp(end, "mbit")) factor = 1000000.0 / 8;
        else if (!strcasecmp(end, "gbit")) factor = 1000000000.0 / 8;
        else return -1;
    } else if (kind == 1) {
        if (!strcmp(end, "us") || !*end) factor = 1;
        else if (!strcmp(end, "ms")) factor = 1000;
        else if (!strcmp(end, "s")) factor = 1000000;
        else return -1;
    } else {
        if (!strcasecmp(end, "b") || !*end) factor = 1;
        else if (!strcasecmp(end, "kb") || !strcasecmp(end, "kib")) factor = 1024;
        else if (!strcasecmp(end, "mb") || !strcasecmp(end, "mib")) factor = 1048576;
        else return -1;
    }
    value *= factor;
    if (!(value >= 1 && value <= limit))
        return -1;
    *result = (uint64_t)value;
    return 0;
}

static int put_attribute(struct nlmsghdr *n, int type, const void *data, int width)
{
    size_t offset = NLMSG_ALIGN(n->nlmsg_len);
    size_t length = RTA_LENGTH(width);
    struct rtattr *attribute;
    if (offset + RTA_ALIGN(length) > CAKE_TC_MAX_MSG)
        return -1;
    attribute = (struct rtattr *)((unsigned char *)n + offset);
    attribute->rta_type = type;
    attribute->rta_len = length;
    if (width)
        memcpy(RTA_DATA(attribute), data, width);
    memset((unsigned char *)attribute + length, 0, RTA_ALIGN(length) - length);
    n->nlmsg_len = offset + RTA_ALIGN(length);
    return 0;
}

static int parse_options(struct qdisc_util *q, int argc, char **argv, struct nlmsghdr *n)
{
    struct option_value options[TCA_CAKE_MAX + 1] = {{0, 0}};
    struct rtattr *nest;
    size_t nest_offset;
    int i;
    (void)q;
    while (argc) {
        unsigned int k;
        int attribute = 0, width = 4;
        uint64_t value = 0;
        const char *name = *argv++;
        argc--;
        for (k = 0; k < sizeof(keywords) / sizeof(keywords[0]); k++) {
            if (!strcmp(name, keywords[k].name)) {
                attribute = keywords[k].attribute;
                value = keywords[k].value;
                break;
            }
        }
        if (!attribute) {
            char *end;
            const char *arg;
            if (!strcmp(name, "help")) {
                usage();
                return -1;
            }
            if (!argc)
                goto invalid;
            arg = *argv++;
            argc--;
            if (!strcmp(name, "bandwidth")) {
                attribute = TCA_CAKE_BASE_RATE64;
                width = 8;
                if (strcmp(arg, "unlimited") && scaled(arg, 0, &value)) goto invalid;
            } else if (!strcmp(name, "rtt") || !strcmp(name, "target")) {
                attribute = !strcmp(name, "rtt") ? TCA_CAKE_RTT : TCA_CAKE_TARGET;
                if (scaled(arg, 1, &value)) goto invalid;
            } else if (!strcmp(name, "memlimit")) {
                attribute = TCA_CAKE_MEMORY;
                if (scaled(arg, 2, &value)) goto invalid;
            } else if (!strcmp(name, "overhead")) {
                long signed_value;
                errno = 0;
                signed_value = strtol(arg, &end, 10);
                if (errno || end == arg || *end || signed_value < -64 || signed_value > 256) goto invalid;
                attribute = TCA_CAKE_OVERHEAD;
                value = (uint32_t)(int32_t)signed_value;
            } else if (!strcmp(name, "mpu") || !strcmp(name, "fwmark")) {
                unsigned long parsed;
                errno = 0;
                parsed = strtoul(arg, &end, !strcmp(name, "fwmark") ? 0 : 10);
                if (errno || end == arg || *end || *arg == '-' || parsed > UINT32_MAX) goto invalid;
                attribute = !strcmp(name, "mpu") ? TCA_CAKE_MPU : TCA_CAKE_FWMARK;
                if (attribute == TCA_CAKE_MPU && parsed > 65535) goto invalid;
                value = parsed;
            } else goto invalid;
        }
        options[attribute].width = width;
        options[attribute].value = value;
    }
    nest_offset = NLMSG_ALIGN(n->nlmsg_len);
    if (put_attribute(n, TCA_OPTIONS, NULL, 0))
        return -1;
    for (i = 1; i <= TCA_CAKE_MAX; i++) {
        uint32_t value32 = options[i].value;
        if (options[i].width && put_attribute(n, i,
            options[i].width == 8 ? (const void *)&options[i].value : (const void *)&value32,
            options[i].width))
            return -1;
    }
    nest = (struct rtattr *)((unsigned char *)n + nest_offset);
    nest->rta_len = n->nlmsg_len - nest_offset;
    return 0;
invalid:
    fprintf(stderr, "Invalid or incomplete CAKE option\n");
    usage();
    return -1;
}

static void attributes(struct rtattr **table, int max, struct rtattr *nest)
{
    int length;
    struct rtattr *attribute;
    memset(table, 0, (max + 1) * sizeof(*table));
    if (!nest || nest->rta_len < RTA_LENGTH(0))
        return;
    length = RTA_PAYLOAD(nest);
    attribute = RTA_DATA(nest);
    while (RTA_OK(attribute, length)) {
        int type = attribute->rta_type & 0x3fff;
        if (type <= max)
            table[type] = attribute;
        attribute = RTA_NEXT(attribute, length);
    }
}

static uint64_t read_value(const struct rtattr *attribute, int width)
{
    uint64_t value = 0;
    if (attribute && (int)RTA_PAYLOAD(attribute) >= width)
        memcpy(&value, RTA_DATA(attribute), width);
    return value;
}

static int print_options(struct qdisc_util *q, FILE *out, struct rtattr *option)
{
    struct rtattr *table[TCA_CAKE_MAX + 1];
    unsigned int k;
    (void)q;
    attributes(table, TCA_CAKE_MAX, option);
    if (table[TCA_CAKE_BASE_RATE64]) {
        uint64_t rate = read_value(table[TCA_CAKE_BASE_RATE64], 8);
        if (rate) fprintf(out, "bandwidth %.3fMbit ", (double)rate * 8 / 1000000);
        else fprintf(out, "bandwidth unlimited ");
    }
    for (k = 0; k < sizeof(keywords) / sizeof(keywords[0]); k++) {
        int attribute = keywords[k].attribute;
        if (attribute != TCA_CAKE_RAW && table[attribute] &&
            read_value(table[attribute], 4) == keywords[k].value)
            fprintf(out, "%s ", keywords[k].name);
    }
    if (table[TCA_CAKE_RTT]) fprintf(out, "rtt %lluus ", (unsigned long long)read_value(table[TCA_CAKE_RTT], 4));
    if (table[TCA_CAKE_TARGET]) fprintf(out, "target %lluus ", (unsigned long long)read_value(table[TCA_CAKE_TARGET], 4));
    if (table[TCA_CAKE_OVERHEAD]) fprintf(out, "overhead %d ", (int32_t)read_value(table[TCA_CAKE_OVERHEAD], 4));
    if (table[TCA_CAKE_MPU]) fprintf(out, "mpu %llu ", (unsigned long long)read_value(table[TCA_CAKE_MPU], 4));
    if (table[TCA_CAKE_MEMORY]) fprintf(out, "memlimit %llu ", (unsigned long long)read_value(table[TCA_CAKE_MEMORY], 4));
    return 0;
}

static int print_stats(struct qdisc_util *q, FILE *out, struct rtattr *stats)
{
    struct rtattr *table[TCA_CAKE_STATS_MAX + 1];
    struct rtattr *tins[TC_CAKE_MAX_TINS + 1];
    int i;
    (void)q;
    attributes(table, TCA_CAKE_STATS_MAX, stats);
    fprintf(out, " memory peak %llu limit %llu\n",
        (unsigned long long)read_value(table[TCA_CAKE_STATS_MEMORY_USED], 4),
        (unsigned long long)read_value(table[TCA_CAKE_STATS_MEMORY_LIMIT], 4));
    attributes(tins, TC_CAKE_MAX_TINS, table[TCA_CAKE_STATS_TIN_STATS]);
    for (i = 1; i <= TC_CAKE_MAX_TINS; i++) {
        struct rtattr *tin[TCA_CAKE_TIN_STATS_MAX + 1];
        if (!tins[i]) continue;
        attributes(tin, TCA_CAKE_TIN_STATS_MAX, tins[i]);
        fprintf(out, " tin %d: sent %llu packets, dropped %llu, ECN %llu, avg %lluus, peak %lluus\n", i,
            (unsigned long long)read_value(tin[TCA_CAKE_TIN_STATS_SENT_PACKETS], 4),
            (unsigned long long)read_value(tin[TCA_CAKE_TIN_STATS_DROPPED_PACKETS], 4),
            (unsigned long long)read_value(tin[TCA_CAKE_TIN_STATS_ECN_MARKED_PACKETS], 4),
            (unsigned long long)read_value(tin[TCA_CAKE_TIN_STATS_AVG_DELAY_US], 4),
            (unsigned long long)read_value(tin[TCA_CAKE_TIN_STATS_PEAK_DELAY_US], 4));
    }
    return 0;
}

struct qdisc_util cake_qdisc_util = {
    .id = "cake", .parse_qopt = parse_options,
    .print_qopt = print_options, .print_xstats = print_stats
};
