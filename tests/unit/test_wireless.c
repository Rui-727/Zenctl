/* test_wireless.c - wireless domain unit tests.
 *
 * Two layers:
 *
 *  1. Message construction / parsing: call the nl80211__build_* and
 *     nl80211__parse_* helpers (which take a buffer + family ID and
 *     have no side effects) and verify the on-wire byte layout. No
 *     socket is opened. This is the only way to test the netlink
 *     payload deterministically without a kernel.
 *
 *  2. Public API behavior: call zenctl_wireless_* against a mock
 *     sysfs tree. The paths that need a real nl80211 socket
 *     (set_regdomain on a valid code, set/get TX power, set power
 *     save) are exercised only for their error paths (bad input,
 *     missing interface) — the actual kernel round-trip is covered
 *     by the message-construction tests above and by `make smoke`
 *     against a live /sys.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>

#include "zenctl/zenctl.h"
#include "harness.h"
#include "mock_sysfs.h"

/* Private nl80211 helpers under test. */
#include "../lib/usb/nl80211.h"

/* ---- attribute walkers (mirror of the static ones in nl80211.c) ---- */

static const struct nlattr *find_attr(const void *attrs, size_t len,
                                      uint16_t type)
{
    size_t off = 0;
    while (off + NLA_HDRLEN <= len) {
        const struct nlattr *a = (const struct nlattr *)
                                 ((const char *)attrs + off);
        if (a->nla_len < NLA_HDRLEN) break;
        size_t aligned = NLA_ALIGN(a->nla_len);
        if (off + aligned > len) break;
        if (a->nla_type == type) return a;
        off += aligned;
    }
    return NULL;
}

static uint32_t attr_u32(const struct nlattr *a)
{
    uint32_t v = 0;
    memcpy(&v, (const char *)a + NLA_HDRLEN, sizeof(v));
    return v;
}

static int32_t attr_s32(const struct nlattr *a)
{
    int32_t v = 0;
    memcpy(&v, (const char *)a + NLA_HDRLEN, sizeof(v));
    return v;
}

static const struct nlmsghdr *as_nlmsg(const void *buf)
{
    return (const struct nlmsghdr *)buf;
}

static const struct genlmsghdr *as_genl(const void *buf)
{
    return (const struct genlmsghdr *)((const char *)buf + NLMSG_HDRLEN);
}

static const void *msg_attrs(const void *buf)
{
    return (const char *)buf + NLMSG_HDRLEN + GENL_HDRLEN;
}

static size_t msg_attrs_len(const void *buf)
{
    const struct nlmsghdr *h = as_nlmsg(buf);
    return (size_t)h->nlmsg_len - (NLMSG_HDRLEN + GENL_HDRLEN);
}

/* ---- build_getfamily ---- */

static void test_build_getfamily(void)
{
    unsigned char buf[128];
    memset(buf, 0xEE, sizeof(buf));
    ssize_t n = nl80211__build_getfamily(buf, sizeof(buf));
    OK(n > 0, "build_getfamily returns positive size");
    OK((size_t)n <= sizeof(buf), "build_getfamily fits in 128 bytes");

    const struct nlmsghdr *h = as_nlmsg(buf);
    OK(h->nlmsg_type == GENL_ID_CTRL,
       "getfamily nlmsg_type == GENL_ID_CTRL");
    OK(h->nlmsg_flags == NLM_F_REQUEST,
       "getfamily nlmsg_flags == NLM_F_REQUEST");
    OK(h->nlmsg_len == (uint32_t)n,
       "getfamily nlmsg_len matches returned size");

    const struct genlmsghdr *g = as_genl(buf);
    OK(g->cmd == CTRL_CMD_GETFAMILY,
       "getfamily genl cmd == CTRL_CMD_GETFAMILY");
    OK(g->version == 1, "getfamily genl version == 1");

    const struct nlattr *a = find_attr(msg_attrs(buf), msg_attrs_len(buf),
                                       CTRL_ATTR_FAMILY_NAME);
    OK(a != NULL, "getfamily has CTRL_ATTR_FAMILY_NAME");
    if (a) {
        const char *name = (const char *)a + NLA_HDRLEN;
        OK(strcmp(name, "nl80211") == 0,
           "getfamily family name == \"nl80211\"");
        OK(a->nla_len == NLA_HDRLEN + 8,
           "getfamily family name attr len == 7 chars + NUL");
    }
}

static void test_build_getfamily_overflow(void)
{
    unsigned char buf[8];
    ssize_t n = nl80211__build_getfamily(buf, sizeof(buf));
    OK(n < 0, "getfamily with 8-byte buffer returns -1");
    OK(errno == ENOBUFS, "getfamily overflow sets ENOBUFS");
}

/* ---- build_get_reg ---- */

static void test_build_get_reg(void)
{
    unsigned char buf[64];
    memset(buf, 0xEE, sizeof(buf));
    const int fid = 0x1a;  /* arbitrary */
    ssize_t n = nl80211__build_get_reg(buf, sizeof(buf), fid);
    OK(n > 0, "build_get_reg returns positive size");

    const struct nlmsghdr *h = as_nlmsg(buf);
    OK(h->nlmsg_type == (uint16_t)fid, "get_reg nlmsg_type == family_id");
    OK(h->nlmsg_flags == NLM_F_REQUEST, "get_reg flags == REQUEST");

    const struct genlmsghdr *g = as_genl(buf);
    OK(g->cmd == NL80211_CMD_GET_REG, "get_reg cmd == GET_REG");

    OK(msg_attrs_len(buf) == 0, "get_reg carries no attributes");
}

static void test_build_get_reg_bad_fid(void)
{
    unsigned char buf[64];
    OK(nl80211__build_get_reg(buf, sizeof(buf), 0) < 0,
       "get_reg with family_id=0 rejected");
    OK(errno == EINVAL, "get_reg bad family_id sets EINVAL");
}

/* ---- build_set_reg ---- */

static void test_build_set_reg(void)
{
    unsigned char buf[64];
    const int fid = 0x1a;
    ssize_t n = nl80211__build_set_reg(buf, sizeof(buf), fid, "DE");
    OK(n > 0, "build_set_reg returns positive size");

    const struct genlmsghdr *g = as_genl(buf);
    OK(g->cmd == NL80211_CMD_REQ_SET_REG, "set_reg cmd == REQ_SET_REG");

    const struct nlattr *a = find_attr(msg_attrs(buf), msg_attrs_len(buf),
                                       NL80211_ATTR_REG_ALPHA2);
    OK(a != NULL, "set_reg has REG_ALPHA2 attr");
    if (a) {
        OK(a->nla_len == NLA_HDRLEN + 2,
           "set_reg alpha2 attr len == 2 bytes (no NUL)");
        const char *p = (const char *)a + NLA_HDRLEN;
        OK(p[0] == 'D' && p[1] == 'E',
           "set_reg alpha2 payload == \"DE\"");
    }
}

static void test_build_set_reg_bad_alpha2(void)
{
    unsigned char buf[64];
    const int fid = 0x1a;
    OK(nl80211__build_set_reg(buf, sizeof(buf), fid, "USA") < 0,
       "set_reg with 3-char code rejected");
    OK(errno == EINVAL, "set_reg bad alpha2 sets EINVAL");
    OK(nl80211__build_set_reg(buf, sizeof(buf), fid, "X") < 0,
       "set_reg with 1-char code rejected");
    OK(nl80211__build_set_reg(buf, sizeof(buf), fid, NULL) < 0,
       "set_reg with NULL rejected");
}

/* ---- build_get_wiphy ---- */

static void test_build_get_wiphy(void)
{
    unsigned char buf[64];
    const int fid = 0x1a;
    ssize_t n = nl80211__build_get_wiphy(buf, sizeof(buf), fid, 7);
    OK(n > 0, "build_get_wiphy returns positive size");

    const struct genlmsghdr *g = as_genl(buf);
    OK(g->cmd == NL80211_CMD_GET_WIPHY, "get_wiphy cmd == GET_WIPHY");

    const struct nlattr *a = find_attr(msg_attrs(buf), msg_attrs_len(buf),
                                       NL80211_ATTR_WIPHY);
    OK(a != NULL, "get_wiphy has WIPHY attr");
    if (a) {
        OK(a->nla_len == NLA_HDRLEN + 4, "get_wiphy WIPHY attr len == 4");
        OK(attr_u32(a) == 7, "get_wiphy WIPHY payload == 7");
    }
}

static void test_build_get_wiphy_bad_idx(void)
{
    unsigned char buf[64];
    OK(nl80211__build_get_wiphy(buf, sizeof(buf), 0x1a, -1) < 0,
       "get_wiphy with negative phy_idx rejected");
    OK(errno == EINVAL, "get_wiphy bad phy_idx sets EINVAL");
}

/* ---- build_set_txpower ---- */

static void test_build_set_txpower_fixed(void)
{
    unsigned char buf[128];
    const int fid = 0x1a;
    ssize_t n = nl80211__build_set_txpower(buf, sizeof(buf), fid,
                                           3, NL80211_TX_POWER_FIXED,
                                           (int32_t)1500);
    OK(n > 0, "build_set_txpower(FIXED, 1500) returns positive size");

    const struct genlmsghdr *g = as_genl(buf);
    OK(g->cmd == NL80211_CMD_SET_WIPHY, "set_txpower cmd == SET_WIPHY");

    const struct nlattr *s = find_attr(msg_attrs(buf), msg_attrs_len(buf),
                                       NL80211_ATTR_WIPHY_TX_POWER_SETTING);
    const struct nlattr *l = find_attr(msg_attrs(buf), msg_attrs_len(buf),
                                       NL80211_ATTR_WIPHY_TX_POWER_LEVEL);
    OK(s != NULL, "set_txpower has TX_POWER_SETTING attr");
    OK(l != NULL, "set_txpower has TX_POWER_LEVEL attr");
    if (s) OK(attr_u32(s) == NL80211_TX_POWER_FIXED,
              "set_txpower SETTING == FIXED");
    if (l) OK(attr_s32(l) == 1500,
              "set_txpower LEVEL == 1500 mBm");
}

static void test_build_set_txpower_auto(void)
{
    unsigned char buf[128];
    const int fid = 0x1a;
    ssize_t n = nl80211__build_set_txpower(buf, sizeof(buf), fid,
                                           3, NL80211_TX_POWER_AUTOMATIC,
                                           0);
    OK(n > 0, "build_set_txpower(AUTO) returns positive size");

    const struct nlattr *l = find_attr(msg_attrs(buf), msg_attrs_len(buf),
                                       NL80211_ATTR_WIPHY_TX_POWER_LEVEL);
    OK(l == NULL, "set_txpower AUTO omits TX_POWER_LEVEL attr");

    const struct nlattr *s = find_attr(msg_attrs(buf), msg_attrs_len(buf),
                                       NL80211_ATTR_WIPHY_TX_POWER_SETTING);
    OK(s != NULL, "set_txpower AUTO still has SETTING attr");
    if (s) OK(attr_u32(s) == NL80211_TX_POWER_AUTOMATIC,
              "set_txpower AUTO SETTING == AUTOMATIC");
}

static void test_build_set_txpower_bad_setting(void)
{
    unsigned char buf[128];
    OK(nl80211__build_set_txpower(buf, sizeof(buf), 0x1a, 0, 99, 100) < 0,
       "set_txpower with setting=99 rejected");
    OK(errno == EINVAL, "set_txpower bad setting sets EINVAL");
}

/* ---- build_set_ps ---- */

static void test_build_set_ps(void)
{
    unsigned char buf[64];
    const int fid = 0x1a;
    ssize_t n = nl80211__build_set_ps(buf, sizeof(buf), fid, 5, true);
    OK(n > 0, "build_set_ps returns positive size");

    const struct genlmsghdr *g = as_genl(buf);
    OK(g->cmd == NL80211_CMD_SET_POWER_SAVE, "set_ps cmd == SET_POWER_SAVE");

    const struct nlattr *idx = find_attr(msg_attrs(buf), msg_attrs_len(buf),
                                         NL80211_ATTR_IFINDEX);
    const struct nlattr *ps  = find_attr(msg_attrs(buf), msg_attrs_len(buf),
                                         NL80211_ATTR_PS_STATE);
    OK(idx != NULL, "set_ps has IFINDEX attr");
    OK(ps  != NULL, "set_ps has PS_STATE attr");
    if (idx) OK(attr_u32(idx) == 5, "set_ps IFINDEX == 5");
    if (ps)  OK(attr_u32(ps)  == NL80211_PS_ENABLED,
                "set_ps(enabled=true) PS_STATE == ENABLED");

    /* disabled case */
    n = nl80211__build_set_ps(buf, sizeof(buf), fid, 5, false);
    OK(n > 0, "build_set_ps(disabled) returns positive size");
    ps = find_attr(msg_attrs(buf), msg_attrs_len(buf),
                   NL80211_ATTR_PS_STATE);
    if (ps) OK(attr_u32(ps) == NL80211_PS_DISABLED,
               "set_ps(enabled=false) PS_STATE == DISABLED");
}

static void test_build_set_ps_bad_ifindex(void)
{
    unsigned char buf[64];
    OK(nl80211__build_set_ps(buf, sizeof(buf), 0x1a, 0, true) < 0,
       "set_ps with ifindex=0 rejected");
    OK(nl80211__build_set_ps(buf, sizeof(buf), 0x1a, -1, true) < 0,
       "set_ps with ifindex=-1 rejected");
}

/* ---- parse_family_id ---- */

static void test_parse_family_id(void)
{
    /* Hand-craft a NEWFAMILY reply:
     *   nlmsghdr(type=GENL_ID_CTRL, len=...)
     *   genlmsghdr(cmd=CTRL_CMD_NEWFAMILY)
     *   nlattr(CTRL_ATTR_FAMILY_ID, u16=0x1a)
     *   nlattr(CTRL_ATTR_FAMILY_NAME, "nl80211\0")  */
    unsigned char msg[128];
    memset(msg, 0, sizeof(msg));
    size_t off = 0;

    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_type = GENL_ID_CTRL;
    h->nlmsg_flags = 0;
    h->nlmsg_seq = 0;
    h->nlmsg_pid = 0;
    off = NLMSG_HDRLEN;

    struct genlmsghdr *g = (struct genlmsghdr *)(msg + off);
    g->cmd = CTRL_CMD_NEWFAMILY;
    g->version = 1;
    off += GENL_HDRLEN;

    /* family id attr (u16 payload) */
    struct nlattr *a = (struct nlattr *)(msg + off);
    a->nla_type = CTRL_ATTR_FAMILY_ID;
    a->nla_len = (uint16_t)(NLA_HDRLEN + 2);
    uint16_t id = 0x1a;
    memcpy(msg + off + NLA_HDRLEN, &id, sizeof(id));
    off += NLA_ALIGN(a->nla_len);

    /* family name attr (string + NUL) */
    a = (struct nlattr *)(msg + off);
    a->nla_type = CTRL_ATTR_FAMILY_NAME;
    a->nla_len = (uint16_t)(NLA_HDRLEN + 8);  /* "nl80211\0" */
    memcpy(msg + off + NLA_HDRLEN, "nl80211", 8);
    off += NLA_ALIGN(a->nla_len);

    h->nlmsg_len = (uint32_t)off;

    int parsed_id = -1;
    OK(nl80211__parse_family_id(msg, off, &parsed_id) == 0,
       "parse_family_id succeeds on valid NEWFAMILY reply");
    OK(parsed_id == 0x1a, "parse_family_id returns 0x1a");
}

static void test_parse_family_id_missing(void)
{
    /* NEWFAMILY reply without CTRL_ATTR_FAMILY_ID. */
    unsigned char msg[64];
    memset(msg, 0, sizeof(msg));
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_type = GENL_ID_CTRL;
    h->nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN;
    struct genlmsghdr *g = (struct genlmsghdr *)(msg + NLMSG_HDRLEN);
    g->cmd = CTRL_CMD_NEWFAMILY;
    int id = -1;
    OK(nl80211__parse_family_id(msg, h->nlmsg_len, &id) < 0,
       "parse_family_id fails when FAMILY_ID attr is missing");
    OK(errno == ENOENT, "parse_family_id missing sets ENOENT");
}

/* ---- parse_txpower ---- */

static void test_parse_txpower(void)
{
    unsigned char msg[128];
    memset(msg, 0, sizeof(msg));
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_type = 0x1a;  /* arbitrary family id */
    size_t off = NLMSG_HDRLEN;
    struct genlmsghdr *g = (struct genlmsghdr *)(msg + off);
    g->cmd = NL80211_CMD_NEW_WIPHY;
    off += GENL_HDRLEN;

    /* Some unrelated attr first (to ensure walker skips it). */
    struct nlattr *a = (struct nlattr *)(msg + off);
    a->nla_type = NL80211_ATTR_WIPHY;
    a->nla_len = (uint16_t)(NLA_HDRLEN + 4);
    uint32_t phy = 3;
    memcpy(msg + off + NLA_HDRLEN, &phy, sizeof(phy));
    off += NLA_ALIGN(a->nla_len);

    /* The attr we want. */
    a = (struct nlattr *)(msg + off);
    a->nla_type = NL80211_ATTR_WIPHY_TX_POWER_LEVEL;
    a->nla_len = (uint16_t)(NLA_HDRLEN + 4);
    int32_t mbm = 1500;
    memcpy(msg + off + NLA_HDRLEN, &mbm, sizeof(mbm));
    off += NLA_ALIGN(a->nla_len);

    h->nlmsg_len = (uint32_t)off;

    int32_t out = -999;
    OK(nl80211__parse_txpower(msg, off, &out) == 0,
       "parse_txpower succeeds on NEW_WIPHY reply");
    OK(out == 1500, "parse_txpower returns 1500 mBm");
}

static void test_parse_txpower_negative(void)
{
    /* TX power of -500 mBm (-5 dBm) is valid; verify signed parsing. */
    unsigned char msg[64];
    memset(msg, 0, sizeof(msg));
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_type = 0x1a;
    size_t off = NLMSG_HDRLEN + GENL_HDRLEN;
    struct nlattr *a = (struct nlattr *)(msg + off);
    a->nla_type = NL80211_ATTR_WIPHY_TX_POWER_LEVEL;
    a->nla_len = (uint16_t)(NLA_HDRLEN + 4);
    int32_t mbm = -500;
    memcpy(msg + off + NLA_HDRLEN, &mbm, sizeof(mbm));
    off += NLA_ALIGN(a->nla_len);
    h->nlmsg_len = (uint32_t)off;

    int32_t out = 0;
    OK(nl80211__parse_txpower(msg, off, &out) == 0,
       "parse_txpower succeeds for negative mBm");
    OK(out == -500, "parse_txpower returns -500 mBm (signed)");
}

static void test_parse_txpower_zero_is_valid(void)
{
    /* 0 mBm is a valid (if unusual) value; must not be treated as
     * an error. */
    unsigned char msg[64];
    memset(msg, 0, sizeof(msg));
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_type = 0x1a;
    size_t off = NLMSG_HDRLEN + GENL_HDRLEN;
    struct nlattr *a = (struct nlattr *)(msg + off);
    a->nla_type = NL80211_ATTR_WIPHY_TX_POWER_LEVEL;
    a->nla_len = (uint16_t)(NLA_HDRLEN + 4);
    int32_t mbm = 0;
    memcpy(msg + off + NLA_HDRLEN, &mbm, sizeof(mbm));
    off += NLA_ALIGN(a->nla_len);
    h->nlmsg_len = (uint32_t)off;

    int32_t out = -1;
    OK(nl80211__parse_txpower(msg, off, &out) == 0,
       "parse_txpower succeeds for 0 mBm");
    OK(out == 0, "parse_txpower returns 0 mBm");
}

static void test_parse_txpower_missing(void)
{
    unsigned char msg[64];
    memset(msg, 0, sizeof(msg));
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_type = 0x1a;
    h->nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN;
    int32_t out = -1;
    OK(nl80211__parse_txpower(msg, h->nlmsg_len, &out) < 0,
       "parse_txpower fails when attr missing");
    OK(errno == ENOENT, "parse_txpower missing sets ENOENT");
}

/* ---- parse_alpha2 ---- */

static void test_parse_alpha2(void)
{
    unsigned char msg[64];
    memset(msg, 0, sizeof(msg));
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_type = 0x1a;
    size_t off = NLMSG_HDRLEN + GENL_HDRLEN;
    struct nlattr *a = (struct nlattr *)(msg + off);
    a->nla_type = NL80211_ATTR_REG_ALPHA2;
    a->nla_len = (uint16_t)(NLA_HDRLEN + 2);
    memcpy(msg + off + NLA_HDRLEN, "DE", 2);
    off += NLA_ALIGN(a->nla_len);
    h->nlmsg_len = (uint32_t)off;

    char out[4] = {0};
    OK(nl80211__parse_alpha2(msg, off, out, sizeof(out)) == 0,
       "parse_alpha2 succeeds on NEW_REG_RULE reply");
    OK(strcmp(out, "DE") == 0, "parse_alpha2 returns \"DE\"");
}

static void test_parse_alpha2_with_nul(void)
{
    /* Some kernel versions send a 3-byte NUL-terminated alpha2. The
     * parser must accept both forms. */
    unsigned char msg[64];
    memset(msg, 0, sizeof(msg));
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_type = 0x1a;
    size_t off = NLMSG_HDRLEN + GENL_HDRLEN;
    struct nlattr *a = (struct nlattr *)(msg + off);
    a->nla_type = NL80211_ATTR_REG_ALPHA2;
    a->nla_len = (uint16_t)(NLA_HDRLEN + 3);
    memcpy(msg + off + NLA_HDRLEN, "US\0", 3);
    off += NLA_ALIGN(a->nla_len);
    h->nlmsg_len = (uint32_t)off;

    char out[4] = {0};
    OK(nl80211__parse_alpha2(msg, off, out, sizeof(out)) == 0,
       "parse_alpha2 accepts 3-byte NUL-terminated form");
    OK(strcmp(out, "US") == 0, "parse_alpha2 returns \"US\"");
}

static void test_parse_alpha2_missing(void)
{
    unsigned char msg[64];
    memset(msg, 0, sizeof(msg));
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_type = 0x1a;
    h->nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN;
    char out[4] = {0};
    OK(nl80211__parse_alpha2(msg, h->nlmsg_len, out, sizeof(out)) < 0,
       "parse_alpha2 fails when attr missing");
    OK(errno == ENOENT, "parse_alpha2 missing sets ENOENT");
}

/* ---- parse_ack ---- */

static void test_parse_ack_ok(void)
{
    unsigned char msg[32];
    memset(msg, 0, sizeof(msg));
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_type = NLMSG_ERROR;
    h->nlmsg_len = NLMSG_HDRLEN + 4;
    int32_t zero = 0;
    memcpy(msg + NLMSG_HDRLEN, &zero, sizeof(zero));
    OK(nl80211__parse_ack(msg, h->nlmsg_len) == 0,
       "parse_ack returns 0 for error==0 (ack)");
}

static void test_parse_ack_error(void)
{
    unsigned char msg[32];
    memset(msg, 0, sizeof(msg));
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_type = NLMSG_ERROR;
    h->nlmsg_len = NLMSG_HDRLEN + 4;
    int32_t eperm = EPERM;
    memcpy(msg + NLMSG_HDRLEN, &eperm, sizeof(eperm));
    errno = 0;
    OK(nl80211__parse_ack(msg, h->nlmsg_len) < 0,
       "parse_ack returns -1 for error==EPERM");
    OK(errno == EPERM, "parse_ack propagates EPERM");
}

static void test_parse_ack_not_error(void)
{
    unsigned char msg[32];
    memset(msg, 0, sizeof(msg));
    struct nlmsghdr *h = (struct nlmsghdr *)msg;
    h->nlmsg_type = 0x1a;  /* not NLMSG_ERROR */
    h->nlmsg_len = NLMSG_HDRLEN + 4;
    errno = 0;
    OK(nl80211__parse_ack(msg, h->nlmsg_len) < 0,
       "parse_ack returns -1 on non-error message");
    OK(errno == EINVAL, "parse_ack non-error sets EINVAL");
}

/* ---- public API: zenctl_wireless_set_regdomain input validation ---- */

static void test_set_regdomain_bad_input(void)
{
    mock_sysfs_create_dir("sys/class/ieee80211/phy0");
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_wireless_t *wl = zenctl_wireless_open("phy0", &err);
    OK(wl != NULL, "wireless_open(\"phy0\") against mock tree");

    memset(&err, 0, sizeof(err));
    OK(zenctl_wireless_set_regdomain(wl, "USA", &err) == -1,
       "set_regdomain(\"USA\") rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "set_regdomain(\"USA\") sets EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_wireless_set_regdomain(wl, "X", &err) == -1,
       "set_regdomain(\"X\") rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "set_regdomain(\"X\") sets EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_wireless_set_regdomain(wl, "12", &err) == -1,
       "set_regdomain(\"12\") rejected (non-alpha)");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "set_regdomain(\"12\") sets EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_wireless_set_regdomain(wl, NULL, &err) == -1,
       "set_regdomain(NULL) rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "set_regdomain(NULL) sets EINVAL");

    zenctl_wireless_close(wl);
}

/* ---- public API: get_power_save returns ENOTSUP ---- */

static void test_get_power_save_unsupported(void)
{
    mock_sysfs_create_dir("sys/class/ieee80211/phy0");
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_wireless_t *wl = zenctl_wireless_open("phy0", &err);
    OK(wl != NULL, "wireless_open(\"phy0\") for PS test");

    bool out = false;
    memset(&err, 0, sizeof(err));
    OK(zenctl_wireless_get_power_save(wl, &out, &err) == -1,
       "get_power_save returns -1");
    OK(err.code == ZENCTL_ERR_ENOTSUP,
       "get_power_save sets ENOTSUP");

    zenctl_wireless_close(wl);
}

/* ---- public API: set_power_save with no interface → ENOENT ----
 * The mock tree has phy0 but no /sys/class/net/<iface>/phy80211 link
 * to it, so nl80211_phy_to_ifindex should fail and the setter should
 * surface ENOENT. */

static void test_set_power_save_no_iface(void)
{
    mock_sysfs_create_dir("sys/class/ieee80211/phy0");
    /* No /sys/class/net/ entries — phy_to_ifindex returns -1. */
    mock_sysfs_create_dir("sys/class/net");

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_wireless_t *wl = zenctl_wireless_open("phy0", &err);
    OK(wl != NULL, "wireless_open(\"phy0\") for PS-set test");

    memset(&err, 0, sizeof(err));
    OK(zenctl_wireless_set_power_save(wl, true, &err) == -1,
       "set_power_save without interface returns -1");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "set_power_save (no iface) sets ENOENT");

    zenctl_wireless_close(wl);
}

/* ---- public API: NULL-handle guards ---- */

static void test_null_guards(void)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    char *s = NULL;
    OK(zenctl_wireless_get_regdomain(NULL, &s, &err) == -1,
       "get_regdomain(NULL) rejected");
    OK(err.code == ZENCTL_ERR_EINVAL, "get_regdomain(NULL) sets EINVAL");

    memset(&err, 0, sizeof(err));
    int32_t mbm = 0;
    OK(zenctl_wireless_get_txpower(NULL, &mbm, &err) == -1,
       "get_txpower(NULL) rejected");
    OK(err.code == ZENCTL_ERR_EINVAL, "get_txpower(NULL) sets EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_wireless_set_txpower(NULL, 1000, &err) == -1,
       "set_txpower(NULL) rejected");
    OK(err.code == ZENCTL_ERR_EINVAL, "set_txpower(NULL) sets EINVAL");

    memset(&err, 0, sizeof(err));
    OK(zenctl_wireless_set_power_save(NULL, true, &err) == -1,
       "set_power_save(NULL) rejected");
    OK(err.code == ZENCTL_ERR_EINVAL, "set_power_save(NULL) sets EINVAL");
}

/* ---- phy_to_ifindex against the mock tree ---- */

static void test_phy_to_ifindex(void)
{
    /* Build a mock /sys/class/net/wlan0 with phy80211 symlink to
     * ../../ieee80211/phy0 and an ifindex file. */
    mock_sysfs_create_dir("sys/class/ieee80211/phy0");
    mock_sysfs_create_dir("sys/class/net/wlan0");
    mock_sysfs_create_symlink("sys/class/net/wlan0/phy80211",
                              "../../ieee80211/phy0");
    mock_sysfs_create_file("sys/class/net/wlan0/ifindex", "3\n");

    int idx = nl80211_phy_to_ifindex("phy0");
    OK(idx == 3, "phy_to_ifindex(\"phy0\") returns 3 (wlan0's ifindex)");

    int missing = nl80211_phy_to_ifindex("phy99");
    OK(missing < 0, "phy_to_ifindex(\"phy99\") returns -1");
    OK(errno == ENOENT, "phy_to_ifindex(missing) sets ENOENT");
}

/* ---- suite entry ---- */

int test_wireless_suite(void)
{
    SUITE_START("Wireless domain (nl80211)");
    test_build_getfamily();
    test_build_getfamily_overflow();
    test_build_get_reg();
    test_build_get_reg_bad_fid();
    test_build_set_reg();
    test_build_set_reg_bad_alpha2();
    test_build_get_wiphy();
    test_build_get_wiphy_bad_idx();
    test_build_set_txpower_fixed();
    test_build_set_txpower_auto();
    test_build_set_txpower_bad_setting();
    test_build_set_ps();
    test_build_set_ps_bad_ifindex();
    test_parse_family_id();
    test_parse_family_id_missing();
    test_parse_txpower();
    test_parse_txpower_negative();
    test_parse_txpower_zero_is_valid();
    test_parse_txpower_missing();
    test_parse_alpha2();
    test_parse_alpha2_with_nul();
    test_parse_alpha2_missing();
    test_parse_ack_ok();
    test_parse_ack_error();
    test_parse_ack_not_error();
    test_set_regdomain_bad_input();
    test_get_power_save_unsupported();
    test_set_power_save_no_iface();
    test_null_guards();
    test_phy_to_ifindex();
    SUITE_END();
    return SUITE_FAILURES();
}
