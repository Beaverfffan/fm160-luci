/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * net - the data plane: the AT commands that build a PDP context, and the
 * parsers for what comes back.
 *
 * Same contract as pdu.c and usbmode.c, and for the same reason: this is the
 * half of M2 that can be settled without a modem.  Every function here is a
 * pure transformation - build a command string, or read a response - so the
 * host-side test can drive all of it, including the shapes a live modem only
 * produces when something has already gone wrong.
 *
 * That matters more here than usual.  This device has no SIM, so AT+CGDCONT
 * answers in 5.2 s with no data, AT+GTWWAN? has never returned an address, and
 * the ENTIRE success path is unreachable on the bench.  The parsers are
 * therefore written to be provable from the vendor's documented examples rather
 * than from what this modem happens to emit, and anything the documentation
 * leaves ambiguous is kept in raw form next to the decoded one.
 */

#ifndef FM160_NET_H
#define FM160_NET_H

#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* the dial ladder                                                      */
/* ------------------------------------------------------------------ */

/*
 * Where the dialer is.  One rung per thing that has to be true before the next
 * one can be attempted, in the order the vendor's dial-up document gives them
 * (3.5/3.6): card, registration, context, activation, address.
 */
enum fm160_net_step {
	NET_STEP_IDLE = 0,     /* not dialling                              */
	NET_STEP_PIN,          /* AT+CPIN?                                  */
	NET_STEP_REG,          /* AT+CREG? / AT+CGREG? / AT+CEREG?          */
	NET_STEP_APN,          /* AT+CGDCONT=                              */
	NET_STEP_ACTIVATE,     /* AT+GTWWAN=1,<cid> (or AT+GTRNDIS=1,<cid>)  */
	NET_STEP_IP,           /* AT+GTWWAN? until an address appears       */
	NET_STEP_UP,           /* netifd owns it now                        */
	NET_STEP_BUSY,         /* a transaction is in flight                */
	NET_STEP_FAILED,       /* this attempt gave up                      */
	NET_STEP_DOWN,         /* deliberately disconnected                 */
};

const char *fm160_net_step_name(enum fm160_net_step s);

#define FM160_NET_CMD_MAX   160
#define FM160_NET_APN_MAX   100
#define FM160_NET_ADDR_MAX  64
#define FM160_NET_CID_MAX   8      /* PDP contexts kept from AT+CGDCONT?  */
#define FM160_NET_CID_LIMIT 15     /* highest <cid> we will ever write    */

/* ------------------------------------------------------------------ */
/* the two verb families                                                */
/* ------------------------------------------------------------------ */

/*
 * The vendor contradicts itself about ECM.  The AT manual (11.1.15) says
 * ECM/RMNET use +GTWWAN and only RNDIS uses +GTRNDIS; the dial-up document
 * V1.0's own ECM chapter shows AT+GTRNDIS=1,1, FM160 example included.  So the
 * verb is probed at run time and cached, never hardcoded - which is why it is a
 * value here rather than being spelled into the command builders.
 */
enum fm160_net_verb {
	NET_VERB_GTWWAN = 0,
	NET_VERB_GTRNDIS,
};
#define FM160_NET_VERB_MAX 2

const char *fm160_net_verb_name(enum fm160_net_verb v);
/* "AT+GTWWAN=?" / "AT+GTRNDIS=?" when caps, else the bare read "AT+GTWWAN?" */
int fm160_net_probe_command(char *out, size_t outlen, enum fm160_net_verb v, bool caps);
/* "AT+GTWWAN=1,1" to bring <cid> up, "AT+GTWWAN=0,1" to take it down. */
int fm160_net_activate_command(char *out, size_t outlen, enum fm160_net_verb v,
			       int cid, bool up);

/* ------------------------------------------------------------------ */
/* PDP context                                                          */
/* ------------------------------------------------------------------ */

enum fm160_net_pdp {
	NET_PDP_IP = 0,
	NET_PDP_IPV6,
	NET_PDP_IPV4V6,
};

bool fm160_net_pdp_parse(const char *s, enum fm160_net_pdp *out);
const char *fm160_net_pdp_name(enum fm160_net_pdp p);

/*
 * AT+CGDCONT=1,"IP","cmnet"
 *
 * The APN is interpolated into a quoted AT argument, so its grammar is not a
 * formality: a quote or a comma in an accepted APN would end the argument early
 * and let the remainder be read as further AT+CGDCONT fields - i.e. an APN
 * string becomes a command.  Anything outside the APN character set is refused
 * rather than escaped, because there is no escaping to agree on and a refused
 * APN is a clear error while a mis-quoted one is a silent misconfiguration.
 */
bool fm160_net_apn_ok(const char *apn);
bool fm160_net_cid_ok(int cid);
int  fm160_net_apn_command(char *out, size_t outlen, int cid,
			   enum fm160_net_pdp pdp, const char *apn);

/* ------------------------------------------------------------------ */
/* parsers                                                              */
/* ------------------------------------------------------------------ */

/*
 * "+GTWWAN: 1,1,"IP","pdns","sdns"" (dial-up document 3.5)
 *
 * ⚠️ The field order is an ASSUMPTION.  The document only ever shows cid 1
 * active, so "<cid>,<status>" and "<status>,<cid>" produce the same line and
 * cannot be told apart from the documentation.  The reading taken here follows
 * the write syntax (AT+GTWWAN=<op>,<cid>), and the raw fields are kept beside
 * the decoded ones so the AT debug page can show what the modem really said -
 * on a machine with no SIM this parser has never seen a successful answer, and
 * saying so is better than implying it has been confirmed.
 */
struct fm160_net_wwan {
	bool valid;
	int  cid;
	int  active;            /* 1 the link is up, 0 not, -1 not reported */
	char pdp[8];
	char dns1[FM160_NET_ADDR_MAX];
	char dns2[FM160_NET_ADDR_MAX];
	/* The first field of the answer that is an address, whatever the modem
	 * calls it.  See the note about the assumed column order below. */
	bool has_addr;
	char addr[FM160_NET_ADDR_MAX];
	/* The fields verbatim, because the order is assumed (see above). */
	int  raw_n;
	char raw[6][FM160_NET_ADDR_MAX];
};

bool fm160_net_parse_wwan(const char *resp, struct fm160_net_wwan *st);

/*
 * AT+CGDCONT? -> one or more "+CGDCONT: <cid>,"<pdp>","<apn>",...".
 *
 * Two details the vendor's own transcripts show: there are five contexts on
 * this modem (1 ip, 2 ims, 3 cmnet, 4 cmwap, 5 sos), so "only one line" is
 * never a safe assumption; and a multi-line answer sometimes prints the
 * "+CGDCONT:" header on the first line only, leaving the rest bare.  Both forms
 * are accepted, which is why this returns a count rather than a bool.
 */
struct fm160_net_cgdcont {
	int  cid;
	char pdp[8];
	char apn[FM160_NET_APN_MAX];
};

int fm160_net_parse_cgdcont(const char *resp,
			    struct fm160_net_cgdcont *out, int max);

/*
 * "+CPIN: READY" -> 1.  A card that wants a PIN, or is absent, gives some other
 * word -> 0.  No line at all -> -1, which is a different answer: it means the
 * question was never answered, and treating that as "no card" would make the
 * dialer skip the one step that explains the failure.
 */
int fm160_net_parse_cpin(const char *resp);

/* "<state>" for one cid out of "+CGACT: <cid>,<state>" -> 1/0, or -1. */
int fm160_net_parse_cgact(const char *resp, int cid);

/*
 * The <stat> field of "+CREG: <n>,<stat>" and friends.
 *
 * token is "CREG", "CGREG", "CEREG" or "C5GREG".  Only the two-field form
 * carries <n>; the one-field form is "+CREG: 1".  Returns the stat, or
 * FM160_NET_REG_UNKNOWN.
 */
#define FM160_NET_REG_UNKNOWN 99
int fm160_net_parse_reg(const char *resp, const char *token);

/* ------------------------------------------------------------------ */
/* the reconnect ladder                                                 */
/* ------------------------------------------------------------------ */

/*
 * DESIGN 4.3: retry at once, then 5 s, 15 s, 60 s, 300 s, and stay at 300 s.
 *
 * It is a table rather than c << n because the rungs are the design's numbers,
 * and a reader checking them against the document should not have to compute
 * anything.  attempt is 0-based; anything past the end of the table returns the
 * last rung rather than growing without bound.
 */
#define FM160_NET_LADDER_MAX 5
int fm160_net_ladder_len(void);
int fm160_net_backoff_s(int attempt);
int fm160_net_backoff_ms(int attempt);

/*
 * DESIGN 4.3: module resets are counted persistently, and once the count in the
 * window reaches the limit the daemon stops healing itself and says so.  A
 * daemon that keeps resetting a modem forever is worse than one that gives up:
 * it turns a broken card into a router that reboots its modem every five
 * minutes, and it hides the fault instead of reporting it.
 */
#define FM160_NET_RESET_LIMIT    3
#define FM160_NET_RESET_WINDOW_S 86400
bool fm160_net_reset_allowed(int resets_in_window);
const char *fm160_net_reset_refusal(void);

/* True only for something worth handing to netifd as an address: a dotted quad
 * with every octet 0-255 and no padding, or a colon-bearing hex group string.
 * An address that fails this is reported as "no address yet" rather than
 * configured, because netifd's failure mode with a bad address is an interface
 * that looks up and carries nothing. */
bool fm160_net_addr_ok(const char *s);

#endif /* FM160_NET_H */
