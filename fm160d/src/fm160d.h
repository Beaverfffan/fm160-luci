/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * fm160d - Fibocom FM160 dedicated modem manager daemon (policy layer).
 *
 * Layering:
 *   LuCI JS  --ubus-->  fm160d (this)  --ubus-->  at-daemon (transport)
 *                                                    |
 *                                                /dev/ttyUSBx
 *
 * fm160d is the ONLY component allowed to issue AT commands on the FM160's AT
 * port.  Everything else goes through the ubus object "fm160".
 */

#ifndef FM160D_H
#define FM160D_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <libubox/list.h>
#include <libubox/uloop.h>
#include <libubox/blobmsg.h>
#include <libubox/utils.h>
#include <libubus.h>

#define FM160_VENDOR_ID     "2cb7"   /* Fibocom */
#define FM160_PORT_MAX      64
#define FM160_STR_MAX       96
#define FM160_AT_CMD_MAX    256
#define FM160_AT_END_MAX    32
#define FM160_RAW_MAX       8192
#define FM160_CAND_MAX      12

/* ------------------------------------------------------------------ */
/* AT request queue                                                     */
/* ------------------------------------------------------------------ */

enum at_prio {
	AT_PRIO_INTERACTIVE = 0,   /* user pressed something            */
	AT_PRIO_STATE       = 1,   /* dialer / mode switch / sms        */
	AT_PRIO_POLL        = 2,   /* background polling                */
};

enum at_status {
	AT_STATUS_UNKNOWN = 0,
	AT_STATUS_OK      = 1,
	AT_STATUS_ERROR   = 2,     /* ERROR / +CME ERROR                */
	AT_STATUS_TIMEOUT = 3,     /* no terminal line                  */
	AT_STATUS_NOPORT  = 4,     /* AT port not (yet) available       */
	AT_STATUS_BUSY    = 5,     /* rejected, e.g. quiet window       */
};

struct at_req;

typedef void (*at_done_cb)(struct at_req *req, enum at_status status,
			   const char *response, void *arg);

struct at_req {
	struct list_head list;
	enum at_prio prio;
	uint64_t id;
	uint64_t submit_ms;
	char cmd[FM160_AT_CMD_MAX];
	char end_flag[FM160_AT_END_MAX];
	int timeout_ms;          /* sendat timeout, seconds internally */
	bool silent;             /* do not advance the circuit breaker */
	at_done_cb cb;
	void *arg;
};

/* ------------------------------------------------------------------ */
/* State cache                                                          */
/* ------------------------------------------------------------------ */

#define FM160_REG_UNKNOWN  99
/* Sentinel for a decoded value the modem did not report.  Every "not known or
 * not detectable" code is 255 (or 99) and 255 is also a legal raw value, so the
 * raw number is always kept next to the decoded one. */
#define FM160_NONE         (-1000000)

/*
 * One cell as reported by AT+GTCCINFO? (AT manual 9.1.15).
 *
 * The column layout is NOT the same for every row - it depends on both <rat>
 * and <IsServiceCell>.  Verified against the manual:
 *
 *   LTE service (rat 4, IsServiceCell 1) - 14 columns
 *     <IsServiceCell>,<rat>,<mcc>,<mnc>,<tac>,<cellid>,<earfcn>,
 *     <physicalcellId>,<band>,<bandwidth>,<rssnr_value>,<rxlev>,<rsrp>,<rsrq>
 *       idx  0..7         8 band  9 bandwidth 10 sinr 11 rxlev 12 rsrp 13 rsrq
 *
 *   LTE neighbour (rat 4, IsServiceCell 2) - 12 columns
 *     ...,<physicalcellId>,<bandwidth>,<rxlev>,<rsrp>,<rsrq>
 *       idx  8 bandwidth 9 rxlev 10 rsrp 11 rsrq      (band NOT reported)
 *
 *   NR service (rat 9) - 14 columns, same indices as the LTE service row.
 *   NR neighbour (rat 9)  - 12 columns but 8 is <ss-sinr>, NOT <bandwidth>:
 *     ...,<physicalcellId>,<ss-sinr>,<rxlev>,<ss-rsrp>,<ss-rsrq>
 *       idx  8 sinr 9 rxlev 10 rsrp 11 rsrq
 */
struct fm160_cell {
	bool valid;
	bool is_service;
	int  rat;                /* <rat>: 0 invalid, 2 WCDMA, 4 LTE, 9 NR */
	int  mcc, mnc;
	long long tac;
	long long cellid;
	long long earfcn;
	int  pci;
	int  band;               /* 0 = not reported (all neighbour rows)   */
	int  bandwidth_raw;      /* LTE: RB count 6/15/25/50/75/100         */
	                         /* NR : code 0(=5MHz)/10/15/.../400        */
	int  bandwidth_mhz;      /* decoded MHz, FM160_NONE when unknown    */
	int  sinr_raw;           /* rssnr_value (LTE) / ss-sinr (NR)        */
	int  sinr_db10;          /* tenths of a dB; FM160_NONE when 255     */
	int  rsrp_raw;
	int  rsrp_dbm;
	int  rsrq_raw;
	int  rsrq_db10;          /* tenths of a dB                          */
	int  rxlev_raw;
};

struct fm160_state {
	/* port */
	char port[FM160_PORT_MAX];
	bool port_found;
	bool port_probing;
	uint64_t port_next_probe_ms;
	char cand[FM160_CAND_MAX][FM160_PORT_MAX];
	int  cand_count;
	int  cand_idx;

	/* identity */
	char manufacturer[FM160_STR_MAX];
	char model[FM160_STR_MAX];
	char revision[FM160_STR_MAX];
	char imei[FM160_STR_MAX];
	char sn[FM160_STR_MAX];
	char iccid[FM160_STR_MAX];
	int  usb_mode;                 /* -1 unknown */
	bool ident_done;

	/* sim / registration */
	char pin_status[32];
	int  creg, cgreg, cereg, c5greg;   /* FM160_REG_UNKNOWN = unknown */
	char oper[FM160_STR_MAX];
	int  act_rat;

	/* signal */
	int  csq_rssi, csq_ber;            /* 99 = unknown */
	/* AT+GTCSQNREN=1 replaces +CSQ's <rssi> with ss_rsrp (0..126) and makes
	 * the -113+2n formula meaningless.  We never enable it; this flag lets
	 * the UI hide the dBm number instead of showing a wrong one. */
	bool csq_is_ss_rsrp;
	/*
	 * AT+CESQ (AT manual 9.1.2) returns NINE fields:
	 *   <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>,<ss_rsrq>,<ss_rsrp>,<ss_sinr>
	 * The first four are GERAN/UTRAN, 4-5 are LTE, 6-8 are NR.  A field that
	 * does not apply to the current serving cell is 255 (99 for rxlev/ber).
	 * Raw values are kept as reported; the decoded ones are in tenths of a dB
	 * (or dBm) so that no float arithmetic is needed anywhere.
	 */
	int  cesq_rxlev, cesq_ber, cesq_rscp, cesq_ecno, cesq_rsrq, cesq_rsrp;
	int  cesq_ss_rsrq, cesq_ss_rsrp, cesq_ss_sinr;
	int  geran_rssi_dbm;               /* -110 + rxlev   (0..63)   */
	int  utra_rscp_dbm;                /* -120 + rscp    (0..96)   */
	int  utra_ecno_db10;               /* -240 + 5*ecno  (0..49)   */
	int  lte_rsrp_dbm;                 /* -140 + rsrp    (0..97)   */
	int  lte_rsrq_db10;                /* -195 + 5*rsrq  (0..34)   */
	int  nr_ss_rsrp_dbm;               /* -156 + ss_rsrp (0..126)  */
	int  nr_ss_rsrq_db10;              /* -430 + 5*ss_rsrq (0..126)*/
	int  nr_ss_sinr_db10;              /* -230 + 5*ss_sinr (0..127)*/

	struct fm160_cell serving;   /* headline cell = highest RAT seen      */
	struct fm160_cell serving2;  /* EN-DC: the LTE anchor, otherwise empty */
	struct fm160_cell neigh[10];
	int  neigh_count;
	uint64_t cell_last_ok_ms;

	/* netdev counters (read from sysfs, costs zero AT) */
	char netdev[32];
	uint64_t rx_bytes, tx_bytes;
	uint64_t rx_bytes_prev, tx_bytes_prev;
	uint64_t rx_bps, tx_bps;

	/* health */
	int  at_state;                     /* 0 ok, 1 degraded, 2 dead   */
	int  consec_timeout;
	uint64_t last_ok_ms;
	uint64_t last_fail_ms;
	uint64_t at_busy_max_ms;           /* worst response time seen   */

	/* quiet window */
	int  quiet_kind;
	uint64_t quiet_until_ms;
	char quiet_reason[FM160_STR_MAX];

	/* foreground detection */
	uint64_t last_active_ms;
	bool foreground;

	/* accepted profile override from config */
	bool enabled;
	int  tier_scale;                   /* 1 = normal, 2 = relaxed    */
};

enum {
	QUIET_NONE = 0,
	QUIET_MANUAL,
	QUIET_CFUN,
	QUIET_MODE_SWITCH,
	QUIET_DIAL,
	QUIET_COPS,
	QUIET_GNSS,
};

extern struct fm160_state g_state;
extern struct ubus_context *g_ubus;

/* ------------------------------------------------------------------ */
/* time helpers                                                         */
/* ------------------------------------------------------------------ */

uint64_t fm160_now_ms(void);

/* ------------------------------------------------------------------ */
/* config (uci /etc/config/fm160)                                       */
/* ------------------------------------------------------------------ */

void fm160_config_load(void);

/* ------------------------------------------------------------------ */
/* atq.c - AT queue over at-daemon                                      */
/* ------------------------------------------------------------------ */

int  atq_init(void);
/* Enqueue a command.  cb may be NULL (fire and forget).  Returns 0 if the
 * request was accepted, -EAGAIN when AT is not usable (no port yet, quiet
 * window, circuit open) and -ENOSPC when the queue is full. */
int  atq_submit(enum at_prio prio, const char *cmd, const char *end_flag,
		int timeout_ms, at_done_cb cb, void *arg);
int  atq_submit_silent(enum at_prio prio, const char *cmd, int timeout_ms);
void atq_set_quiet(int kind, const char *reason, int seconds);
void atq_clear_quiet(void);
bool atq_quiet_active(void);
int  atq_depth(void);
void atq_state_reset(void);
/* Called by the port prober while scanning candidates. */
int  atq_probe_port(const char *port, at_done_cb cb, void *arg);
/* Force a fresh lookup of the transport ubus object. */
void at_daemon_hint_reconnect(void);

/* Response helpers usable from callbacks. */
bool fm160_resp_ok(const char *resp);
bool fm160_resp_error(const char *resp);
/* Find a line starting with "prefix" (after stripping echo); copies the rest
 * (after the prefix, leading spaces trimmed) into out. Returns true on hit. */
bool fm160_resp_find(const char *resp, const char *prefix, char *out, size_t outlen);
int  fm160_resp_lines(const char *resp, const char *prefix,
		      char out[][FM160_STR_MAX], int max);

/* ------------------------------------------------------------------ */
/* sched.c - polling scheduler                                          */
/* ------------------------------------------------------------------ */

void fm160_sched_init(void);
void fm160_sched_tick(void);           /* called from the 1 Hz housekeeping timer */
void fm160_sched_note_result(bool ok, bool timeout);
void fm160_sched_report_foreground(void);
void fm160_sched_kick(void);           /* force an immediate refresh of tier 0/1 */
void fm160_sched_suspend(int seconds); /* park every tier for a while          */

/* ------------------------------------------------------------------ */
/* state.c - snapshot and push                                          */
/* ------------------------------------------------------------------ */

void fm160_state_init(void);
void fm160_state_touch_ok(void);
/* Publish the current snapshot to ubus subscribers as "fm160.state". */
void fm160_state_publish(void);
void fm160_state_mark_dirty(void);
/* Build (or reuse) the serialised snapshot; caller must not free it twice. */
struct blob_buf *fm160_state_blob(void);
/* netdev counters */
void fm160_netdev_poll(void);

/* ------------------------------------------------------------------ */
/* cmds.c - FM160 command layer                                         */
/* ------------------------------------------------------------------ */

void fm160_cmd_ident_start(void);
void fm160_ident_reset(void);
void fm160_cmd_poll_reg(void);
void fm160_cmd_poll_signal(void);
void fm160_cmd_poll_cell(void);

/* Parsers (also used by the AT debug page / tests). */
void fm160_parse_csq(const char *resp);
void fm160_parse_cesq(const char *resp);
void fm160_parse_greg(const char *resp, const char *prefix, int *slot);
void fm160_parse_ccinfo(const char *resp);
void fm160_parse_usbmode(const char *resp);

/* ------------------------------------------------------------------ */
/* ubus_methods.c                                                       */
/* ------------------------------------------------------------------ */

void fm160_ubus_methods_init(void);

/* ------------------------------------------------------------------ */
/* logging                                                              */
/* ------------------------------------------------------------------ */

void fm160_log(int priority, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* <syslog.h> defines LOG_ERR/LOG_INFO/LOG_DEBUG with these exact values; the
 * guards keep this header usable both with and without it. */
#ifndef LOG_ERR
#define LOG_ERR   3
#endif
#ifndef LOG_WARN
#define LOG_WARN  4
#endif
#ifndef LOG_INFO
#define LOG_INFO  6
#endif
#ifndef LOG_DEBUG
#define LOG_DEBUG 7
#endif

#endif /* FM160D_H */
