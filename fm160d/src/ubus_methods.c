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

static void manual_at_cb(struct at_req *r, enum at_status status,
			 const char *response, void *arg)
{
	struct pending_at *p = arg;
	struct blob_buf b = {};
	const char *name;

	switch (status) {
	case AT_STATUS_OK:      name = "ok";      break;
	case AT_STATUS_ERROR:   name = "error";   break;
	case AT_STATUS_TIMEOUT: name = "timeout"; break;
	case AT_STATUS_NOPORT:  name = "no_port"; break;
	case AT_STATUS_BUSY:    name = "busy";    break;
	default:                name = "unknown"; break;
	}

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "status", name);
	blobmsg_add_string(&b, "command", r->cmd);
	blobmsg_add_string(&b, "response", response ? response : "");
	ubus_send_reply(p->ctx, &p->req, b.head);
	blob_buf_free(&b);

	ubus_complete_deferred_request(p->ctx, &p->req, UBUS_STATUS_OK);

	/* A user-issued command is also how the UI says "I am here": keep the
	 * foreground window open so the polling tiers stay warm for a moment. */
	fm160_sched_report_foreground();

	free(p);
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

static const struct ubus_method fm160_methods[] = {
	UBUS_METHOD_NOARG("status",   handle_status),
	UBUS_METHOD("profile",   handle_profile,   profile_policy),
	UBUS_METHOD("at",        handle_at,        at_policy),
	UBUS_METHOD_NOARG("rescan",   handle_rescan),
	UBUS_METHOD_NOARG("ident",    handle_ident),
	UBUS_METHOD_NOARG("identity", handle_identity),
	UBUS_METHOD("enabled",   handle_enabled,   enabled_policy),
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
