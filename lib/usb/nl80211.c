/* nl80211.c - raw netlink (genetlink) implementation of the nl80211
 * controls used by the wireless domain: regdomain get/set, TX power
 * get/set, and 802.11 power-save set.
 *
 * No libnl dependency — we open an AF_NETLINK / SOCK_RAW /
 * NETLINK_GENERIC socket directly and walk attributes by hand. This
 * keeps libzenctl self-contained on any Linux with kernel headers.
 *
 * Layout of every message we exchange:
 *
 *   <struct nlmsghdr><struct genlmsghdr><nlattr...>
 *
 * The nlmsghdr.nlmsg_type is the genl family ID (or GENL_ID_CTRL for
 * controller messages). The genlmsghdr.cmd is the NL80211_CMD_*
 * (or CTRL_CMD_*). Attributes are TLV: {u16 nla_len, u16 nla_type,
 * payload, padding to NLA_ALIGNTO (4 bytes)}.
 *
 * Netlink payloads are host-endian on the wire; we memcpy into
 * uint32_t / int32_t fields without conversion. See
 * docs/KERNEL_USB_BT_FW.md section 3.2 for the kernel-side reference.
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE         /* sockaddr_nl in <linux/netlink.h> on some glibc */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>

#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>

#include "nl80211.h"

/* ---- constants ---- */

#define NL80211_FAMILY_NAME   "nl80211"
#define NL80211_RECVBUF_SIZE  (32 * 1024)

/* TX power setting sentinel exposed via the high-level API:
 * mBm == -1 means "automatic". */
#define NL80211_TXPOWER_AUTO_SENTINEL  ((int32_t)-1)

/* ---- message builder ---- *
 * A tiny in-buffer TLV appender. Tracks the write offset; the caller
 * finalizes by stamping nlmsghdr.nlmsg_len. */

struct nlbuf {
    char  *base;
    size_t cap;
    size_t off;   /* current write offset (start of next attr) */
};

/* Initialize an nlbuf to hold a genl message with the given family ID
 * and command. Lays down the nlmsghdr + genlmsghdr and positions the
 * write offset just past them. Returns 0 on success, -1 on overflow. */
static int nlbuf_init(struct nlbuf *b, void *buf, size_t sz,
                      int family_id, uint8_t cmd, uint16_t flags,
                      uint32_t seq)
{
    if (sz < NLMSG_HDRLEN + GENL_HDRLEN) {
        errno = ENOBUFS;
        return -1;
    }
    b->base = buf;
    b->cap  = sz;
    b->off  = 0;

    struct nlmsghdr *nlh = (struct nlmsghdr *)b->base;
    nlh->nlmsg_len   = 0;               /* finalized later */
    nlh->nlmsg_type  = (uint16_t)family_id;
    nlh->nlmsg_flags = flags;
    nlh->nlmsg_seq   = seq;
    nlh->nlmsg_pid   = 0;               /* kernel assigns */

    struct genlmsghdr *gh = (struct genlmsghdr *)((char *)nlh + NLMSG_HDRLEN);
    gh->cmd      = cmd;
    gh->version  = 1;
    gh->reserved = 0;

    b->off = NLMSG_HDRLEN + GENL_HDRLEN;
    return 0;
}

/* Finalize: stamp nlmsg_len and return the total message size. */
static ssize_t nlbuf_finalize(struct nlbuf *b)
{
    struct nlmsghdr *nlh = (struct nlmsghdr *)b->base;
    nlh->nlmsg_len = (uint32_t)b->off;
    return (ssize_t)b->off;
}

/* Append a raw TLV. Padding bytes are zeroed. */
static int nlbuf_put_attr(struct nlbuf *b, uint16_t type,
                          const void *data, size_t len)
{
    size_t total = (size_t)NLA_HDRLEN + NLA_ALIGN(len);
    if (b->off + total > b->cap) {
        errno = ENOBUFS;
        return -1;
    }
    struct nlattr *a = (struct nlattr *)(b->base + b->off);
    a->nla_len  = (uint16_t)(NLA_HDRLEN + len);
    a->nla_type = type;
    if (len)
        memcpy(b->base + b->off + NLA_HDRLEN, data, len);
    size_t pad = NLA_ALIGN(len) - len;
    if (pad)
        memset(b->base + b->off + NLA_HDRLEN + len, 0, pad);
    b->off += total;
    return 0;
}

static int nlbuf_put_u32(struct nlbuf *b, uint16_t type, uint32_t v)
{
    return nlbuf_put_attr(b, type, &v, sizeof(v));
}

static int nlbuf_put_s32(struct nlbuf *b, uint16_t type, int32_t v)
{
    return nlbuf_put_attr(b, type, &v, sizeof(v));
}

/* ---- attribute walking ---- */

struct nla_iter {
    const char *base;
    size_t      remaining;
    const struct nlattr *cur;
};

static void nla_iter_init(struct nla_iter *it, const void *attrs, size_t len)
{
    it->base      = (const char *)attrs;
    it->remaining = len;
    it->cur       = NULL;
}

/* Advance to the next attribute. Returns 1 on success, 0 at end. */
static int nla_iter_next(struct nla_iter *it)
{
    if (it->remaining < NLA_HDRLEN) return 0;
    it->cur = (const struct nlattr *)it->base;
    if (it->cur->nla_len < NLA_HDRLEN || (size_t)it->cur->nla_len > it->remaining)
        return 0;
    size_t aligned = NLA_ALIGN(it->cur->nla_len);
    if (aligned > it->remaining) aligned = it->remaining;
    it->base      += aligned;
    it->remaining -= aligned;
    return 1;
}

/* Find the first attribute matching `type`. Returns pointer to its
 * nlattr (in the buffer) or NULL. */
static const struct nlattr *nla_find(const void *attrs, size_t len, uint16_t type)
{
    struct nla_iter it;
    nla_iter_init(&it, attrs, len);
    while (nla_iter_next(&it)) {
        if (it.cur->nla_type == type)
            return it.cur;
    }
    return NULL;
}

/* ---- message construction helpers (exposed for testing) ---- */

ssize_t nl80211__build_getfamily(void *buf, size_t sz)
{
    if (!buf) { errno = EINVAL; return -1; }
    struct nlbuf b;
    if (nlbuf_init(&b, buf, sz, GENL_ID_CTRL, CTRL_CMD_GETFAMILY,
                   NLM_F_REQUEST, 1) != 0)
        return -1;
    /* CTRL_ATTR_FAMILY_NAME is NUL-terminated string per genl spec. */
    if (nlbuf_put_attr(&b, CTRL_ATTR_FAMILY_NAME,
                       NL80211_FAMILY_NAME,
                       strlen(NL80211_FAMILY_NAME) + 1) != 0)
        return -1;
    return nlbuf_finalize(&b);
}

ssize_t nl80211__build_get_reg(void *buf, size_t sz, int family_id)
{
    if (!buf || family_id <= 0) { errno = EINVAL; return -1; }
    struct nlbuf b;
    if (nlbuf_init(&b, buf, sz, family_id, NL80211_CMD_GET_REG,
                   NLM_F_REQUEST, 1) != 0)
        return -1;
    return nlbuf_finalize(&b);
}

ssize_t nl80211__build_set_reg(void *buf, size_t sz, int family_id,
                               const char *alpha2)
{
    if (!buf || !alpha2 || family_id <= 0) { errno = EINVAL; return -1; }
    if (strlen(alpha2) != 2) { errno = EINVAL; return -1; }
    struct nlbuf b;
    if (nlbuf_init(&b, buf, sz, family_id, NL80211_CMD_REQ_SET_REG,
                   NLM_F_REQUEST, 1) != 0)
        return -1;
    /* 2 bytes, no NUL — the kernel's __nl80211_set_reg copies exactly
     * sizeof(alpha2) == 2 bytes from nla_data. */
    if (nlbuf_put_attr(&b, NL80211_ATTR_REG_ALPHA2, alpha2, 2) != 0)
        return -1;
    return nlbuf_finalize(&b);
}

ssize_t nl80211__build_get_wiphy(void *buf, size_t sz, int family_id,
                                 int phy_idx)
{
    if (!buf || family_id <= 0 || phy_idx < 0) { errno = EINVAL; return -1; }
    struct nlbuf b;
    if (nlbuf_init(&b, buf, sz, family_id, NL80211_CMD_GET_WIPHY,
                   NLM_F_REQUEST, 1) != 0)
        return -1;
    if (nlbuf_put_u32(&b, NL80211_ATTR_WIPHY, (uint32_t)phy_idx) != 0)
        return -1;
    return nlbuf_finalize(&b);
}

ssize_t nl80211__build_set_txpower(void *buf, size_t sz, int family_id,
                                   int phy_idx, int setting, int32_t mBm)
{
    if (!buf || family_id <= 0 || phy_idx < 0) { errno = EINVAL; return -1; }
    if (setting < NL80211_TX_POWER_AUTOMATIC ||
        setting > NL80211_TX_POWER_FIXED) {
        errno = EINVAL; return -1;
    }
    struct nlbuf b;
    if (nlbuf_init(&b, buf, sz, family_id, NL80211_CMD_SET_WIPHY,
                   NLM_F_REQUEST, 1) != 0)
        return -1;
    if (nlbuf_put_u32(&b, NL80211_ATTR_WIPHY, (uint32_t)phy_idx) != 0)
        return -1;
    if (nlbuf_put_u32(&b, NL80211_ATTR_WIPHY_TX_POWER_SETTING,
                      (uint32_t)setting) != 0)
        return -1;
    if (setting != NL80211_TX_POWER_AUTOMATIC) {
        if (nlbuf_put_s32(&b, NL80211_ATTR_WIPHY_TX_POWER_LEVEL, mBm) != 0)
            return -1;
    }
    return nlbuf_finalize(&b);
}

ssize_t nl80211__build_set_ps(void *buf, size_t sz, int family_id,
                              int ifindex, bool enabled)
{
    if (!buf || family_id <= 0 || ifindex <= 0) { errno = EINVAL; return -1; }
    struct nlbuf b;
    if (nlbuf_init(&b, buf, sz, family_id, NL80211_CMD_SET_POWER_SAVE,
                   NLM_F_REQUEST, 1) != 0)
        return -1;
    if (nlbuf_put_u32(&b, NL80211_ATTR_IFINDEX, (uint32_t)ifindex) != 0)
        return -1;
    if (nlbuf_put_u32(&b, NL80211_ATTR_PS_STATE,
                      (uint32_t)(enabled ? NL80211_PS_ENABLED
                                         : NL80211_PS_DISABLED)) != 0)
        return -1;
    return nlbuf_finalize(&b);
}

/* ---- message parsing helpers ---- */

int nl80211__parse_ack(const void *buf, size_t sz)
{
    if (!buf || sz < NLMSG_HDRLEN) { errno = EINVAL; return -1; }
    const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;
    if (nlh->nlmsg_type != NLMSG_ERROR) { errno = EINVAL; return -1; }
    if (nlh->nlmsg_len < NLMSG_HDRLEN + sizeof(int32_t)) {
        errno = EBADMSG; return -1;
    }
    int32_t err;
    memcpy(&err, (const char *)nlh + NLMSG_HDRLEN, sizeof(err));
    if (err == 0) return 0;
    errno = (err > 0) ? err : EINVAL;
    return -1;
}

int nl80211__parse_family_id(const void *buf, size_t sz, int *out_id)
{
    if (!buf || !out_id || sz < NLMSG_HDRLEN + GENL_HDRLEN) {
        errno = EINVAL; return -1;
    }
    const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;
    if (nlh->nlmsg_type != GENL_ID_CTRL) { errno = EINVAL; return -1; }
    const char *attrs = (const char *)nlh + NLMSG_HDRLEN + GENL_HDRLEN;
    size_t attrs_len = (size_t)nlh->nlmsg_len - (NLMSG_HDRLEN + GENL_HDRLEN);
    const struct nlattr *a = nla_find(attrs, attrs_len, CTRL_ATTR_FAMILY_ID);
    if (!a || a->nla_len < NLA_HDRLEN + sizeof(uint16_t)) {
        errno = ENOENT; return -1;
    }
    uint16_t id;
    memcpy(&id, (const char *)a + NLA_HDRLEN, sizeof(id));
    *out_id = (int)id;
    return 0;
}

int nl80211__parse_txpower(const void *buf, size_t sz, int32_t *out_mBm)
{
    if (!buf || !out_mBm || sz < NLMSG_HDRLEN + GENL_HDRLEN) {
        errno = EINVAL; return -1;
    }
    const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;
    const char *attrs = (const char *)nlh + NLMSG_HDRLEN + GENL_HDRLEN;
    size_t attrs_len = (size_t)nlh->nlmsg_len - (NLMSG_HDRLEN + GENL_HDRLEN);
    const struct nlattr *a = nla_find(attrs, attrs_len,
                                      NL80211_ATTR_WIPHY_TX_POWER_LEVEL);
    if (!a || a->nla_len < NLA_HDRLEN + sizeof(int32_t)) {
        errno = ENOENT; return -1;
    }
    memcpy(out_mBm, (const char *)a + NLA_HDRLEN, sizeof(int32_t));
    return 0;
}

int nl80211__parse_alpha2(const void *buf, size_t sz,
                          char *out, size_t outsz)
{
    if (!buf || !out || sz < NLMSG_HDRLEN + GENL_HDRLEN) {
        errno = EINVAL; return -1;
    }
    if (outsz < 3) { errno = EINVAL; return -1; }
    const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;
    const char *attrs = (const char *)nlh + NLMSG_HDRLEN + GENL_HDRLEN;
    size_t attrs_len = (size_t)nlh->nlmsg_len - (NLMSG_HDRLEN + GENL_HDRLEN);
    const struct nlattr *a = nla_find(attrs, attrs_len,
                                      NL80211_ATTR_REG_ALPHA2);
    if (!a || a->nla_len < NLA_HDRLEN + 2) {
        errno = ENOENT; return -1;
    }
    /* alpha2 is 2 bytes; kernel may also send a NUL-terminated 3-byte
     * version. Copy up to 2 chars, force NUL. */
    size_t plen = (size_t)a->nla_len - NLA_HDRLEN;
    if (plen > 2) plen = 2;
    memcpy(out, (const char *)a + NLA_HDRLEN, plen);
    out[plen] = '\0';
    if (plen < 2) { errno = EBADMSG; return -1; }
    return 0;
}

/* ---- socket I/O ---- */

static int nl80211_socket_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (fd < 0) return -1;

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int nl80211_sendto(int fd, const void *buf, size_t len)
{
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    ssize_t n = sendto(fd, buf, len, 0,
                       (struct sockaddr *)&sa, sizeof(sa));
    if (n < 0 || (size_t)n != len) {
        if (n >= 0) errno = EIO;
        return -1;
    }
    return 0;
}

/* Receive one datagram into a heap-allocated 32KB buffer. Returns the
 * byte count on success, -1 on error. Caller frees *out_buf. */
static ssize_t nl80211_recv_all(int fd, char **out_buf)
{
    char *buf = malloc(NL80211_RECVBUF_SIZE);
    if (!buf) { errno = ENOMEM; return -1; }
    struct sockaddr_nl sa;
    socklen_t slen = sizeof(sa);
    ssize_t n = recvfrom(fd, buf, NL80211_RECVBUF_SIZE, 0,
                         (struct sockaddr *)&sa, &slen);
    if (n < 0) {
        int saved = errno;
        free(buf);
        errno = saved;
        return -1;
    }
    *out_buf = buf;
    return n;
}

/* Send `req` (a complete nl80211 request message) and wait for the
 * single ack reply (NLMSG_ERROR with error==0). Returns 0 on ack,
 * -1 on error (errno set, possibly from the kernel's error code). */
static int nl80211_do_ack(const void *req, size_t req_len)
{
    int fd = nl80211_socket_open();
    if (fd < 0) return -1;
    int rc = -1;
    char *reply = NULL;

    if (nl80211_sendto(fd, req, req_len) != 0) goto out;
    ssize_t n = nl80211_recv_all(fd, &reply);
    if (n < 0) goto out;
    /* Walk all messages in the reply; the ack is the NLMSG_ERROR one.
     * Multipart replies are terminated by NLMSG_DONE which is itself
     * type NLMSG_ERROR-shaped (carries a 0 error code), so the same
     * parse works for both. */
    const struct nlmsghdr *nlh = (const struct nlmsghdr *)reply;
    ssize_t remaining = n;
    int saw_err = 0;
    while (remaining >= (ssize_t)NLMSG_HDRLEN) {
        if (nlh->nlmsg_len < NLMSG_HDRLEN ||
            (ssize_t)nlh->nlmsg_len > remaining)
            break;
        if (nlh->nlmsg_type == NLMSG_ERROR) {
            saw_err = 1;
            int r = nl80211__parse_ack(nlh, (size_t)nlh->nlmsg_len);
            if (r != 0) goto out;   /* errno set by parse_ack */
            /* Done means end-of-multipart; ERROR with code 0 means ack.
             * For ack-style requests the kernel replies with exactly
             * one of these. */
            rc = 0;
            goto out;
        }
        remaining -= NLMSG_ALIGN(nlh->nlmsg_len);
        nlh = (const struct nlmsghdr *)
              ((const char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    }
    if (!saw_err) {
        /* No explicit ack and no error. Some commands (e.g. GET_REG
         * with a multipart reply) ack with NLMSG_DONE at the end;
         * callers that want a payload should use nl80211_do_query
         * instead. Treat absence of NLMSG_ERROR as success only if
         * we saw at least one message. */
        errno = EIO;
        goto out;
    }
out:
    if (reply) free(reply);
    close(fd);
    return rc;
}

/* Send `req` and collect the reply payload. Calls `cb` for every
 * genl message in the reply (multipart included). `cb` returns 0 to
 * keep walking, non-zero to stop (success). Returns 0 if `cb` stopped
 * with success, -1 otherwise (errno set). */
static int nl80211_do_query(const void *req, size_t req_len,
                            int (*cb)(const struct nlmsghdr *nlh,
                                      size_t len, void *user),
                            void *user)
{
    int fd = nl80211_socket_open();
    if (fd < 0) return -1;
    int rc = -1;
    char *reply = NULL;

    if (nl80211_sendto(fd, req, req_len) != 0) goto out;
    /* Loop until we see NLMSG_DONE or the callback signals success.
     * For non-dump queries the kernel sends a single datagram; for
     * dump queries it may send several. */
    for (;;) {
        ssize_t n = nl80211_recv_all(fd, &reply);
        if (n < 0) goto out;
        const struct nlmsghdr *nlh = (const struct nlmsghdr *)reply;
        ssize_t remaining = n;
        int done = 0;
        int multi = 0;
        while (remaining >= (ssize_t)NLMSG_HDRLEN) {
            if (nlh->nlmsg_len < NLMSG_HDRLEN ||
                (ssize_t)nlh->nlmsg_len > remaining)
                break;
            if (nlh->nlmsg_type == NLMSG_ERROR) {
                int r = nl80211__parse_ack(nlh, (size_t)nlh->nlmsg_len);
                if (r != 0) goto out;
                /* error==0 ack: end of dump. */
                done = 1;
                break;
            }
            if (nlh->nlmsg_type == NLMSG_DONE) {
                done = 1;
                break;
            }
            if (nlh->nlmsg_flags & NLM_F_MULTI) multi = 1;
            int r = cb(nlh, (size_t)nlh->nlmsg_len, user);
            if (r > 0) { rc = 0; goto out; }
            if (r < 0) goto out;
            remaining -= NLMSG_ALIGN(nlh->nlmsg_len);
            nlh = (const struct nlmsghdr *)
                  ((const char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
        }
        free(reply); reply = NULL;
        if (done) break;
        if (!multi) break;
    }
    /* If we got here without the callback signaling success, treat as
     * "no matching data". */
    errno = ENOENT;
out:
    if (reply) free(reply);
    close(fd);
    return rc;
}

/* ---- family-ID resolution (with caching) ---- */

static int family_id_cached = -1;

int nl80211_get_family_id(void)
{
    if (family_id_cached >= 0) return family_id_cached;

    char req[128];
    ssize_t r = nl80211__build_getfamily(req, sizeof(req));
    if (r < 0) return -1;

    int fd = nl80211_socket_open();
    if (fd < 0) return -1;
    int rc = -1;
    char *reply = NULL;

    if (nl80211_sendto(fd, req, (size_t)r) != 0) goto out;
    ssize_t n = nl80211_recv_all(fd, &reply);
    if (n < 0) goto out;

    /* Walk messages: the genl controller replies with a single
     * CTRL_CMD_NEWFAMILY message. If the family is unknown we get
     * NLMSG_ERROR with ENOENT-ish code. */
    const struct nlmsghdr *nlh = (const struct nlmsghdr *)reply;
    ssize_t remaining = n;
    while (remaining >= (ssize_t)NLMSG_HDRLEN) {
        if (nlh->nlmsg_len < NLMSG_HDRLEN ||
            (ssize_t)nlh->nlmsg_len > remaining)
            break;
        if (nlh->nlmsg_type == NLMSG_ERROR) {
            nl80211__parse_ack(nlh, (size_t)nlh->nlmsg_len);
            goto out;   /* errno set */
        }
        int id = -1;
        if (nl80211__parse_family_id(nlh, (size_t)nlh->nlmsg_len, &id) == 0) {
            family_id_cached = id;
            rc = id;
            goto out;
        }
        remaining -= NLMSG_ALIGN(nlh->nlmsg_len);
        nlh = (const struct nlmsghdr *)
              ((const char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    }
    errno = ENOENT;
out:
    if (reply) free(reply);
    close(fd);
    return rc;
}

/* ---- regdomain ---- */

struct regdomain_ctx {
    char *out;
    size_t sz;
    int   found;
};

static int regdomain_cb(const struct nlmsghdr *nlh, size_t len, void *user)
{
    (void)len;
    struct regdomain_ctx *c = (struct regdomain_ctx *)user;
    if (nl80211__parse_alpha2(nlh, nlh->nlmsg_len, c->out, c->sz) == 0) {
        c->found = 1;
        return 1;   /* stop, success */
    }
    return 0;       /* keep walking */
}

int nl80211_get_regdomain(char *out_alpha2, size_t sz)
{
    if (!out_alpha2 || sz < 3) { errno = EINVAL; return -1; }
    int fid = nl80211_get_family_id();
    if (fid < 0) return -1;

    char req[64];
    ssize_t r = nl80211__build_get_reg(req, sizeof(req), fid);
    if (r < 0) return -1;

    struct regdomain_ctx c = { .out = out_alpha2, .sz = sz, .found = 0 };
    /* GET_REG may return a multipart reply (one NEW_REG_RULE per
     * band); the alpha2 is on the first message. */
    if (nl80211_do_query(req, (size_t)r, regdomain_cb, &c) != 0)
        return -1;
    if (!c.found) { errno = ENOENT; return -1; }
    return 0;
}

int nl80211_set_regdomain(const char *alpha2)
{
    if (!alpha2 || strlen(alpha2) != 2) { errno = EINVAL; return -1; }
    int fid = nl80211_get_family_id();
    if (fid < 0) return -1;

    char req[64];
    ssize_t r = nl80211__build_set_reg(req, sizeof(req), fid, alpha2);
    if (r < 0) return -1;
    return nl80211_do_ack(req, (size_t)r);
}

/* ---- TX power ---- */

struct txpower_ctx {
    int32_t *out;
    int      found;
};

static int txpower_cb(const struct nlmsghdr *nlh, size_t len, void *user)
{
    (void)len;
    struct txpower_ctx *c = (struct txpower_ctx *)user;
    if (nl80211__parse_txpower(nlh, nlh->nlmsg_len, c->out) == 0) {
        c->found = 1;
        return 1;
    }
    return 0;
}

int nl80211_get_txpower(int phy_idx, int32_t *out_mBm)
{
    if (!out_mBm || phy_idx < 0) { errno = EINVAL; return -1; }
    int fid = nl80211_get_family_id();
    if (fid < 0) return -1;

    char req[64];
    ssize_t r = nl80211__build_get_wiphy(req, sizeof(req), fid, phy_idx);
    if (r < 0) return -1;

    struct txpower_ctx c = { .out = out_mBm, .found = 0 };
    if (nl80211_do_query(req, (size_t)r, txpower_cb, &c) != 0)
        return -1;
    if (!c.found) { errno = ENOENT; return -1; }
    return 0;
}

int nl80211_set_txpower(int phy_idx, int32_t mBm)
{
    if (phy_idx < 0) { errno = EINVAL; return -1; }
    int fid = nl80211_get_family_id();
    if (fid < 0) return -1;

    int setting;
    int32_t level;
    if (mBm == NL80211_TXPOWER_AUTO_SENTINEL) {
        setting = NL80211_TX_POWER_AUTOMATIC;
        level   = 0;
    } else {
        setting = NL80211_TX_POWER_FIXED;
        level   = mBm;
    }

    char req[128];
    ssize_t r = nl80211__build_set_txpower(req, sizeof(req), fid,
                                           phy_idx, setting, level);
    if (r < 0) return -1;
    return nl80211_do_ack(req, (size_t)r);
}

/* ---- power save ---- */

int nl80211_set_power_save(int ifindex, bool enabled)
{
    if (ifindex <= 0) { errno = EINVAL; return -1; }
    int fid = nl80211_get_family_id();
    if (fid < 0) return -1;

    char req[64];
    ssize_t r = nl80211__build_set_ps(req, sizeof(req), fid, ifindex, enabled);
    if (r < 0) return -1;
    return nl80211_do_ack(req, (size_t)r);
}

/* ---- phy name -> ifindex (sysfs walk; no netlink needed) ---- */

int nl80211_phy_to_ifindex(const char *phy_name)
{
    if (!phy_name || !*phy_name) { errno = EINVAL; return -1; }

    DIR *d = opendir("/sys/class/net");
    if (!d) return -1;

    int ifindex = -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;

        /* Bounds-check d_name length: NAME_MAX is 255 but our paths
         * must fit in 256-byte stacks. Skip overlong names defensively. */
        size_t nl = strlen(de->d_name);
        if (nl > 200) continue;

        /* Read the phy80211 symlink target. */
        char link_path[256];
        snprintf(link_path, sizeof(link_path),
                 "/sys/class/net/%s/phy80211", de->d_name);
        char target[512];
        ssize_t tlen = readlink(link_path, target, sizeof(target) - 1);
        if (tlen < 0) continue;
        target[tlen] = '\0';

        /* The link target is like ../../ieee80211/phy0. Extract the
         * basename and compare. */
        const char *base = strrchr(target, '/');
        base = base ? base + 1 : target;
        if (strcmp(base, phy_name) != 0) continue;

        /* Read ifindex. */
        char idx_path[256];
        snprintf(idx_path, sizeof(idx_path),
                 "/sys/class/net/%s/ifindex", de->d_name);
        FILE *f = fopen(idx_path, "r");
        if (!f) continue;
        char buf[32];
        if (fgets(buf, sizeof(buf), f) != NULL) {
            char *end = NULL;
            long v = strtol(buf, &end, 10);
            if (end != buf && (*end == '\0' || *end == '\n') && v > 0)
                ifindex = (int)v;
        }
        fclose(f);
        break;
    }
    closedir(d);

    if (ifindex < 0) errno = ENOENT;
    return ifindex;
}
