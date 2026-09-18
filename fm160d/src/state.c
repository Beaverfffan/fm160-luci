/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * state.c - state cache, sysfs traffic counters and ubus push.
 *
 * Two things are deliberately kept out of the AT path here:
 *   - byte counters come from /sys/class/net/<if>/statistics (zero AT cost);
 *   - the UI only ever reads the cached snapshot; it can never trigger AT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "fm160d.h"

/*
 * Monotonic milliseconds - the single clock behind every deadline in this
 * daemon (poll jitter, backoff, quiet windows, cache ages).  It deliberately
 * is not the wall clock: an NTP step or a manual `date` would otherwise make a
 * timer fire instantly or hang for hours.  libubox exposes its timers but no
 * public clock helper, so this is a two-line wrapper over CLOCK_MONOTONIC.
 */
uint64_t fm160_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

static bool dirty = true;
static uint64_t netdev_last_ms;
static const char *netdev_candidates[] = { "wwan0", "usb0", "wwan1", NULL };

static int read_u64_file(const char *path, uint64_t *out)
{
	FILE *f = fopen(path, "r");
	char buf[64];

	if (!f)
		return -1;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	*out = strtoull(buf, NULL, 10);
	return 0;
}

static bool netdev_exists(const char *name)
{
	char path[128];

	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes", name);
	return access(path, R_OK) == 0;
}

void fm160_netdev_poll(void)
{
	char path[160];
	uint64_t now = fm160_now_ms(), dt;
	uint64_t rx, tx;

	if (!g_state.netdev[0]) {
		const char **p;

		for (p = netdev_candidates; *p; p++) {
			if (netdev_exists(*p)) {
				snprintf(g_state.netdev, sizeof(g_state.netdev),
					 "%s", *p);
				fm160_log(LOG_INFO, "traffic counters from %s",
					  g_state.netdev);
				break;
			}
		}
		if (!g_state.netdev[0])
			return;
	}

	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes",
		 g_state.netdev);
	if (read_u64_file(path, &rx))
		return;
	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_bytes",
		 g_state.netdev);
	if (read_u64_file(path, &tx))
		return;

	if (!g_state.rx_bytes_prev && !g_state.tx_bytes_prev) {
		g_state.rx_bytes_prev = rx;
		g_state.tx_bytes_prev = tx;
		netdev_last_ms = now;
		return;
	}

	dt = now - netdev_last_ms;
	if (dt >= 500) {
		uint64_t drx = rx >= g_state.rx_bytes_prev ?
			       rx - g_state.rx_bytes_prev : 0;
		uint64_t dtx = tx >= g_state.tx_bytes_prev ?
			       tx - g_state.tx_bytes_prev : 0;

		g_state.rx_bps = drx * 1000 / dt;
		g_state.tx_bps = dtx * 1000 / dt;
		g_state.rx_bytes_prev = rx;
		g_state.tx_bytes_prev = tx;
		netdev_last_ms = now;
		dirty = true;
	}
	g_state.rx_bytes = rx;
	g_state.tx_bytes = tx;
}

void fm160_state_init(void)
{
	memset(&g_state, 0, sizeof(g_state));
	g_state.usb_mode = -1;
	g_state.creg = FM160_REG_UNKNOWN;
	g_state.cgreg = FM160_REG_UNKNOWN;
	g_state.cereg = FM160_REG_UNKNOWN;
	g_state.c5greg = FM160_REG_UNKNOWN;
	g_state.csq_rssi = 99;
	g_state.csq_ber = 99;
	g_state.cesq_rsrp = 255;
	g_state.cesq_rsrq = 255;
	g_state.tier_scale = 1;
	g_state.enabled = true;
	dirty = true;
}

void fm160_state_touch_ok(void)
{
	g_state.last_ok_ms = fm160_now_ms();
}

void fm160_state_mark_dirty(void)
{
	dirty = true;
}

/* ------------------------------------------------------------------ */
/* serialisation                                                        */
/* ------------------------------------------------------------------ */

/*
 * One cell row.  Raw values and decoded values are both exported: the raw ones
 * are what the modem said (useful when a decode rule turns out to be wrong on
 * real hardware), the decoded ones are what the UI shows.
 *
 * Note that blobmsg stores INT32 as a signed 32 bit value on the wire - libubox
 * formats it with PRId32 - so negative dBm values survive the trip to LuCI.
 */
static void cell_to_blob(struct blob_buf *b, const char *name,
			 const struct fm160_cell *c)
{
	void *t = blobmsg_open_table(b, name);

	blobmsg_add_u8(b, "valid", c->valid);
	blobmsg_add_u8(b, "is_service", c->is_service);
	blobmsg_add_u32(b, "rat", c->rat);
	blobmsg_add_u32(b, "mcc", c->mcc);
	blobmsg_add_u32(b, "mnc", c->mnc);
	blobmsg_add_u64(b, "tac", c->tac);
	blobmsg_add_u64(b, "cellid", c->cellid);
	blobmsg_add_u64(b, "earfcn", c->earfcn);
	blobmsg_add_u32(b, "pci", c->pci);
	blobmsg_add_u32(b, "band", c->band);           /* 0 = not reported */
	blobmsg_add_u32(b, "bandwidth", c->bandwidth_mhz);
	blobmsg_add_u32(b, "sinr_db10", c->sinr_db10);
	blobmsg_add_u32(b, "rsrp_dbm", c->rsrp_dbm);
	blobmsg_add_u32(b, "rsrq_db10", c->rsrq_db10);
	blobmsg_add_u32(b, "rxlev_raw", c->rxlev_raw);
	/* raw, for diagnostics */
	blobmsg_add_u32(b, "raw_sinr", c->sinr_raw);
	blobmsg_add_u32(b, "raw_rsrp", c->rsrp_raw);
	blobmsg_add_u32(b, "raw_rsrq", c->rsrq_raw);
	blobmsg_add_u32(b, "raw_bandwidth", c->bandwidth_raw);

	blobmsg_close_table(b, t);
}

/*
 * --- M4 helpers ---------------------------------------------------------
 *
 * A band is three integers, so the band lists are rendered as arrays of
 * tables rather than as strings: LuCI then gets the decoded value without
 * having to re-implement the encoding, which differs per RAT and which
 * fm160_band_decode() already owns.  The raw token travels alongside because
 * the raw form - not the decoded one - is what has to be written back.
 */
static void band_to_blob(struct blob_buf *b, const struct fm160_band *bd)
{
	void *t = blobmsg_open_table(b, NULL);

	blobmsg_add_u32(b, "raw", bd->raw);
	blobmsg_add_u32(b, "band", bd->band);
	blobmsg_add_u32(b, "rat", bd->rat);
	blobmsg_close_table(b, t);
}

static void band_array_to_blob(struct blob_buf *b, const char *name,
			       const struct fm160_band *v, int n)
{
	void *a = blobmsg_open_array(b, name);
	int i;

	for (i = 0; i < n; i++)
		band_to_blob(b, &v[i]);
	blobmsg_close_array(b, a);
}

/* Plain integer list, used for the rat/pref1/pref2 capability groups. */
static void int_array_to_blob(struct blob_buf *b, const char *name,
			      const int *v, int n)
{
	void *a = blobmsg_open_array(b, name);
	int i;

	for (i = 0; i < n; i++)
		blobmsg_add_u32(b, NULL, (uint32_t)v[i]);
	blobmsg_close_array(b, a);
}

/*
 * One carrier-aggregation row.  dl_mod/ul_mod are exported as the raw code
 * (0 BPSK .. 5 1024QAM, 6 unknown) so that a UI which has not been updated
 * still shows something honest.
 */
static void ca_cell_to_blob(struct blob_buf *b, const struct fm160_ca_cell *c)
{
	void *t = blobmsg_open_table(b, NULL);

	blobmsg_add_u8(b, "valid", c->valid);
	blobmsg_add_u8(b, "is_pcc", c->is_pcc);
	blobmsg_add_u32(b, "state", c->state);
	blobmsg_add_u32(b, "band", c->band);
	blobmsg_add_u32(b, "pci", c->pci);
	blobmsg_add_u64(b, "freq", c->freq);
	blobmsg_add_u32(b, "dl_bw_mhz", c->dl_bw_mhz);
	blobmsg_add_u32(b, "ul_bw_mhz", c->ul_bw_mhz);
	blobmsg_add_u32(b, "dl_mimo", c->dl_mimo);
	blobmsg_add_u32(b, "ul_mimo", c->ul_mimo);
	blobmsg_add_u32(b, "dl_mod", c->dl_mod);
	blobmsg_add_u32(b, "ul_mod", c->ul_mod);
	blobmsg_add_u32(b, "rsrp_dbm", c->rsrp_dbm);
	blobmsg_close_table(b, t);
}

struct blob_buf *fm160_state_blob(void)
{
	static struct blob_buf b;
	struct blob_attr *cells;
	void *m4;
	int i, j;

	/* Reusable buffer: free the previous contents before refilling so that
	 * repeated calls do not leak.  blob_buf_free() resets head/buf/buflen,
	 * and blob_buf_init() only fills in .grow, so the two compose safely. */
	if (b.head)
		blob_buf_free(&b);
	blob_buf_init(&b, 0);

	/* --- port / health ------------------------------------------- */
	blobmsg_add_string(&b, "port", g_state.port_found ? g_state.port : "");
	blobmsg_add_u8(&b, "port_found", g_state.port_found);
	blobmsg_add_u8(&b, "enabled", g_state.enabled);
	blobmsg_add_u32(&b, "at_state", g_state.at_state);
	blobmsg_add_u32(&b, "consec_timeout", g_state.consec_timeout);
	blobmsg_add_u32(&b, "worst_response_ms", (uint32_t)g_state.at_busy_max_ms);
	blobmsg_add_u32(&b, "queue_depth", atq_depth());
	blobmsg_add_u8(&b, "foreground", g_state.foreground);
	blobmsg_add_u8(&b, "quiet", atq_quiet_active());
	if (atq_quiet_active()) {
		blobmsg_add_string(&b, "quiet_reason", g_state.quiet_reason);
		blobmsg_add_u32(&b, "quiet_left_ms",
				(uint32_t)(g_state.quiet_until_ms - fm160_now_ms()));
	}
	if (g_state.last_ok_ms)
		blobmsg_add_u32(&b, "last_ok_age_ms",
				(uint32_t)(fm160_now_ms() - g_state.last_ok_ms));

	/* --- identity ------------------------------------------------- */
	blobmsg_add_string(&b, "manufacturer", g_state.manufacturer);
	blobmsg_add_string(&b, "model", g_state.model);
	blobmsg_add_string(&b, "revision", g_state.revision);
	blobmsg_add_string(&b, "imei", g_state.imei);
	blobmsg_add_string(&b, "sn", g_state.sn);
	blobmsg_add_string(&b, "iccid", g_state.iccid);
	blobmsg_add_u8(&b, "ident_done", g_state.ident_done);
	if (g_state.usb_mode >= 0)
		blobmsg_add_u32(&b, "usb_mode", g_state.usb_mode);

	/* --- sim / registration --------------------------------------- */
	blobmsg_add_string(&b, "pin_status", g_state.pin_status);
	blobmsg_add_u32(&b, "cereg", g_state.cereg);
	blobmsg_add_u32(&b, "c5greg", g_state.c5greg);
	blobmsg_add_string(&b, "operator", g_state.oper);

	/* --- signal --------------------------------------------------- */
	{
		void *s = blobmsg_open_table(&b, "csq");

		blobmsg_add_u32(&b, "raw_rssi", g_state.csq_rssi);
		blobmsg_add_u32(&b, "raw_ber", g_state.csq_ber);
		blobmsg_add_u8(&b, "is_ss_rsrp", g_state.csq_is_ss_rsrp);
		/* Only meaningful while the field really is an RSSI. */
		if (!g_state.csq_is_ss_rsrp && g_state.csq_rssi <= 31)
			blobmsg_add_u32(&b, "rssi_dbm",
					(uint32_t)(-113 + 2 * g_state.csq_rssi));
		blobmsg_close_table(&b, s);
	}

	{
		void *c = blobmsg_open_table(&b, "cesq");

		/* decoded */
		blobmsg_add_u32(&b, "geran_rssi_dbm", (uint32_t)g_state.geran_rssi_dbm);
		blobmsg_add_u32(&b, "utra_rscp_dbm", (uint32_t)g_state.utra_rscp_dbm);
		blobmsg_add_u32(&b, "utra_ecno_db10", (uint32_t)g_state.utra_ecno_db10);
		blobmsg_add_u32(&b, "lte_rsrp_dbm", (uint32_t)g_state.lte_rsrp_dbm);
		blobmsg_add_u32(&b, "lte_rsrq_db10", (uint32_t)g_state.lte_rsrq_db10);
		blobmsg_add_u32(&b, "nr_ss_rsrp_dbm", (uint32_t)g_state.nr_ss_rsrp_dbm);
		blobmsg_add_u32(&b, "nr_ss_rsrq_db10", (uint32_t)g_state.nr_ss_rsrq_db10);
		blobmsg_add_u32(&b, "nr_ss_sinr_db10", (uint32_t)g_state.nr_ss_sinr_db10);
		/* raw, as reported by the modem (255 = not applicable) */
		blobmsg_add_u32(&b, "raw_rxlev", g_state.cesq_rxlev);
		blobmsg_add_u32(&b, "raw_ber", g_state.cesq_ber);
		blobmsg_add_u32(&b, "raw_rscp", g_state.cesq_rscp);
		blobmsg_add_u32(&b, "raw_ecno", g_state.cesq_ecno);
		blobmsg_add_u32(&b, "raw_rsrq", g_state.cesq_rsrq);
		blobmsg_add_u32(&b, "raw_rsrp", g_state.cesq_rsrp);
		blobmsg_add_u32(&b, "raw_ss_rsrq", g_state.cesq_ss_rsrq);
		blobmsg_add_u32(&b, "raw_ss_rsrp", g_state.cesq_ss_rsrp);
		blobmsg_add_u32(&b, "raw_ss_sinr", g_state.cesq_ss_sinr);
		blobmsg_close_table(&b, c);
	}

	/* --- cells ---------------------------------------------------- */
	/* serving is the headline cell (highest RAT).  In EN-DC the modem reports
	 * two service rows; the LTE anchor ends up in serving2. */
	blobmsg_add_u8(&b, "cell_valid", g_state.serving.valid);
	blobmsg_add_u8(&b, "cell2_valid", g_state.serving2.valid);
	cell_to_blob(&b, "serving", &g_state.serving);
	cell_to_blob(&b, "serving2", &g_state.serving2);
	if (g_state.cell_last_ok_ms)
		blobmsg_add_u32(&b, "cell_age_ms",
				(uint32_t)(fm160_now_ms() - g_state.cell_last_ok_ms));

	cells = blobmsg_open_array(&b, "neighbours");
	for (i = 0; i < g_state.neigh_count; i++)
		cell_to_blob(&b, NULL, &g_state.neigh[i]);
	blobmsg_close_array(&b, cells);

	/* --- M4: band lock / cell lock / carrier aggregation ----------- */
	/*
	 * CONVENTION, and it is not the obvious one: blobmsg_open_table() /
	 * blobmsg_open_array() return a HANDLE that is only ever handed back to
	 * blobmsg_close_*().  Every value and every nested open still goes into the
	 * same struct blob_buf - &b - because that is where libubox tracks the
	 * current head.  Passing the handle instead (blobmsg_add_u32(&t, ...))
	 * compiles perfectly and then dereferences a void* as a blob_buf, which on
	 * this device was a SIGSEGV inside fm160_state_publish() every time the
	 * identity chain finished.
	 */
	m4 = blobmsg_open_table(&b, "m4");

	/* AT+GTACT? - the restriction actually in force right now. */
	{
		void *g = blobmsg_open_table(&b, "bands");

		blobmsg_add_u8(&b, "valid", g_state.gtact.valid);
		blobmsg_add_u32(&b, "rat", (uint32_t)g_state.gtact.rat);
		blobmsg_add_u32(&b, "pref1", (uint32_t)g_state.gtact.pref1);
		blobmsg_add_u32(&b, "pref2", (uint32_t)g_state.gtact.pref2);
		/* Bands the modem listed that match no documented encoding.
		 * Counted, never guessed - a non-zero value means the firmware
		 * is not the one the parser was written against. */
		blobmsg_add_u32(&b, "unknown", (uint32_t)g_state.gtact.unknown_n);
		blobmsg_add_u8(&b, "auto_seen", g_state.gtact.auto_seen);
		band_array_to_blob(&b, "umts", g_state.gtact.umts, g_state.gtact.umts_n);
		band_array_to_blob(&b, "lte", g_state.gtact.lte, g_state.gtact.lte_n);
		band_array_to_blob(&b, "nr", g_state.gtact.nr, g_state.gtact.nr_n);
		if (g_state.gtact.last_ok_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - g_state.gtact.last_ok_ms);
		blobmsg_close_table(&b, g);
	}

	/* AT+GTACT=? - what the modem says it supports.  This is the licence
	 * for the write path: an invalid caps set means the UI must refuse to
	 * offer band editing rather than let the user guess. */
	{
		void *g = blobmsg_open_table(&b, "band_caps");
		void *gn;

		blobmsg_add_u8(&b, "valid", g_state.gtact_caps.valid);
		int_array_to_blob(&b, "rat", g_state.gtact_caps.rat,
				  g_state.gtact_caps.rat_n);
		int_array_to_blob(&b, "pref1", g_state.gtact_caps.pref1,
				  g_state.gtact_caps.pref1_n);
		int_array_to_blob(&b, "pref2", g_state.gtact_caps.pref2,
				  g_state.gtact_caps.pref2_n);
		band_array_to_blob(&b, "umts", g_state.gtact_caps.umts,
				   g_state.gtact_caps.umts_n);
		band_array_to_blob(&b, "lte", g_state.gtact_caps.lte,
				   g_state.gtact_caps.lte_n);
		band_array_to_blob(&b, "nr", g_state.gtact_caps.nr,
				   g_state.gtact_caps.nr_n);
		/* Which of the nine groups the modem actually filled in.  On
		 * FM160-CN the gsm, cdma and evdo groups come back empty. */
		gn = blobmsg_open_array(&b, "group_nonempty");
		for (i = 0; i < FM160_GTACT_GROUPS; i++)
			blobmsg_add_u8(&b, NULL, g_state.gtact_caps.group_nonempty[i]);
		blobmsg_close_array(&b, gn);
		if (g_state.gtact_caps.last_ok_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - g_state.gtact_caps.last_ok_ms);
		blobmsg_close_table(&b, g);
	}

	/* AT+GTCELLLOCK? - persistent, and only takes effect after a UE reset
	 * that fm160d deliberately never performs itself. */
	{
		void *c = blobmsg_open_table(&b, "celllock");

		blobmsg_add_u8(&b, "valid", g_state.celllock.valid);
		blobmsg_add_u8(&b, "enabled", g_state.celllock.enabled);
		blobmsg_add_u32(&b, "rat", (uint32_t)g_state.celllock.rat);
		blobmsg_add_u32(&b, "type", (uint32_t)g_state.celllock.type);
		blobmsg_add_u64(&b, "earfcn", g_state.celllock.earfcn);
		blobmsg_add_u32(&b, "pci", (uint32_t)g_state.celllock.pci);
		blobmsg_add_u32(&b, "scs", (uint32_t)g_state.celllock.scs);
		blobmsg_add_u32(&b, "nrband", (uint32_t)g_state.celllock.nrband);
		blobmsg_add_u8(&b, "has_pci", g_state.celllock.has_pci);
		blobmsg_add_u8(&b, "has_scs", g_state.celllock.has_scs);
		blobmsg_add_u8(&b, "has_nrband", g_state.celllock.has_nrband);
		if (g_state.celllock.last_ok_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - g_state.celllock.last_ok_ms);
		blobmsg_close_table(&b, c);
	}

	/* AT+GTCELLLOCK=? - ranges, not enumerations. */
	{
		void *c = blobmsg_open_table(&b, "celllock_caps");
		void *a;
		bool undocumented = false;

		blobmsg_add_u8(&b, "valid", g_state.celllock_caps.valid);
		a = blobmsg_open_array(&b, "mode");
		for (i = 0; i < g_state.celllock_caps.mode_n; i++) {
			blobmsg_add_u32(&b, NULL,
					(uint32_t)g_state.celllock_caps.mode[i]);
			/* The live modem offers a third mode value that the
			 * manual does not define.  Recorded as evidence only;
			 * fm160_celllock_command() refuses to write anything
			 * other than 0 or 1. */
			if (g_state.celllock_caps.mode[i] > 1)
				undocumented = true;
		}
		blobmsg_close_array(&b, a);
		blobmsg_add_u8(&b, "mode_undocumented", undocumented);
		blobmsg_add_u32(&b, "rat_min", (uint32_t)g_state.celllock_caps.rat_min);
		blobmsg_add_u32(&b, "rat_max", (uint32_t)g_state.celllock_caps.rat_max);
		a = blobmsg_open_array(&b, "type");
		for (i = 0; i < g_state.celllock_caps.type_n; i++)
			blobmsg_add_u32(&b, NULL,
					(uint32_t)g_state.celllock_caps.type[i]);
		blobmsg_close_array(&b, a);
		/* u64, not u32: blobmsg formats an INT32 as a SIGNED int32, so
		 * this value - the top of the range the modem advertises - comes
		 * back as -1 on the wire.  It really is 4294967295. */
		blobmsg_add_u64(&b, "earfcn_max", g_state.celllock_caps.earfcn_max);
		blobmsg_add_u32(&b, "pci_max", (uint32_t)g_state.celllock_caps.pci_max);
		blobmsg_add_u32(&b, "scs_min", (uint32_t)g_state.celllock_caps.scs_min);
		blobmsg_add_u32(&b, "scs_max", (uint32_t)g_state.celllock_caps.scs_max);
		blobmsg_add_u32(&b, "nrband_min", (uint32_t)g_state.celllock_caps.nrband_min);
		blobmsg_add_u32(&b, "nrband_max", (uint32_t)g_state.celllock_caps.nrband_max);
		if (g_state.celllock_caps.last_ok_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - g_state.celllock_caps.last_ok_ms);
		blobmsg_close_table(&b, c);
	}

	/* AT+GTCAINFO? - "valid" is false both when the modem is idle (a bare
	 * OK, measured with no SIM) and when the read failed; ca_age_ms only
	 * appears once a PCC row has actually been seen. */
	{
		void *c = blobmsg_open_table(&b, "ca");
		void *a;

		blobmsg_add_u8(&b, "valid", g_state.ca.valid);
		blobmsg_add_u32(&b, "rat", (uint32_t)g_state.ca.rat);
		blobmsg_add_u8(&b, "has_nr", g_state.ca.has_nr);
		if (g_state.ca.valid) {
			void *p = blobmsg_open_table(&b, "pcc");

			blobmsg_add_u32(&b, "state", g_state.ca.pcc.state);
			blobmsg_add_u32(&b, "band", g_state.ca.pcc.band);
			blobmsg_add_u32(&b, "pci", g_state.ca.pcc.pci);
			blobmsg_add_u64(&b, "freq", g_state.ca.pcc.freq);
			blobmsg_add_u32(&b, "dl_bw_mhz", g_state.ca.pcc.dl_bw_mhz);
			blobmsg_add_u32(&b, "ul_bw_mhz", g_state.ca.pcc.ul_bw_mhz);
			blobmsg_add_u32(&b, "dl_mimo", g_state.ca.pcc.dl_mimo);
			blobmsg_add_u32(&b, "ul_mimo", g_state.ca.pcc.ul_mimo);
			blobmsg_add_u32(&b, "dl_mod", g_state.ca.pcc.dl_mod);
			blobmsg_add_u32(&b, "ul_mod", g_state.ca.pcc.ul_mod);
			blobmsg_add_u32(&b, "rsrp_dbm", g_state.ca.pcc.rsrp_dbm);
			blobmsg_close_table(&b, p);
		}
		a = blobmsg_open_array(&b, "scc");
		for (j = 0; j < g_state.ca.scc_n; j++)
			ca_cell_to_blob(&b, &g_state.ca.scc[j]);
		blobmsg_close_array(&b, a);
		if (g_state.ca.last_ok_ms)
			blobmsg_add_u64(&b, "age_ms",
					fm160_now_ms() - g_state.ca.last_ok_ms);
		blobmsg_close_table(&b, c);
	}

	blobmsg_close_table(&b, m4);

	/* --- traffic -------------------------------------------------- */
	{
		void *t = blobmsg_open_table(&b, "traffic");

		blobmsg_add_string(&b, "netdev", g_state.netdev);
		blobmsg_add_u64(&b, "rx_bytes", g_state.rx_bytes);
		blobmsg_add_u64(&b, "tx_bytes", g_state.tx_bytes);
		blobmsg_add_u64(&b, "rx_bps", g_state.rx_bps);
		blobmsg_add_u64(&b, "tx_bps", g_state.tx_bps);
		blobmsg_close_table(&b, t);
	}

	return &b;
}

void fm160_state_publish(void)
{
	struct blob_buf *b;

	if (!dirty || !g_ubus)
		return;
	b = fm160_state_blob();
	ubus_send_event(g_ubus, "fm160.state", b->head);
	blob_buf_free(b);
	dirty = false;
}
