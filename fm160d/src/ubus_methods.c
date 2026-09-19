/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ubus_methods.c - the ubus object "fm160".
 *
 * This is the only interface the LuCI front end (and any other consumer) is
 * allowed to use.  Read paths never touch the modem: they return the cached
 * snapshot.  The single write path that does touch the modem is "at", which is
 * rate limited and opens a short quiet window so that a user-issued command
 * cannot collide with a poll in flight.
 */

#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "fm160d.h"

enum {
	ATTR_CMD,
	ATTR_TIMEOUT,
	ATTR_END_FLAG,
	__ATTR_MAX
};

static const struct blobmsg_policy at_policy[] = {
	[ATTR_CMD]      = { .name = "cmd",      .type = BLOBMSG_TYPE_STRING },
	[ATTR_TIMEOUT]  = { .name = "timeout",  .type = BLOBMSG_TYPE_INT32  },
	[ATTR_END_FLAG] = { .name = "end_flag", .type = BLOBMSG_TYPE_STRING },
};

enum {
	ATTR_ACTIVE,
	__ATTR_PROFILE_MAX
};

static const struct blobmsg_policy profile_policy[] = {
	[ATTR_ACTIVE] = { .name = "active", .type = BLOBMSG_TYPE_BOOL },
};

enum {
	ATTR_ENABLED,
	__ATTR_ENABLED_MAX
};

static const struct blobmsg_policy enabled_policy[] = {
	[ATTR_ENABLED] = { .name = "enabled", .type = BLOBMSG_TYPE_BOOL },
};

/* --- M4 ------------------------------------------------------------- */

enum {
	ATTR_BANDS,
	__ATTR_BANDS_MAX
};

static const struct blobmsg_policy bands_policy[] = {
	[ATTR_BANDS] = { .name = "bands", .type = BLOBMSG_TYPE_STRING },
};

enum {
	ATTR_MODE,
	ATTR_RAT,
	ATTR_TYPE,
	ATTR_EARFCN,
	ATTR_PCI,
	ATTR_SCS,
	ATTR_NRBAND,
	__ATTR_CELLLOCK_MAX
};

static const struct blobmsg_policy celllock_policy[] = {
	[ATTR_MODE]   = { .name = "mode",   .type = BLOBMSG_TYPE_INT32  },
	[ATTR_RAT]    = { .name = "rat",    .type = BLOBMSG_TYPE_INT32  },
	[ATTR_TYPE]   = { .name = "type",   .type = BLOBMSG_TYPE_INT32  },
	/*
	 * earfcn reaches 4294967295 in the capability range, so it cannot be
	 * carried as an int32 without wrapping - but the type is deliberately
	 * left UNSPEC here rather than declared INT64, and that is a fix, not an
	 * oversight.
	 *
	 * libubox parses a JSON number that FITS int32 into an INT32 blob and
	 * only widens to INT64 above 2^31.  A policy that demands INT64 therefore
	 * rejects every ordinary earfcn and accepts only the rare giant one - the
	 * exact inverse of what was intended, and invisible until someone tried
	 * an ordinary value.  Measured on the device: `ubus call fm160
	 * sms_delete '{"id":9999}'` answered "Invalid argument" instead of "Not
	 * found" for precisely this reason.
	 *
	 * UNSPEC skips the type check (blobmsg_parse only compares when the
	 * policy names a type), and blobmsg_get_u64() below reads an INT32, an
	 * INT64 or a numeric string alike.  The value is validated where it
	 * belongs: fm160_cmd_set_celllock() and the modem's own AT+GTCELLLOCK=?
	 * answer.
	 */
	[ATTR_EARFCN] = { .name = "earfcn" },
	[ATTR_PCI]    = { .name = "pci",    .type = BLOBMSG_TYPE_INT32  },
	[ATTR_SCS]    = { .name = "scs",    .type = BLOBMSG_TYPE_INT32  },
	[ATTR_NRBAND] = { .name = "nrband", .type = BLOBMSG_TYPE_INT32  },
};

/* --- M5 ------------------------------------------------------------- */

enum {
	ATTR_GNSS_ON,
	__ATTR_GNSS_MAX
};

static const struct blobmsg_policy gnss_policy[] = {
	[ATTR_GNSS_ON] = { .name = "enabled", .type = BLOBMSG_TYPE_BOOL },
};

enum {
	ATTR_CONSTELLATION,
	__ATTR_CONSTELLATION_MAX
};

static const struct blobmsg_policy constellation_policy[] = {
	[ATTR_CONSTELLATION] = { .name = "constellation", .type = BLOBMSG_TYPE_INT32 },
};

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static int reply_snapshot(struct ubus_context *ctx, struct ubus_request_data *req)
{
	struct blob_buf *b = fm160_state_blob();

	return ubus_send_reply(ctx, req, b->head);
}

/*
 * Deferred ("async") reply.
 *
 * libubus has no ubus_request_data_dup()/ubus_request_data_free(): the incoming
 * struct ubus_request_data is only valid for the duration of the handler.  To
 * answer later you copy it out with ubus_defer_request() - which also marks the
 * original as deferred, telling ubus not to send its own reply - and finish
 * with ubus_complete_deferred_request().  struct ubus_request_data is a plain
 * POD struct (object/peer/seq/acl/deferred/fd/req_fd), so keeping the copy in a
 * heap allocation is safe as long as it outlives the completion.
 */
struct pending_at {
	struct ubus_context *ctx;
	struct ubus_request_data req;
};

static const char *at_status_name(enum at_status status)
{
	switch (status) {
	case AT_STATUS_OK:      return "ok";
	case AT_STATUS_ERROR:   return "error";
	case AT_STATUS_TIMEOUT: return "timeout";
	case AT_STATUS_NOPORT:  return "no_port";
	case AT_STATUS_BUSY:    return "busy";
	default:                return "unknown";
	}
}

/* Complete a deferred reply with the outcome of one AT exchange. */
static void pending_reply(struct pending_at *p, struct at_req *r,
			  enum at_status status, const char *response)
{
	struct blob_buf b = {};

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "status", at_status_name(status));
	blobmsg_add_string(&b, "command", r ? r->cmd : "");
	blobmsg_add_string(&b, "response", response ? response : "");
	ubus_send_reply(p->ctx, &p->req, b.head);
	blob_buf_free(&b);

	ubus_complete_deferred_request(p->ctx, &p->req, UBUS_STATUS_OK);

	/* A user-issued command is also how the UI says "I am here": keep the
	 * foreground window open so the polling tiers stay warm for a moment. */
	fm160_sched_report_foreground();
	free(p);
}

static void manual_at_cb(struct at_req *r, enum at_status status,
			 const char *response, void *arg)
{
	pending_reply(arg, r, status, response);
}

/* ------------------------------------------------------------------ */
/* handlers                                                             */
/* ------------------------------------------------------------------ */

static int handle_status(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method,
			 struct blob_attr *msg)
{
	return reply_snapshot(ctx, req);
}

static int handle_profile(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	struct blob_attr *tb[__ATTR_PROFILE_MAX];

	blobmsg_parse(profile_policy, __ATTR_PROFILE_MAX, tb,
		      msg ? blob_data(msg) : NULL, msg ? blob_len(msg) : 0);
	if (tb[ATTR_ACTIVE] && blobmsg_get_bool(tb[ATTR_ACTIVE])) {
		fm160_sched_report_foreground();
		g_state.foreground = true;
		fm160_sched_kick();
	}

	return reply_snapshot(ctx, req);
}

static int handle_at(struct ubus_context *ctx, struct ubus_object *obj,
		     struct ubus_request_data *req, const char *method,
		     struct blob_attr *msg)
{
	struct blob_attr *tb[__ATTR_MAX];
	struct pending_at *p;
	const char *cmd;
	const char *end_flag = NULL;
	int timeout_ms = 5000;
	int ret;

	blobmsg_parse(at_policy, __ATTR_MAX, tb,
		      msg ? blob_data(msg) : NULL, msg ? blob_len(msg) : 0);
	if (!tb[ATTR_CMD])
		return UBUS_STATUS_INVALID_ARGUMENT;

	cmd = blobmsg_get_string(tb[ATTR_CMD]);
	if (!cmd[0] || strlen(cmd) > FM160_AT_CMD_MAX - 8)
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (tb[ATTR_TIMEOUT])
		timeout_ms = (int)blobmsg_get_u32(tb[ATTR_TIMEOUT]);
	if (tb[ATTR_END_FLAG])
		end_flag = blobmsg_get_string(tb[ATTR_END_FLAG]);

	if (!g_state.port_found)
		return UBUS_STATUS_NOT_FOUND;
	if (g_state.at_state == 2)
		return UBUS_STATUS_UNKNOWN_ERROR;

	p = calloc(1, sizeof(*p));
	if (!p)
		return UBUS_STATUS_UNKNOWN_ERROR;

	p->ctx = ctx;

	/* From here on we own the reply: ubus_defer_request() copies the request
	 * out and marks the original deferred, so every path below must finish
	 * with ubus_complete_deferred_request().  It has to happen *before*
	 * atq_submit() because that can invoke manual_at_cb() synchronously. */
	ubus_defer_request(ctx, req, &p->req);

	/* A manual command is a debugging tool: give the modem room to answer
	 * and keep polls out of the way while it runs. */
	atq_set_quiet(QUIET_MANUAL, "manual AT", 10);

	ret = atq_submit(AT_PRIO_INTERACTIVE, cmd, end_flag, timeout_ms,
			 manual_at_cb, p);
	if (ret) {
		atq_clear_quiet();
		ubus_complete_deferred_request(ctx, &p->req,
					       UBUS_STATUS_UNKNOWN_ERROR);
		free(p);
		return UBUS_STATUS_OK;   /* the error is in the deferred reply */
	}
	return UBUS_STATUS_OK;
}

static int handle_rescan(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method,
			 struct blob_attr *msg)
{
	fm160_log(LOG_INFO, "rescan requested");
	atq_state_reset();
	g_state.port_found = false;
	g_state.port_probing = false;
	g_state.cand_count = 0;
	g_state.cand_idx = 0;
	g_state.port_next_probe_ms = 0;
	g_state.at_state = 0;
	g_state.consec_timeout = 0;
	fm160_state_mark_dirty();

	return reply_snapshot(ctx, req);
}

static int handle_ident(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	if (!g_state.port_found)
		return UBUS_STATUS_NOT_FOUND;
	fm160_cmd_ident_start();

	return reply_snapshot(ctx, req);
}

static int handle_identity(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	/* The full identity record including the raw USB mode is already part
	 * of the snapshot, but the manual enumerations are exposed separately
	 * because they are what the mode-switch decision needs. */
	char buf[64];
	struct blob_buf b = {};

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "manufacturer", g_state.manufacturer);
	blobmsg_add_string(&b, "model", g_state.model);
	blobmsg_add_string(&b, "revision", g_state.revision);
	blobmsg_add_string(&b, "imei", g_state.imei);
	blobmsg_add_string(&b, "sn", g_state.sn);
	blobmsg_add_string(&b, "iccid", g_state.iccid);
	if (g_state.usb_mode >= 0) {
		snprintf(buf, sizeof(buf), "%d", g_state.usb_mode);
		blobmsg_add_string(&b, "usb_mode", buf);
	}
	ubus_send_reply(ctx, req, b.head);
	blob_buf_free(&b);

	return UBUS_STATUS_OK;
}

static int handle_enabled(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	struct blob_attr *tb[__ATTR_ENABLED_MAX];

	blobmsg_parse(enabled_policy, __ATTR_ENABLED_MAX, tb,
		      msg ? blob_data(msg) : NULL, msg ? blob_len(msg) : 0);
	if (tb[ATTR_ENABLED]) {
		g_state.enabled = blobmsg_get_bool(tb[ATTR_ENABLED]);
		if (!g_state.enabled) {
			atq_state_reset();
			fm160_log(LOG_WARN, "management disabled by user");
		} else {
			g_state.at_state = 0;
			g_state.consec_timeout = 0;
			fm160_sched_kick();
		}
		fm160_state_mark_dirty();
	}

	return reply_snapshot(ctx, req);
}

/*
 * --- M4 write handlers ----------------------------------------------
 *
 * Both defer, then hand off to the write sequence in cmds.c, which is
 * responsible for the write -> read-back -> re-parse round trip.  The reply
 * only reports whether that round trip worked; the values themselves arrive
 * through the next "status".
 *
 * Neither handler validates the band encoding or the cell-lock ranges beyond
 * what is needed to build a safe command line: the authoritative checks live
 * in fm160_bands_command()/fm160_celllock_command(), so a second copy here
 * could only drift out of sync with them.
 */

/* Allocate the deferred-reply context and mark the request deferred. */
static struct pending_at *pending_begin(struct ubus_context *ctx,
					struct ubus_request_data *req)
{
	struct pending_at *p = calloc(1, sizeof(*p));

	if (!p)
		return NULL;
	p->ctx = ctx;
	ubus_defer_request(ctx, req, &p->req);
	return p;
}

/* Fail a deferred reply that was never handed to the AT queue. */
static int pending_abort(struct ubus_context *ctx, struct pending_at *p,
			 int status)
{
	ubus_complete_deferred_request(ctx, &p->req, status);
	free(p);
	/* The deferred reply carries the error; returning OK here only stops
	 * libubus from trying to answer a request that is already answered. */
	return UBUS_STATUS_OK;
}

static int handle_setbands(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	struct blob_attr *tb[__ATTR_BANDS_MAX];
	struct pending_at *p;
	const char *bands;

	blobmsg_parse(bands_policy, __ATTR_BANDS_MAX, tb,
		      msg ? blob_data(msg) : NULL, msg ? blob_len(msg) : 0);
	if (!tb[ATTR_BANDS])
		return UBUS_STATUS_INVALID_ARGUMENT;
	bands = blobmsg_get_string(tb[ATTR_BANDS]);
	/* The same length guard the "at" method uses: this string ends up
	 * inside a fixed command buffer, so it must not be able to overflow it. */
	if (!bands[0] || strlen(bands) > FM160_AT_CMD_MAX - 16)
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (!g_state.port_found)
		return UBUS_STATUS_NOT_FOUND;
	if (g_state.at_state == 2)
		return UBUS_STATUS_UNKNOWN_ERROR;
	/* The capability enumeration is the licence to write.  Without it we do
	 * not know which tokens this firmware accepts, and AT+GTACT is
	 * persistent - a wrong guess survives a reboot. */
	if (!g_state.gtact_caps.valid) {
		fm160_log(LOG_WARNING, "setbands refused: AT+GTACT=? not enumerated");
		return UBUS_STATUS_NOT_SUPPORTED;
	}

	p = pending_begin(ctx, req);
	if (!p)
		return UBUS_STATUS_UNKNOWN_ERROR;

	if (fm160_cmd_set_bands(bands, manual_at_cb, p))
		return pending_abort(ctx, p, UBUS_STATUS_UNKNOWN_ERROR);
	return UBUS_STATUS_OK;
}

static int handle_setcelllock(struct ubus_context *ctx, struct ubus_object *obj,
			      struct ubus_request_data *req, const char *method,
			      struct blob_attr *msg)
{
	struct blob_attr *tb[__ATTR_CELLLOCK_MAX];
	struct pending_at *p;
	/* -1 is "the caller did not supply this field".  For pci, scs and nrband
	 * that is different from 0: PCI 0 is a real cell, SCS 0 is 15 kHz, and
	 * NR band 0 does not exist - see fm160_celllock_command(). */
	int mode, rat = 0, type = 0, pci = -1, scs = -1, nrband = -1;
	unsigned long long earfcn = 0;

	blobmsg_parse(celllock_policy, __ATTR_CELLLOCK_MAX, tb,
		      msg ? blob_data(msg) : NULL, msg ? blob_len(msg) : 0);
	if (!tb[ATTR_MODE])
		return UBUS_STATUS_INVALID_ARGUMENT;
	mode = (int)blobmsg_get_u32(tb[ATTR_MODE]);
	/* Only the two documented values.  The live modem also advertises mode
	 * 2 in AT+GTCELLLOCK=?; writing an undocumented value into a persistent
	 * EFS setting is not a risk this milestone takes. */
	if (mode != 0 && mode != 1)
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (tb[ATTR_RAT])    rat    = (int)blobmsg_get_u32(tb[ATTR_RAT]);
	if (tb[ATTR_TYPE])   type   = (int)blobmsg_get_u32(tb[ATTR_TYPE]);
	if (tb[ATTR_EARFCN]) earfcn = blobmsg_get_u64(tb[ATTR_EARFCN]);
	if (tb[ATTR_PCI])    pci    = (int)blobmsg_get_u32(tb[ATTR_PCI]);
	if (tb[ATTR_SCS])    scs    = (int)blobmsg_get_u32(tb[ATTR_SCS]);
	if (tb[ATTR_NRBAND]) nrband = (int)blobmsg_get_u32(tb[ATTR_NRBAND]);

	if (!g_state.port_found)
		return UBUS_STATUS_NOT_FOUND;
	if (g_state.at_state == 2)
		return UBUS_STATUS_UNKNOWN_ERROR;
	if (!g_state.celllock_caps.valid) {
		fm160_log(LOG_WARNING,
			  "setcelllock refused: AT+GTCELLLOCK=? not enumerated");
		return UBUS_STATUS_NOT_SUPPORTED;
	}

	if (mode == 1) {
		/* Enabling needs a frequency to lock to.  earfcn 0 is a real
		 * value in the range the modem reports, so "absent" and "zero"
		 * are different things and only absence is rejected. */
		if (!tb[ATTR_EARFCN])
			return UBUS_STATUS_INVALID_ARGUMENT;
		if (earfcn > g_state.celllock_caps.earfcn_max)
			return UBUS_STATUS_INVALID_ARGUMENT;
		if (pci >= 0 && pci > g_state.celllock_caps.pci_max)
			return UBUS_STATUS_INVALID_ARGUMENT;
		if (scs >= 0 && (scs < g_state.celllock_caps.scs_min ||
				 scs > g_state.celllock_caps.scs_max))
			return UBUS_STATUS_INVALID_ARGUMENT;
		if (nrband >= 0 && (nrband < g_state.celllock_caps.nrband_min ||
				    nrband > g_state.celllock_caps.nrband_max))
			return UBUS_STATUS_INVALID_ARGUMENT;
	}

	p = pending_begin(ctx, req);
	if (!p)
		return UBUS_STATUS_UNKNOWN_ERROR;

	if (fm160_cmd_set_celllock(mode, rat, type, earfcn, pci, scs, nrband,
				   manual_at_cb, p))
		return pending_abort(ctx, p, UBUS_STATUS_UNKNOWN_ERROR);
	return UBUS_STATUS_OK;
}

/*
 * --- M5 write handlers ----------------------------------------------
 *
 * The engine switch has no capability guard on the daemon side, and the reason
 * is not laxity: AT+GTGPSPOWER is the one setting in this object that the module
 * does NOT store, so a wrong value cannot outlive a power cycle, and both 0 and 1
 * were exercised end to end on hardware.  The constellation write below is
 * stored, and it carries the full guard: no AT+GTGPSCFG=? answer, no write.
 *
 * Neither handler validates the value range beyond what the builder needs.  The
 * authoritative checks are fm160_gnss_cfg_command() (the documented set) and
 * fm160_cmd_set_gnss_cfg() (the modem's own list); a second copy here could only
 * drift away from them.
 */
static int handle_setgnss(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	struct blob_attr *tb[__ATTR_GNSS_MAX];
	struct pending_at *p;
	int on;

	blobmsg_parse(gnss_policy, __ATTR_GNSS_MAX, tb,
		      msg ? blob_data(msg) : NULL, msg ? blob_len(msg) : 0);
	if (!tb[ATTR_GNSS_ON])
		return UBUS_STATUS_INVALID_ARGUMENT;
	on = blobmsg_get_bool(tb[ATTR_GNSS_ON]) ? 1 : 0;

	if (!g_state.port_found)
		return UBUS_STATUS_NOT_FOUND;
	if (g_state.at_state == 2)
		return UBUS_STATUS_UNKNOWN_ERROR;

	p = pending_begin(ctx, req);
	if (!p)
		return UBUS_STATUS_UNKNOWN_ERROR;

	if (fm160_cmd_set_gnss_power(on, manual_at_cb, p))
		return pending_abort(ctx, p, UBUS_STATUS_UNKNOWN_ERROR);
	return UBUS_STATUS_OK;
}

static int handle_setgnsscfg(struct ubus_context *ctx, struct ubus_object *obj,
			     struct ubus_request_data *req, const char *method,
			     struct blob_attr *msg)
{
	struct blob_attr *tb[__ATTR_CONSTELLATION_MAX];
	struct pending_at *p;
	int value;

	blobmsg_parse(constellation_policy, __ATTR_CONSTELLATION_MAX, tb,
		      msg ? blob_data(msg) : NULL, msg ? blob_len(msg) : 0);
	if (!tb[ATTR_CONSTELLATION])
		return UBUS_STATUS_INVALID_ARGUMENT;
	value = (int)blobmsg_get_u32(tb[ATTR_CONSTELLATION]);

	if (!g_state.port_found)
		return UBUS_STATUS_NOT_FOUND;
	if (g_state.at_state == 2)
		return UBUS_STATUS_UNKNOWN_ERROR;
	if (!g_state.gnss.cfg.caps_valid) {
		fm160_log(LOG_WARNING,
			  "setgnsscfg refused: AT+GTGPSCFG=? not enumerated");
		return UBUS_STATUS_NOT_SUPPORTED;
	}

	p = pending_begin(ctx, req);
	if (!p)
		return UBUS_STATUS_UNKNOWN_ERROR;

	if (fm160_cmd_set_gnss_cfg(value, manual_at_cb, p))
		return pending_abort(ctx, p, UBUS_STATUS_UNKNOWN_ERROR);
	return UBUS_STATUS_OK;
}

/* --- M3: SMS -------------------------------------------------------- */

enum {
	ATTR_SMS_NUMBER,
	ATTR_SMS_TEXT,
	__ATTR_SMS_SEND_MAX
};

static const struct blobmsg_policy sms_send_policy[] = {
	[ATTR_SMS_NUMBER] = { .name = "number", .type = BLOBMSG_TYPE_STRING },
	[ATTR_SMS_TEXT]   = { .name = "text",   .type = BLOBMSG_TYPE_STRING },
};

enum {
	ATTR_SMS_ID,
	__ATTR_SMS_ID_MAX
};

static const struct blobmsg_policy sms_id_policy[] = {
	/*
	 * UNSPEC for the same reason as earfcn above: the id travels as a JSON
	 * number that always fits int32, so libubox stores it as an INT32 blob,
	 * and a policy demanding INT64 rejects it.  With INT64 here, both
	 * sms_delete and sms_markread answered "Invalid argument" to every
	 * request - including the ones from the LuCI page, whose rpc sends the
	 * same shape.  Reading with blobmsg_get_u64() accepts either blob.
	 */
	[ATTR_SMS_ID] = { .name = "id" },
};

/*
 * The message list is deliberately NOT part of the status snapshot: it is up to
 * a kilobyte of text per message and the snapshot is pushed on every change.
 * Building it here means it is built when a page is actually looking at it.
 */
static int handle_sms_list(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	struct blob_buf b = {};
	int i, n = fm160_sms_count();

	(void)obj;
	(void)method;
	(void)msg;

	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "count", (uint32_t)n);
	{
		void *a = blobmsg_open_array(&b, "messages");

		for (i = 0; i < n; i++) {
			const struct fm160_sms_msg *m = fm160_sms_get(i);
			void *t;

			if (!m)
				break;
			t = blobmsg_open_table(&b, NULL);
			blobmsg_add_u64(&b, "id", m->id);
			blobmsg_add_u8(&b, "outgoing", m->outgoing);
			/* -1 means "we have no copy on the modem" - a message
			 * read here, or one we sent.  Sent as a flag as well so
			 * the page does not have to know that 4294967295 is
			 * "no index". */
			blobmsg_add_u8(&b, "index_known", m->index >= 0);
			blobmsg_add_u32(&b, "index", (uint32_t)m->index);
			blobmsg_add_string(&b, "storage", m->storage);
			blobmsg_add_string(&b, "number", m->number);
			blobmsg_add_u8(&b, "international", m->international);
			blobmsg_add_string(&b, "text", m->text);
			blobmsg_add_u32(&b, "text_len", (uint32_t)m->text_len);
			blobmsg_add_u32(&b, "encoding", (uint32_t)m->encoding);
			blobmsg_add_u8(&b, "has_time", m->has_time);
			if (m->has_time) {
				blobmsg_add_u32(&b, "year", (uint32_t)m->year);
				blobmsg_add_u32(&b, "month", (uint32_t)m->month);
				blobmsg_add_u32(&b, "day", (uint32_t)m->day);
				blobmsg_add_u32(&b, "hour", (uint32_t)m->hour);
				blobmsg_add_u32(&b, "min", (uint32_t)m->min);
				blobmsg_add_u32(&b, "sec", (uint32_t)m->sec);
				blobmsg_add_u32(&b, "tz_quarters",
						(uint32_t)m->tz_quarters);
				blobmsg_add_u8(&b, "tz_negative", m->tz_negative);
			}
			if (m->concat) {
				blobmsg_add_u8(&b, "concat", 1);
				blobmsg_add_u32(&b, "ref", (uint32_t)m->ref);
				blobmsg_add_u32(&b, "parts", (uint32_t)m->parts);
				blobmsg_add_u32(&b, "part", (uint32_t)m->part);
			}
			blobmsg_add_u32(&b, "status", (uint32_t)m->status);
			blobmsg_add_u64(&b, "age_ms", fm160_now_ms() - m->local_ms);
			blobmsg_close_table(&b, t);
		}
		blobmsg_close_array(&b, a);
	}
	ubus_send_reply(ctx, req, b.head);
	blob_buf_free(&b);
	return UBUS_STATUS_OK;
}

static int handle_sms_send(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	struct blob_attr *tb[__ATTR_SMS_SEND_MAX];
	struct pending_at *p;
	const char *number, *text;
	int rc;

	(void)obj;
	(void)method;

	blobmsg_parse(sms_send_policy, __ATTR_SMS_SEND_MAX, tb,
		      msg ? blob_data(msg) : NULL, msg ? blob_len(msg) : 0);
	if (!tb[ATTR_SMS_NUMBER] || !tb[ATTR_SMS_TEXT])
		return UBUS_STATUS_INVALID_ARGUMENT;
	number = blobmsg_get_string(tb[ATTR_SMS_NUMBER]);
	text = blobmsg_get_string(tb[ATTR_SMS_TEXT]);
	if (!number[0] || !text[0])
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (strlen(text) >= FM160_SMS_TEXT_MAX)
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (!g_state.port_found)
		return UBUS_STATUS_NOT_FOUND;
	if (g_state.at_state == 2)
		return UBUS_STATUS_UNKNOWN_ERROR;
	/* The whole feature is gated on the modem having accepted PDU mode, a
	 * storage and the new-message indication.  Offering the button before
	 * that just queues a command the modem has already refused once. */
	if (!fm160_sms_usable()) {
		fm160_log(LOG_WARNING,
			  "sms send refused: the modem has not accepted the sms setup");
		return UBUS_STATUS_NOT_SUPPORTED;
	}

	p = pending_begin(ctx, req);
	if (!p)
		return UBUS_STATUS_UNKNOWN_ERROR;

	rc = fm160_cmd_sms_send(number, text, manual_at_cb, p);
	if (rc)
		return pending_abort(ctx, p, UBUS_STATUS_INVALID_ARGUMENT);
	return UBUS_STATUS_OK;
}

/*
 * Deleting a message is two operations, and while they are not equally urgent
 * they both have to be attempted:
 *
 *   the modem's copy  AT+CMGD=<index>.  This is the one that matters for the
 *                     device working tomorrow: the modem's store is what fills
 *                     up, and a full store is what stops new messages arriving.
 *                     A message dropped only from this daemon's list would go on
 *                     occupying its slot forever.
 *   our copy          the list entry, ALWAYS, including when the modem refused.
 *                     A page that keeps showing a message the user has just
 *                     deleted reads as a broken page.
 *
 * So the local delete runs regardless, and the reply reports the two separately
 * rather than folding them into one boolean.  A refusal on the modem (no card,
 * or the message is already gone from its store) is then visible for what it is
 * instead of being reported as "the delete failed" when the entry did vanish.
 */
struct sms_delete_ctx {
	struct pending_at *p;
	uint64_t id;
};

static void sms_delete_reply(struct sms_delete_ctx *d, enum at_status st,
			     const char *resp, bool tried_modem)
{
	struct blob_buf b = {};
	int local_ok = fm160_sms_delete_local(d->id) == 0;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "status", at_status_name(st));
	blobmsg_add_u8(&b, "modem_tried", tried_modem);
	blobmsg_add_u8(&b, "modem_deleted", tried_modem && st == AT_STATUS_OK);
	blobmsg_add_u8(&b, "local_deleted", local_ok);
	blobmsg_add_string(&b, "response", resp ? resp : "");
	ubus_send_reply(d->p->ctx, &d->p->req, b.head);
	blob_buf_free(&b);

	ubus_complete_deferred_request(d->p->ctx, &d->p->req, UBUS_STATUS_OK);
	fm160_sched_report_foreground();
	free(d->p);
	free(d);
}

static void sms_delete_cb(struct at_req *r, enum at_status st,
			  const char *resp, void *arg)
{
	(void)r;
	sms_delete_reply(arg, st, resp, true);
}

static int handle_sms_delete(struct ubus_context *ctx, struct ubus_object *obj,
			     struct ubus_request_data *req, const char *method,
			     struct blob_attr *msg)
{
	struct blob_attr *tb[__ATTR_SMS_ID_MAX];
	struct sms_delete_ctx *d;
	const struct fm160_sms_msg *m;
	uint64_t id;

	(void)obj;
	(void)method;

	blobmsg_parse(sms_id_policy, __ATTR_SMS_ID_MAX, tb,
		      msg ? blob_data(msg) : NULL, msg ? blob_len(msg) : 0);
	if (!tb[ATTR_SMS_ID])
		return UBUS_STATUS_INVALID_ARGUMENT;
	id = blobmsg_get_u64(tb[ATTR_SMS_ID]);

	m = fm160_sms_find(id);
	if (!m)
		return UBUS_STATUS_NOT_FOUND;

	d = calloc(1, sizeof(*d));
	if (!d)
		return UBUS_STATUS_UNKNOWN_ERROR;
	d->id = id;
	d->p = pending_begin(ctx, req);
	if (!d->p) {
		free(d);
		return UBUS_STATUS_UNKNOWN_ERROR;
	}

	/*
	 * The modem is asked only when it actually holds a copy and the feature
	 * is usable.  A message whose index is -1 was read from the modem and
	 * kept after the modem forgot it, so there is nothing there to remove;
	 * a modem that never accepted the setup has no storage to remove from.
	 * Both are still deletions the user asked for, so both carry on to the
	 * local delete - they just have no AT exchange in front of it.
	 */
	if (m->index < 0 || !fm160_sms_usable()) {
		sms_delete_reply(d, AT_STATUS_OK, "", false);
		return UBUS_STATUS_OK;
	}

	if (fm160_cmd_sms_delete(m->index, sms_delete_cb, d)) {
		int ret = pending_abort(ctx, d->p, UBUS_STATUS_UNKNOWN_ERROR);

		free(d);
		return ret;
	}
	return UBUS_STATUS_OK;
}

static int handle_sms_markread(struct ubus_context *ctx, struct ubus_object *obj,
			       struct ubus_request_data *req, const char *method,
			       struct blob_attr *msg)
{
	struct blob_attr *tb[__ATTR_SMS_ID_MAX];

	(void)obj;
	(void)method;

	blobmsg_parse(sms_id_policy, __ATTR_SMS_ID_MAX, tb,
		      msg ? blob_data(msg) : NULL, msg ? blob_len(msg) : 0);
	if (!tb[ATTR_SMS_ID])
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (fm160_sms_mark_read(blobmsg_get_u64(tb[ATTR_SMS_ID])))
		return UBUS_STATUS_NOT_FOUND;
	return UBUS_STATUS_OK;
}

/*
 * Ask the modem what it is holding.  This is the only SMS operation a button
 * starts directly, and it is the one that also retries the setup - a human
 * asking for messages is exactly the moment worth trying again.
 */
static int handle_sms_sync(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	struct blob_buf b = {};
	int rc;

	(void)obj;
	(void)method;
	(void)msg;

	if (!g_state.port_found)
		return UBUS_STATUS_NOT_FOUND;
	if (g_state.at_state == 2)
		return UBUS_STATUS_UNKNOWN_ERROR;

	rc = fm160_cmd_sms_sync();
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "result",
			   rc == 0 ? "started" :
			   rc == -EBUSY ? "busy" :
			   rc == -EAGAIN ? "not-ready" : "error");
	blobmsg_add_u8(&b, "usable", fm160_sms_usable());
	blobmsg_add_string(&b, "storage", g_state.sms.st.mem);
	blobmsg_add_string(&b, "detail", g_state.sms.st.last_error_text);
	ubus_send_reply(ctx, req, b.head);
	blob_buf_free(&b);
	return UBUS_STATUS_OK;
}

static const struct ubus_method fm160_methods[] = {
	UBUS_METHOD_NOARG("status",   handle_status),
	UBUS_METHOD("profile",   handle_profile,   profile_policy),
	UBUS_METHOD("at",        handle_at,        at_policy),
	UBUS_METHOD_NOARG("rescan",   handle_rescan),
	UBUS_METHOD_NOARG("ident",    handle_ident),
	UBUS_METHOD_NOARG("identity", handle_identity),
	UBUS_METHOD("enabled",   handle_enabled,   enabled_policy),
	/* M4.  Both defer, because both have to wait for a write and then a
	 * read-back before the answer means anything. */
	UBUS_METHOD("setbands",     handle_setbands,     bands_policy),
	UBUS_METHOD("setcelllock",  handle_setcelllock,  celllock_policy),
	/* M5.  Same deferred contract; "setgnss" is the only write in the whole
	 * object that is not stored by the module. */
	UBUS_METHOD("setgnss",      handle_setgnss,      gnss_policy),
	UBUS_METHOD("setgnsscfg",   handle_setgnsscfg,   constellation_policy),
	/* M3.  "sms_send" is the only deferred one: the rest answer from our
	 * own list and never touch the modem. */
	UBUS_METHOD_NOARG("sms_list",   handle_sms_list),
	UBUS_METHOD("sms_send",     handle_sms_send,     sms_send_policy),
	UBUS_METHOD("sms_delete",   handle_sms_delete,   sms_id_policy),
	UBUS_METHOD("sms_markread", handle_sms_markread, sms_id_policy),
	UBUS_METHOD_NOARG("sms_sync",   handle_sms_sync),
};

static struct ubus_object_type fm160_obj_type =
	UBUS_OBJECT_TYPE("fm160", fm160_methods);

static struct ubus_object fm160_obj = {
	.name = "fm160",
	.type = &fm160_obj_type,
	.methods = fm160_methods,
	.n_methods = ARRAY_SIZE(fm160_methods),
};

void fm160_ubus_methods_init(void)
{
	int ret;

	ret = ubus_add_object(g_ubus, &fm160_obj);
	if (ret)
		fm160_log(LOG_ERR, "failed to add ubus object: %s",
			  ubus_strerror(ret));
}
