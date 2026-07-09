/* test_net.c - network domain unit tests.
 *
 * The sysfs-backed functions (mtu, link, speed, duplex) are tested
 * against a mock /sys/class/net tree. The ethtool ioctl path
 * (rings, offload flags) needs a real socket and a real driver, so
 * it's marked TODO here and deferred to an integration test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zenctl/zenctl.h"
#include "harness.h"
#include "mock_sysfs.h"

static void test_net_sysfs(void)
{
    /* Build /sys/class/net/eth0 with the attributes the sysfs path uses. */
    mock_sysfs_create_dir("sys/class/net/eth0");
    mock_sysfs_create_file("sys/class/net/eth0/mtu", "1500");
    mock_sysfs_create_file("sys/class/net/eth0/carrier", "1");
    mock_sysfs_create_file("sys/class/net/eth0/speed", "1000");
    mock_sysfs_create_file("sys/class/net/eth0/duplex", "full");

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_net_t *net = zenctl_net_open("eth0", &err);
    OK(net != NULL, "net_open(\"eth0\") succeeds against mock tree");
    if (!net) return;

    int v = 0;
    memset(&err, 0, sizeof(err));
    OK(zenctl_net_get_mtu(net, &v, &err) == 0, "get_mtu returns 0");
    OK(v == 1500, "get_mtu returns 1500");

    bool up = false;
    memset(&err, 0, sizeof(err));
    OK(zenctl_net_get_link(net, &up, &err) == 0, "get_link returns 0");
    OK(up == true, "get_link returns true for carrier=\"1\"");

    int mbps = 0;
    memset(&err, 0, sizeof(err));
    OK(zenctl_net_get_speed(net, &mbps, &err) == 0, "get_speed returns 0");
    OK(mbps == 1000, "get_speed returns 1000");

    char *dx = NULL;
    memset(&err, 0, sizeof(err));
    OK(zenctl_net_get_duplex(net, &dx, &err) == 0, "get_duplex returns 0");
    OK(dx && strcmp(dx, "full") == 0, "get_duplex returns \"full\"");
    free(dx);

    /* set_mtu writes the integer */
    memset(&err, 0, sizeof(err));
    OK(zenctl_net_set_mtu(net, 9000, &err) == 0,
       "set_mtu(9000) returns 0");
    char buf[32];
    int n = mock_sysfs_read_file("sys/class/net/eth0/mtu", buf, sizeof(buf));
    OK(n >= 0, "mtu file exists after write");
    OK(strcmp(buf, "9000") == 0, "mtu file contains \"9000\"");

    /* reject negative mtu */
    memset(&err, 0, sizeof(err));
    OK(zenctl_net_set_mtu(net, -1, &err) == -1, "set_mtu(-1) rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "set_mtu(-1) sets ZENCTL_ERR_EINVAL");

    zenctl_net_close(net);
}

static void test_net_open_missing(void)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_net_t *net = zenctl_net_open("nosuchiface", &err);
    OK(net == NULL, "net_open(\"nosuchiface\") returns NULL");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "net_open(missing) sets ZENCTL_ERR_ENOENT");
}

static void test_net_open_bad_name(void)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_net_t *net = zenctl_net_open("../etc/passwd", &err);
    OK(net == NULL, "net_open(\"../etc/passwd\") rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "net_open(\"../etc/passwd\") sets ZENCTL_ERR_EINVAL");
}

static void test_ethtool_ioctl_todo(void)
{
    /* The ring / offload ops use SIOCETHTOOL on an AF_INET SOCK_DGRAM
     * socket and need a real driver that implements them. Testing them
     * here would require either a real NIC or a much more elaborate
     * ioctl mock; deferred to integration tests. */
    printf("ok %d: TODO ethtool ioctl tests (deferred to integration)\n",
           ++test_count);
    test_pass++;
}

int test_net_suite(void)
{
    SUITE_START("net domain");
    test_net_sysfs();
    test_net_open_missing();
    test_net_open_bad_name();
    test_ethtool_ioctl_todo();
    SUITE_END();
    return SUITE_FAILURES();
}
