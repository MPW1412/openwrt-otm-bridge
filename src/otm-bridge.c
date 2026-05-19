/*
 * otm-bridge: capture raw 802.11(p) frames from a Linux monitor interface
 * and publish them via MQTT to opentrafficmap.org's C-ITS ingest, mimicking
 * the wire format of opentrafficmap/its-g5-receiver-firmware (ESP32-C5).
 *
 * Topic layout (default broker mqtts://cits1.opentrafficmap.org):
 *   its/<node>/status   "online"/"offline" (LWT, retained)
 *   its/<node>/info     {"emac":"aa:bb:..","ver":"...","hwv":"..."}  (on connect)
 *   its/<node>/packet   raw 802.11 frame (radiotap stripped), QoS 0
 *
 * <node> defaults to the eth0 MAC as 12 lowercase hex chars.
 */

#include <errno.h>
#include <fcntl.h>
#include <pcap.h>
#include <mosquitto.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define OTM_BRIDGE_VERSION "0.6"

static struct mosquitto *mosq;
static pcap_t *pc;
static volatile sig_atomic_t running = 1;

static char topic_packet[160];
static char topic_status[160];
static char topic_info[160];

static int verbose;
static unsigned long pkt_published;
static unsigned long pkt_pcap_err;

static void on_signal(int s)
{
	(void)s;
	running = 0;
	if (pc)
		pcap_breakloop(pc);
}

static int read_mac_hex12_from(const char *path, char *out13)
{
	char line[32];
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
	fclose(f);

	int j = 0;
	for (int i = 0; line[i] && j < 12; i++) {
		char c = line[i];
		if (c == ':' || c == '-' || c == '\n' || c == '\r')
			continue;
		if (c >= 'A' && c <= 'F') c += 32;
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return -1;
		out13[j++] = c;
	}
	out13[12] = 0;
	return j == 12 ? 0 : -1;
}

/* Try a list of stable MAC sources. eth0 is intentionally last because on
 * lantiq/DSA setups it often gets a randomly-generated locally-administered
 * address that changes per boot, breaking the node identity across reboots. */
static int read_mac_hex12(const char *unused, char *out13)
{
	(void)unused;
	static const char *sources[] = {
		"/sys/class/ieee80211/phy0/macaddress",  /* AR9580, hardware-burned */
		"/sys/class/net/lan1/address",
		"/sys/class/net/br-lan/address",
		"/sys/class/net/eth0/address",
		NULL,
	};
	for (int i = 0; sources[i]; i++) {
		if (read_mac_hex12_from(sources[i], out13) == 0)
			return 0;
	}
	return -1;
}

static void format_mac_colon(const char *hex12, char out[18])
{
	snprintf(out, 18, "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
		 hex12[0], hex12[1], hex12[2], hex12[3], hex12[4], hex12[5],
		 hex12[6], hex12[7], hex12[8], hex12[9], hex12[10], hex12[11]);
}

static int parse_broker_uri(const char *uri, char host[128], int *port, int *tls)
{
	const char *p;
	if (!strncmp(uri, "mqtts://", 8)) { *tls = 1; *port = 8883; p = uri + 8; }
	else if (!strncmp(uri, "mqtt://", 7)) { *tls = 0; *port = 1883; p = uri + 7; }
	else return -1;

	const char *colon = strchr(p, ':');
	const char *slash = strchr(p, '/');
	const char *end   = colon ? colon : (slash ? slash : p + strlen(p));
	size_t hl = end - p;
	if (hl == 0 || hl >= 128) return -1;

	memcpy(host, p, hl);
	host[hl] = 0;
	if (colon) *port = atoi(colon + 1);
	return 0;
}

static void on_connect(struct mosquitto *m, void *ud, int rc)
{
	(void)ud;
	if (rc) {
		syslog(LOG_ERR, "mqtt connect rc=%d (%s)", rc, mosquitto_connack_string(rc));
		return;
	}
	syslog(LOG_INFO, "mqtt connected, topic=its/<node>/packet");

	mosquitto_publish(m, NULL, topic_status,
			  (int)strlen("online"), "online", 1, true);

	char info[256], hex12[13] = "000000000000", mac_colon[18];
	read_mac_hex12("eth0", hex12);
	format_mac_colon(hex12, mac_colon);
	int n = snprintf(info, sizeof(info),
		 "{\"emac\":\"%s\",\"ver\":\"otm-bridge-%s\",\"hwv\":\"FRITZ!Box 3390\"}",
		 mac_colon, OTM_BRIDGE_VERSION);
	mosquitto_publish(m, NULL, topic_info, n, info, 0, false);
}

static void on_disconnect(struct mosquitto *m, void *ud, int rc)
{
	(void)m; (void)ud;
	syslog(LOG_WARNING, "mqtt disconnected rc=%d", rc);
}

static void on_log(struct mosquitto *m, void *ud, int level, const char *str)
{
	(void)m; (void)ud;
	if (level <= MOSQ_LOG_WARNING || verbose >= 2)
		syslog(LOG_DEBUG, "mosq: %s", str);
}

static void pkt_handler(u_char *ud, const struct pcap_pkthdr *h, const u_char *bytes)
{
	(void)ud;
	if (!mosq) return;
	int r = mosquitto_publish(mosq, NULL, topic_packet,
				   (int)h->caplen, bytes, 0, false);
	if (r == MOSQ_ERR_SUCCESS) {
		pkt_published++;
		if (verbose && (pkt_published % 100 == 0))
			syslog(LOG_INFO, "published %lu packets", pkt_published);
	} else if (r == MOSQ_ERR_NO_CONN) {
		/* dropped while disconnected; mosquitto_loop will reconnect */
	} else {
		pkt_pcap_err++;
		if (pkt_pcap_err < 5 || pkt_pcap_err % 1000 == 0)
			syslog(LOG_WARNING, "publish err %d (%s) total=%lu",
			       r, mosquitto_strerror(r), pkt_pcap_err);
	}
}

static void usage(void)
{
	fprintf(stderr,
	    "usage: otm-bridge [options]\n"
	    "  -i IFACE   capture interface (default mon0)\n"
	    "  -b URI     broker URI (default mqtts://cits1.opentrafficmap.org)\n"
	    "  -n NODE    node id (default eth0 MAC as 12 hex chars)\n"
	    "  -c CAFILE  TLS CA file (default /etc/ssl/certs/ca-certificates.crt)\n"
	    "  -s SNAP    pcap snap length (default 2300)\n"
	    "  -v         verbose (repeat for more)\n"
	    "  -f         stay in foreground\n");
}

int main(int argc, char **argv)
{
	const char *iface       = "mon0";
	const char *broker_uri  = "mqtts://cits1.opentrafficmap.org";
	const char *cafile      = "/etc/ssl/certs/ca-certificates.crt";
	char node_id[64]        = { 0 };
	int snaplen             = 2300;
	int foreground          = 0;

	int opt;
	while ((opt = getopt(argc, argv, "i:b:n:c:s:vfh")) != -1) {
		switch (opt) {
		case 'i': iface       = optarg; break;
		case 'b': broker_uri  = optarg; break;
		case 'n': strncpy(node_id, optarg, sizeof(node_id) - 1); break;
		case 'c': cafile      = optarg; break;
		case 's': snaplen     = atoi(optarg); break;
		case 'v': verbose++;            break;
		case 'f': foreground  = 1;      break;
		case 'h':
		default:  usage(); return opt == 'h' ? 0 : 1;
		}
	}

	if (!node_id[0] && read_mac_hex12("eth0", node_id) != 0) {
		fprintf(stderr, "failed to derive node id from eth0 MAC; pass -n\n");
		return 1;
	}

	char host[128];
	int port = 8883, tls = 1;
	if (parse_broker_uri(broker_uri, host, &port, &tls) != 0) {
		fprintf(stderr, "bad broker URI: %s\n", broker_uri);
		return 1;
	}

	snprintf(topic_packet, sizeof(topic_packet), "its/%s/packet", node_id);
	snprintf(topic_status, sizeof(topic_status), "its/%s/status", node_id);
	snprintf(topic_info,   sizeof(topic_info),   "its/%s/info",   node_id);

	openlog("otm-bridge", LOG_PID | (foreground ? LOG_PERROR : 0), LOG_DAEMON);
	syslog(LOG_INFO, "starting v%s iface=%s broker=%s://%s:%d node=%s",
	       OTM_BRIDGE_VERSION, iface, tls ? "mqtts" : "mqtt",
	       host, port, node_id);

	char errbuf[PCAP_ERRBUF_SIZE];
	pc = pcap_open_live(iface, snaplen, 1, 100, errbuf);
	if (!pc) {
		syslog(LOG_ERR, "pcap_open_live(%s): %s", iface, errbuf);
		return 1;
	}
	int dlt = pcap_datalink(pc);
	if (dlt != DLT_IEEE802_11_RADIO)
		syslog(LOG_WARNING, "iface %s datalink is %s, not radiotap; "
		       "server may reject payloads", iface,
		       pcap_datalink_val_to_name(dlt));

	mosquitto_lib_init();
	mosq = mosquitto_new(node_id, true, NULL);
	if (!mosq) { syslog(LOG_ERR, "mosquitto_new failed"); return 1; }

	mosquitto_will_set(mosq, topic_status,
			   (int)strlen("offline"), "offline", 1, true);
	mosquitto_connect_callback_set(mosq, on_connect);
	mosquitto_disconnect_callback_set(mosq, on_disconnect);
	mosquitto_log_callback_set(mosq, on_log);

	if (tls && mosquitto_tls_set(mosq, cafile, NULL, NULL, NULL, NULL)
		   != MOSQ_ERR_SUCCESS) {
		syslog(LOG_ERR, "mosquitto_tls_set(%s) failed", cafile);
		return 1;
	}

	struct sigaction sa = { 0 };
	sa.sa_handler = on_signal;
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (!foreground && daemon(0, 0) < 0) {
		syslog(LOG_ERR, "daemon: %s", strerror(errno));
		return 1;
	}

	int r = mosquitto_connect_async(mosq, host, port, 60);
	if (r != MOSQ_ERR_SUCCESS) {
		syslog(LOG_ERR, "mosquitto_connect_async: %s",
		       mosquitto_strerror(r));
		return 1;
	}
	mosquitto_loop_start(mosq);

	pcap_loop(pc, -1, pkt_handler, NULL);

	syslog(LOG_INFO, "shutting down, published=%lu errs=%lu",
	       pkt_published, pkt_pcap_err);
	mosquitto_publish(mosq, NULL, topic_status,
			  (int)strlen("offline"), "offline", 1, true);
	struct timespec ts = { 1, 0 };
	nanosleep(&ts, NULL);
	mosquitto_disconnect(mosq);
	mosquitto_loop_stop(mosq, true);
	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();
	pcap_close(pc);
	closelog();
	return 0;
}
