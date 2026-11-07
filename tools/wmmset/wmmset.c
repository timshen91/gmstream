// wmmset IFACE AC TXOP_US CWMIN CWMAX AIFS: sets the access point's own EDCA
// parameters for one access category (vo, vi, be, bk) through nl80211
// (NL80211_CMD_SET_WIPHY with NL80211_ATTR_WIPHY_TXQ_PARAMS), what hostapd does
// at start. Clients keep the parameters the beacon advertises; this changes how
// the AP itself contends and how long it may hold the channel. Needs
// CAP_NET_ADMIN. TXOP is rounded down to 32 us units. Build: cc -O2 -o wmmset
// wmmset.c (no libraries). Why: DESIGN.md §14.
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { CMD_SET_WIPHY = 2, ATTR_IFINDEX = 3, ATTR_TXQ_PARAMS = 37 };
enum { TXQ_AC = 1, TXQ_TXOP = 2, TXQ_CWMIN = 3, TXQ_CWMAX = 4, TXQ_AIFS = 5 };

struct msg { struct nlmsghdr n; struct genlmsghdr g; char buf[32768]; };

static struct nlattr *put(struct msg *m, int type, const void *data, int len) {
	struct nlattr *a = (struct nlattr *)((char *)m + NLMSG_ALIGN(m->n.nlmsg_len));
	a->nla_type = type; a->nla_len = NLA_HDRLEN + len;
	if (len) memcpy((char *)a + NLA_HDRLEN, data, len);
	m->n.nlmsg_len = NLMSG_ALIGN(m->n.nlmsg_len) + NLA_ALIGN(a->nla_len);
	return a;
}
static void end(struct msg *m, struct nlattr *a) { a->nla_len = (char *)m + m->n.nlmsg_len - (char *)a; }

static int talk(int fd, struct msg *m, struct msg *reply) {
	if (send(fd, m, m->n.nlmsg_len, 0) < 0) { perror("send"); return -1; }
	int n = recv(fd, reply, sizeof *reply, 0);
	if (n > 0 && reply->n.nlmsg_len > (unsigned)n) reply->n.nlmsg_len = n;
	if (n < 0) { perror("recv"); return -1; }
	if (reply->n.nlmsg_type == NLMSG_ERROR) {
		int e = ((struct nlmsgerr *)NLMSG_DATA(&reply->n))->error;
		if (e) { fprintf(stderr, "netlink error %d (%s)\n", e, strerror(-e)); return -1; }
	}
	return 0;
}

int main(int argc, char **argv) {
	if (argc != 7) { fprintf(stderr, "usage: wmmset IFACE vo|vi|be|bk TXOP_US CWMIN CWMAX AIFS\n"); return 2; }
	const char *acs[] = { "vo", "vi", "be", "bk" };
	uint8_t ac = 255;
	for (int i = 0; i < 4; i++) if (!strcmp(argv[2], acs[i])) ac = i;
	if (ac == 255) { fprintf(stderr, "bad AC\n"); return 2; }
	uint32_t ifindex = if_nametoindex(argv[1]);
	uint16_t txop = atoi(argv[3]) / 32, cwmin = atoi(argv[4]), cwmax = atoi(argv[5]);
	uint8_t aifs = atoi(argv[6]);
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	bind(fd, (struct sockaddr *)&sa, sizeof sa);
	// Family id of nl80211.
	static struct msg m, r;
	m = (struct msg){ .n = { .nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN), .nlmsg_type = GENL_ID_CTRL, .nlmsg_flags = NLM_F_REQUEST },
	                 .g = { .cmd = CTRL_CMD_GETFAMILY, .version = 1 } };
	put(&m, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
	if (talk(fd, &m, &r)) return 1;
	uint16_t family = 0;
	for (struct nlattr *a = (struct nlattr *)((char *)&r.g + GENL_HDRLEN); (char *)a < (char *)&r + r.n.nlmsg_len;
	     a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len))) {
		if (a->nla_len < NLA_HDRLEN) break;
		if (a->nla_type == CTRL_ATTR_FAMILY_ID) memcpy(&family, (char *)a + NLA_HDRLEN, 2);
	}
	if (!family) { fprintf(stderr, "no nl80211\n"); return 1; }
	m = (struct msg){ .n = { .nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN), .nlmsg_type = family, .nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK },
	                  .g = { .cmd = CMD_SET_WIPHY } };
	put(&m, ATTR_IFINDEX, &ifindex, 4);
	struct nlattr *list = put(&m, ATTR_TXQ_PARAMS | NLA_F_NESTED, NULL, 0);
	struct nlattr *q = put(&m, 1 | NLA_F_NESTED, NULL, 0);
	put(&m, TXQ_AC, &ac, 1); put(&m, TXQ_TXOP, &txop, 2); put(&m, TXQ_CWMIN, &cwmin, 2);
	put(&m, TXQ_CWMAX, &cwmax, 2); put(&m, TXQ_AIFS, &aifs, 1);
	end(&m, q); end(&m, list);
	if (talk(fd, &m, &r)) return 1;
	printf("%s %s: txop %u us, cw %u-%u, aifs %u\n", argv[1], argv[2], txop * 32, cwmin, cwmax, aifs);
	return 0;
}
