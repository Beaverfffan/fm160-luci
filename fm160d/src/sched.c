/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sched.c - polling scheduler, quiet windows, circuit breaker, port discovery.
 *
 * The FM160 runs Qualcomm firmware whose AT parser is easily overwhelmed, so
 * every periodic exchange goes through one of the tiers defined below.  Rules
 * that are enforced here (and nowhere else):
 *
 *   - tiers have separate base intervals for the idle and foreground case;
 *   - every firing gets +-20% jitter so tiers never phase-lock;
 *   - a global "speed factor" derived from the circuit breaker slows ALL
 *     polling down when the modem starts timing out;
 *   - while a quiet window is open no POLL-priority command is submitted at
 *     all (state-machine commands own the window and are still allowed);
 *   - byte counters are read from sysfs and cost zero AT commands.
 */

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "fm160d.h"

#define HOUSEKEEP_MS            1000
#define FOREGROUND_HOLD_MS      30000
#define PORT_PROBE_GAP_MS       5000
#define BREAKER_SOFT_LIMIT      3      /* consecutive timeouts -> degraded  */
#define BREAKER_HARD_LIMIT      10     /* -> stop automatic polling         */
#define BREAKER_SOFT_HOLD_S     60

struct poll_item {
	const char *name;
	bool active_only;      /* only fire while a UI page is open     */
	int  idle_ms;          /* 0 = never at idle                     */
	int  active_ms;        /* 0 = same as idle                      */
	uint64_t next_ms;
	uint64_t last_fire_ms;
	int  fires;
	int  skips;
	void (*fire)(void);
};

/*
 * Commands that must NEVER be added to this table.
 *
 * Measured on FM160-CN 89614.1000.00.04.01.02 with no SIM fitted, which is the
 * worst case for anything that waits on card state.  The three rows below all
 * answer in <= 36 ms; these do not, and because the port is serial a single
 * stalled row starves every other command:
 *
 *   AT+GTPKGVER?   12 901 ms   ERROR (and leaks a +GTDUALSIM URC)
 *   AT+CCID        10 345 ms   timeout
 *   AT+CPMS?       10 345 ms   ERROR
 *   AT+CMGF?        5 373 ms   +CME ERROR: 10
 *   AT+CIMI         5 221 ms   (no data, URC leak)
 *   AT+CGDCONT?     5 211 ms   (no data, URC leak)
 *
 * Identity (CGMI/CGMM/CGMR/CGSN/CFSN/ICCID) and the USB mode are read once per
 * boot by the ident sequence, never here.  Anything SIM- or SMS-related belongs
 * inside the quiet window behind an explicit user action.
 *
 * For contrast, the commands that ARE polled, measured the same way:
 *   AT+CSQ 24 ms | AT+CREG?/AT+CGREG?/AT+CEREG? 26-36 ms | AT+GTCCINFO? 25 ms
 *   AT+GTUSBMODE? 16 ms | AT+GTGPS? 15 ms
 */
static struct poll_item items[] = {
	/* registration + RSSI: the cheapest and most important tier */
	{ "reg",    false, 15000,  5000, 0, 0, 0, 0, fm160_cmd_poll_reg    },
	/* extended signal quality (RSRP/RSRQ/SINR) - needs a UI to be useful */
	{ "signal", true,      0, 10000, 0, 0, 0, 0, fm160_cmd_poll_signal },
	/* serving + neighbour cells (AT+GTCCINFO?, < 3 s on the modem) */
	{ "cell",   true,      0, 10000, 0, 0, 0, 0, fm160_cmd_poll_cell   },
	{ NULL, false, 0, 0, 0, 0, 0, 0, NULL },
};

static struct uloop_timeout housekeep_timer;
static int speed_factor = 1;
static uint64_t poll_suspend_until_ms;
static bool port_ever_found;

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static int read_sysfs(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return -1;
	n = fread(buf, 1, len - 1, f);
	fclose(f);
	if (!n)
		return -1;
	buf[n] = '\0';
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
		     buf[n - 1] == ' '))
		buf[--n] = '\0';
	return 0;
}

static double rand01(void)
{
	return (double)(rand() % 10000) / 10000.0;
}

static int item_base_ms(const struct poll_item *it)
{
	int base;

	if (g_state.foreground && it->active_ms)
		base = it->active_ms;
	else
		base = it->idle_ms;

	if (!base)
		return 0;
	base *= g_state.tier_scale > 0 ? g_state.tier_scale : 1;

	/* jitter, then the breaker-driven speed factor */
	base = (int)(base * (0.8 + 0.4 * rand01()));
	return base * speed_factor;
}

static void item_arm(struct poll_item *it)
{
	int base = item_base_ms(it);

	if (!base) {
		it->next_ms = UINT64_MAX;
		return;
	}
	it->next_ms = fm160_now_ms() + base;
}

/* ------------------------------------------------------------------ */
/* port discovery                                                       */
/* ------------------------------------------------------------------ */

/*
 * Record /dev/<name> as an AT candidate.  A name that would not fit is dropped
 * rather than stored truncated: a cut-off device path is worse than a missing
 * one, because it survives discovery and only fails much later at open().
 */
static void add_candidate(const char *name)
{
	int n = snprintf(g_state.cand[g_state.cand_count], FM160_PORT_MAX,
			 "/dev/%s", name);

	if (n < 0 || n >= FM160_PORT_MAX)
		return;
	g_state.cand_count++;
}

static void fm160_scan_candidates(void)
{
	DIR *d;
	struct dirent *de;
	char path[256], vid[64];

	g_state.cand_count = 0;
	g_state.cand_idx = 0;

	d = opendir("/sys/class/tty");
	if (!d)
		return;

	while ((de = readdir(d)) != NULL && g_state.cand_count < FM160_CAND_MAX) {
		char *name = de->d_name;

		if (strncmp(name, "ttyUSB", 6) && strncmp(name, "ttyACM", 6))
			continue;

		/* /sys/class/tty/ttyUSBn/device -> the USB interface; its parent
		 * is the USB device which carries idVendor. */
		if (snprintf(path, sizeof(path),
			     "/sys/class/tty/%s/device/../idVendor",
			     name) >= (int)sizeof(path))
			continue;

		if (read_sysfs(path, vid, sizeof(vid)) != 0) {
			/* layout differs (e.g. some CDC devices): accept anyway */
			add_candidate(name);
			continue;
		}
		if (strcasecmp(vid, FM160_VENDOR_ID))
			continue;
		add_candidate(name);
	}
	closedir(d);

	if (g_state.cand_count)
		fm160_log(LOG_INFO, "found %d Fibocom (%s:*) serial port(s)",
			  g_state.cand_count, FM160_VENDOR_ID);
}

static void probe_cb(struct at_req *req, enum at_status status,
		     const char *response, void *arg)
{
	const char *probed = arg;

	if (status == AT_STATUS_OK && fm160_resp_ok(response)) {
		bool first = !g_state.port_found;

		snprintf(g_state.port, sizeof(g_state.port), "%s", probed);
		g_state.port_found = true;
		g_state.port_probing = false;
		g_state.at_state = 0;
		g_state.consec_timeout = 0;
		speed_factor = 1;
		port_ever_found = true;
		fm160_log(LOG_INFO, "AT port is %s", g_state.port);
		fm160_state_mark_dirty();
		fm160_sched_kick();
		if (first)
			fm160_cmd_ident_start();
		return;
	}

	g_state.port_probing = false;
	/* try the next candidate on the next housekeeping pass */
	g_state.port_next_probe_ms = 0;
}

static void port_discovery_tick(void)
{
	if (!g_state.port_found) {
		if (g_state.port_probing)
			return;
		if (fm160_now_ms() < g_state.port_next_probe_ms)
			return;
		if (g_state.cand_idx >= g_state.cand_count) {
			fm160_scan_candidates();
			if (!g_state.cand_count) {
				g_state.port_next_probe_ms =
					fm160_now_ms() + PORT_PROBE_GAP_MS;
				return;
			}
		}
		g_state.port_probing = true;
		if (atq_probe_port(g_state.cand[g_state.cand_idx], probe_cb,
				   g_state.cand[g_state.cand_idx]) != 0) {
			g_state.port_probing = false;
			g_state.port_next_probe_ms = fm160_now_ms() + 500;
			return;
		}
		g_state.cand_idx++;
		return;
	}

	/* Port was found before but the modem may have re-enumerated: if AT is
	 * degraded or dead for a while, drop the port and rescan. */
	if (g_state.at_state >= 1 &&
	    g_state.last_fail_ms &&
	    fm160_now_ms() - g_state.last_fail_ms > 20000) {
		fm160_log(LOG_WARN,
			  "AT unhealthy for >20 s, dropping %s and rescanning",
			  g_state.port);
		g_state.port_found = false;
		g_state.cand_idx = 0;
		g_state.cand_count = 0;
		g_state.port_next_probe_ms = fm160_now_ms() + PORT_PROBE_GAP_MS;
		fm160_state_mark_dirty();
	}
}

/* ------------------------------------------------------------------ */
/* circuit breaker                                                      */
/* ------------------------------------------------------------------ */

void fm160_sched_note_result(bool ok, bool timeout)
{
	if (ok) {
		g_state.consec_timeout = 0;
		if (g_state.at_state) {
			fm160_log(LOG_INFO, "AT recovered");
			g_state.at_state = 0;
			fm160_state_mark_dirty();
		}
		if (speed_factor != 1) {
			speed_factor = 1;
			fm160_sched_kick();
		}
		return;
	}

	g_state.last_fail_ms = fm160_now_ms();

	if (!timeout)
		return;         /* a plain ERROR is the modem answering - fine */

	g_state.consec_timeout++;

	if (g_state.consec_timeout == BREAKER_SOFT_LIMIT &&
	    g_state.at_state < 1) {
		g_state.at_state = 1;
		speed_factor = 2;
		fm160_log(LOG_WARN,
			  "AT degraded (%d timeouts): slowing polling and resting %d s",
			  g_state.consec_timeout, BREAKER_SOFT_HOLD_S);
		fm160_sched_suspend(BREAKER_SOFT_HOLD_S);
		fm160_state_mark_dirty();
	} else if (g_state.consec_timeout >= BREAKER_HARD_LIMIT &&
		   g_state.at_state < 2) {
		g_state.at_state = 2;
		speed_factor = 4;
		fm160_log(LOG_ERR,
			  "AT dead (%d timeouts): automatic polling stopped",
			  g_state.consec_timeout);
		fm160_state_mark_dirty();
	}
}

void fm160_sched_suspend(int seconds)
{
	struct poll_item *it;

	poll_suspend_until_ms = fm160_now_ms() + (uint64_t)seconds * 1000;
	for (it = items; it->name; it++)
		it->next_ms = poll_suspend_until_ms;
}

/* ------------------------------------------------------------------ */
/* housekeeping                                                         */
/* ------------------------------------------------------------------ */

void fm160_sched_report_foreground(void)
{
	g_state.last_active_ms = fm160_now_ms();
}

void fm160_sched_kick(void)
{
	struct poll_item *it;
	uint64_t now = fm160_now_ms();

	for (it = items; it->name; it++) {
		if (it->next_ms > now)
			it->next_ms = now;
	}
}

static bool item_enabled(const struct poll_item *it)
{
	if (!it->idle_ms && !it->active_ms)
		return false;
	if (it->active_only && !g_state.foreground)
		return false;
	return true;
}

static void housekeeping(struct uloop_timeout *t)
{
	struct poll_item *it;
	uint64_t now = fm160_now_ms();

	uloop_timeout_set(&housekeep_timer, HOUSEKEEP_MS);

	/* quiet window expiry */
	if (g_state.quiet_kind != QUIET_NONE && now >= g_state.quiet_until_ms) {
		atq_clear_quiet();
		fm160_sched_kick();
	}

	/* foreground decay */
	g_state.foreground = g_state.last_active_ms &&
			     (now - g_state.last_active_ms) < FOREGROUND_HOLD_MS;

	port_discovery_tick();
	fm160_netdev_poll();

	if (now < poll_suspend_until_ms)
		return;
	if (g_state.at_state == 2)
		return;
	if (!g_state.port_found || !g_state.enabled)
		return;

	for (it = items; it->name; it++) {
		int base;

		if (!item_enabled(it))
			continue;
		if (now < it->next_ms)
			continue;

		base = item_base_ms(it);
		/* Do not pile up: if the queue is already busy this tick is
		 * skipped and simply re-armed (counted for diagnostics). */
		if (atq_depth() >= 3) {
			it->skips++;
			it->next_ms = now + (base ? base / 2 : 1000);
			continue;
		}

		it->fires++;
		it->last_fire_ms = now;
		it->fire();
		it->next_ms = now + (base ? base : 1000);
	}

	fm160_state_publish();
}

void fm160_sched_init(void)
{
	struct poll_item *it;

	srand((unsigned)fm160_now_ms());
	for (it = items; it->name; it++)
		item_arm(it);
	poll_suspend_until_ms = 0;

	housekeep_timer.cb = housekeeping;
	uloop_timeout_set(&housekeep_timer, HOUSEKEEP_MS);

	fm160_scan_candidates();
}
