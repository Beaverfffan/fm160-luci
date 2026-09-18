/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmds.c - FM160 command layer: builders and response parsers.
 *
 * Command set and semantics come from the Fibocom documents; see
 * docs/AT-FACTS.md for the page references behind every choice here.
 *
 * Nothing in this file ever hardcodes a modem-reported capability: enumerations
 * that the manual calls "device dependent" (USB modes, bands) are always read
 * back with the =? / ? forms before being used.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "fm160d.h"

/* ------------------------------------------------------------------ */
/* small CSV helpers                                                    */
/* ------------------------------------------------------------------ */

static int csv_int(const char *s, int idx, int fallback)
{
	const char *p = s;
	int i = 0;

	while (i < idx) {
		p = strchr(p, ',');
		if (!p)
			return fallback;
		p++;
		i++;
	}
	if (!*p)
		return fallback;
	return atoi(p);
}

/* Pointer to field <idx> of a CSV line, or "" when the line is shorter. */
static const char *csv_at(const char *s, int idx)
{
	const char *p = s;
	int i = 0;

	if (!s)
		return "";
	while (i < idx) {
		p = strchr(p, ',');
		if (!p)
			return "";
		p++;
		i++;
	}
	return p;
}

/* <tac>, <cell_id>, <earfcn> and <physicalcellId> are printed as hex with
 * no 0x prefix; everything else in the same row is decimal.
 *
 *   AT+GTCCINFO?  ->  1,4,460,01,4F50,52BF337,672,1E4,103,100,44,63,63,24
 *                     TAC 0x4F50 = 20304, cell 0x52BF337 = 86725431,
 *                     EARFCN 0x672 = 1650, PCI 0x1E4 = 484
 *
 * (measured on FM160-CN 89614.1000.00.04.01.02).  The manual never states the
 * radix, but its "range is 0-0xFFFFFFF / 0-0xFFFFFFFF" wording gives it away,
 * and a decimal parse of "1E4" yields 1, which is how this was caught. */
static long long csv_hex_ll(const char *s, int idx, long long fallback)
{
	const char *p = s;
	int i = 0;

	while (i < idx) {
		p = strchr(p, ',');
		if (!p)
			return fallback;
		p++;
		i++;
	}
	if (!*p)
		return fallback;
	return strtoll(p, NULL, 16);
}

/*
 * Band number decoding.
 *
 * AT+GTACT (AT manual 9.1.14) does NOT use a uniform offset scheme; the
 * <lte_band>/<nr_band> values are built by prefixing a RAT prefix to the band
 * number, which makes them look like decimal numbers:
 *
 *   101     BAND_LTE_1        100 + n                  -> 101 .. 164
 *   501     BAND_NR_1         501 .. 509   (n = 1 digit)
 *   5010    BAND_NR_10        the manual prints 5010, i.e. NOT 500 + 10
 *   5078    BAND_NR_78        (manual example: AT+GTACT=,,,103,5078)
 *   50512   BAND_NR_512       -> three-digit bands are 50100 .. 50512
 *
 * A naive `band - 500` therefore turns NR n78 into 4578.  AT+GTCCINFO? names
 * the same enum constants (BAND_LTE_1..BAND_LTE_64, BAND_NR_1..BAND_NR_512).
 *
 * Hardware confirms both commands use the prefixed form, so the encoder is
 * shared: `AT+GTACT?` reports 101,103,105,108,134,138,139,140,141 for LTE and
 * 501,5028,5041,5078,5079 for NR (= B1/B3/B5/B8/B34/B38/B39/B40/B41 and
 * n1/n28/n41/n78/n79), while `AT+GTCCINFO?` reports band 103 for the serving
 * cell it describes as B3.  fm160_band_decode() still also accepts a bare band
 * number, because <band> is documented as BAND_INVALID (0) whenever the modem
 * is not registered and the firmware could report either form.
 */
int fm160_band_decode(int rat, int raw)
{
	if (raw <= 0)
		return 0;

	switch (rat) {
	case 4:                                   /* LTE */
		if (raw >= 101 && raw <= 164)     /* BAND_LTE_1..64  */
			return raw - 100;
		if (raw <= 64)                    /* already bare    */
			return raw;
		return 0;
	case 9:                                   /* NR */
		if (raw >= 50100 && raw <= 50999) /* BAND_NR_100..999 */
			return raw - 50000;
		if (raw >= 5010 && raw <= 5099)   /* BAND_NR_10..99   */
			return raw - 5000;
		if (raw >= 501 && raw <= 509)     /* BAND_NR_1..9     */
			return raw - 500;
		if (raw <= 512)                   /* already bare     */
			return raw;
		return 0;
	case 2:                                   /* WCDMA / UMTS     */
		if (raw <= 25)                    /* BAND_UMTS_I..XXV */
			return raw;
		return 0;
	default:
		return 0;
	}
}

/*
 * Which RAT does a raw band token belong to?
 *
 * AT+GTACT? hands back ONE flat band list that mixes all RATs, e.g.
 *   20,6,3,1,8,101,103,105,108,134,138,139,140,141,501,5028,5041,5078,5079
 * where 1,8 are UMTS, the 101..141 run is LTE and the 501.. run is NR.  The
 * only way to tell them apart is the encoding itself, so this is the inverse of
 * fm160_band_decode() and the two must agree.
 *
 * The three NR ranges are disjoint from each other and from LTE, which is what
 * makes the classification unambiguous:
 *   501..509         n1..n9        (501 = 500 + 1)
 *   5010..5099       n10..n99      (5028 = 5000 + 28)
 *   50100..50999     n100..n999    (50100 = 50000 + 100)
 *   101..499         B1..B399      (103 = 100 + 3; GTCAINFO advertises up to 171)
 *   1..25            UMTS I..XXV
 *
 * A bare 0 is AT+GTACT's "automatic band selection" marker, not a band, and
 * anything else (e.g. 510..5009) is not in any documented scheme: it is
 * reported as unknown rather than guessed into a RAT.
 */
int fm160_band_rat_of(int raw)
{
	if (raw <= 0)
		return 0;                        /* 0 = auto / invalid */
	if (raw >= 501 && raw <= 509)
		return 9;
	if (raw >= 5010 && raw <= 5099)
		return 9;
	if (raw >= 50100 && raw <= 50999)
		return 9;
	if (raw >= 101 && raw <= 499)
		return 4;
	if (raw <= 25)
		return 2;
	return 0;
}

/* <bandwidth>: LTE is a resource-block count, NR is a plain MHz code whose 0
 * means 5 MHz - two different tables for the same column. */
static int decode_bandwidth_mhz(int rat, int raw)
{
	static const int lte_rb[6]   = { 6, 15, 25, 50, 75, 100 };
	static const int lte_mhz[6]  = { 1, 3,  5,  10, 15, 20  };
	static const int nr_code[13] = { 0, 10, 15, 20, 25, 30, 40, 50, 60, 80, 90, 100, 200 };
	static const int nr_mhz[13]  = { 5, 10, 15, 20, 25, 30, 40, 50, 60, 80, 90, 100, 200 };
	unsigned i;

	if (raw <= 0)
		return FM160_NONE;

	if (rat == 4) {
		for (i = 0; i < ARRAY_SIZE(lte_rb); i++)
			if (lte_rb[i] == raw)
				return lte_mhz[i];
		return FM160_NONE;
	}
	if (rat == 9) {
		for (i = 0; i < ARRAY_SIZE(nr_code); i++)
			if (nr_code[i] == raw)
				return nr_mhz[i];
		if (raw == 400)
			return 400;
		return FM160_NONE;
	}
	return FM160_NONE;
}

/* <rssnr_value> (LTE) is dB = raw/2; <ss-sinr> (NR) is dB = -23 + raw/2. */
static int decode_sinr_db10(int rat, int raw)
{
	if (raw == 255 || raw < -100 || raw > 127)
		return FM160_NONE;
	if (rat == 4)
		return raw * 5;
	if (rat == 9)
		return -230 + raw * 5;
	return FM160_NONE;
}

/* <rsrp> (LTE) is dBm = -140 + raw; <ss-rsrp> (NR) is dBm = -156 + raw. */
static int decode_rsrp_dbm(int rat, int raw)
{
	if (raw == 255)
		return FM160_NONE;
	if (rat == 4 && raw <= 97)
		return -140 + raw;
	if (rat == 9 && raw <= 126)
		return -156 + raw;
	return FM160_NONE;
}

/* <rsrq> (LTE) is dB = -19.5 + raw/2; <ss-rsrq> (NR) is dB = -43 + raw/2. */
static int decode_rsrq_db10(int rat, int raw)
{
	if (raw == 255)
		return FM160_NONE;
	if (rat == 4 && raw <= 34)
		return -195 + raw * 5;
	if (rat == 9 && raw <= 126)
		return -430 + raw * 5;
	return FM160_NONE;
}

/* ------------------------------------------------------------------ */
/* parsers                                                              */
/* ------------------------------------------------------------------ */

/*
 * AT+CSQ -> +CSQ: <rssi>,<ber>   (AT manual 9.1.1)
 *   rssi 0 = -113 dBm or less, 31 = -51 dBm or greater, 99 = unknown.
 *
 * CAUTION: the manual adds "When act=11 or 13, and set AT+GTCSQNREN=1, the rssi
 * replaced by ss_rsrp (0-126)".  In that configuration the first field is no
 * longer an RSSI and the -113+2n formula would report nonsense.
 *
 * AT+GTCSQNREN has no section of its own in the manual (only this one mention),
 * so rather than poking an undocumented command we detect the situation for
 * free: a value above 31 that is not the 99 "unknown" code can only be an
 * ss_rsrp reading.  No extra AT traffic, and the wrong number is never shown.
 */
void fm160_parse_csq(const char *resp)
{
	char line[FM160_STR_MAX];
	int rssi;

	if (!fm160_resp_find(resp, "+CSQ", line, sizeof(line)))
		return;

	rssi = csv_int(line, 0, 99);
	g_state.csq_rssi = rssi;
	g_state.csq_ber = csv_int(line, 1, 99);

	if (rssi > 31 && rssi != 99) {
		g_state.csq_is_ss_rsrp = true;
		g_state.nr_ss_rsrp_dbm = rssi <= 126 ? -156 + rssi : FM160_NONE;
	} else {
		g_state.csq_is_ss_rsrp = false;
	}
}

/*
 * AT+CESQ -> +CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>,
 *                   <ss_rsrq>,<ss_rsrp>,<ss_sinr>          (AT manual 9.1.2)
 *
 * Nine fields, not six.  Fields that do not apply to the current serving cell
 * are reported as 255 (99 for <rxlev>/<ber>), which is why every decoder below
 * has an explicit validity check and the decoded value has a sentinel.
 */
void fm160_parse_cesq(const char *resp)
{
	char line[FM160_STR_MAX];

	if (!fm160_resp_find(resp, "+CESQ", line, sizeof(line)))
		return;

	g_state.cesq_rxlev   = csv_int(line, 0, 99);
	g_state.cesq_ber     = csv_int(line, 1, 99);
	g_state.cesq_rscp    = csv_int(line, 2, 255);
	g_state.cesq_ecno    = csv_int(line, 3, 255);
	g_state.cesq_rsrq    = csv_int(line, 4, 255);
	g_state.cesq_rsrp    = csv_int(line, 5, 255);
	g_state.cesq_ss_rsrq = csv_int(line, 6, 255);
	g_state.cesq_ss_rsrp = csv_int(line, 7, 255);
	g_state.cesq_ss_sinr = csv_int(line, 8, 255);

	g_state.geran_rssi_dbm = g_state.cesq_rxlev <= 63 ?
				 -110 + g_state.cesq_rxlev : FM160_NONE;
	g_state.utra_rscp_dbm  = g_state.cesq_rscp <= 96 ?
				 -120 + g_state.cesq_rscp : FM160_NONE;
	g_state.utra_ecno_db10 = g_state.cesq_ecno <= 49 ?
				 -240 + g_state.cesq_ecno * 5 : FM160_NONE;
	g_state.lte_rsrp_dbm   = g_state.cesq_rsrp <= 97 ?
				 -140 + g_state.cesq_rsrp : FM160_NONE;
	g_state.lte_rsrq_db10  = g_state.cesq_rsrq <= 34 ?
				 -195 + g_state.cesq_rsrq * 5 : FM160_NONE;
	g_state.nr_ss_rsrp_dbm = g_state.cesq_ss_rsrp <= 126 ?
				 -156 + g_state.cesq_ss_rsrp : FM160_NONE;
	g_state.nr_ss_rsrq_db10 = g_state.cesq_ss_rsrq <= 126 ?
				  -430 + g_state.cesq_ss_rsrq * 5 : FM160_NONE;
	g_state.nr_ss_sinr_db10 = g_state.cesq_ss_sinr <= 127 ?
				  -230 + g_state.cesq_ss_sinr * 5 : FM160_NONE;
}

void fm160_parse_greg(const char *resp, const char *prefix, int *slot)
{
	char line[FM160_STR_MAX];
	char pat[16];

	snprintf(pat, sizeof(pat), "+%s", prefix);
	if (!fm160_resp_find(resp, pat, line, sizeof(line)))
		return;
	/* +CxREG: <n>,<stat>[,...] -- stat is always field 1 */
	*slot = csv_int(line, 1, FM160_REG_UNKNOWN);
}

void fm160_parse_usbmode(const char *resp)
{
	char line[FM160_STR_MAX];

	if (fm160_resp_find(resp, "+GTUSBMODE", line, sizeof(line)))
		g_state.usb_mode = csv_int(line, 0, -1);
}

/* Column index map for one GTCCINFO row; -1 = the column does not exist in
 * that variant.  See the struct fm160_cell comment for the source tables. */
struct cc_cols {
	int band, bw, sinr, rxlev, rsrp, rsrq, rscp, ecno;
};

void fm160_parse_ccinfo(const char *resp)
{
	struct fm160_cell svc[2];
	int svc_n = 0;
	int neigh = 0;
	const char *p;

	memset(svc, 0, sizeof(svc));

	/* GTCCINFO answers with a labelled multi-line block:
	 *
	 *   +GTCCINFO:
	 *   LTE service cell:
	 *   1,4,460,01,4F50,52BF337,672,1E4,103,100,44,63,63,24
	 *   LTE neighbor cell:
	 *
	 * (captured on FM160-CN 89614.1000.00.04.01.02).  The row labels on real
	 * hardware are shorter than the manual's "LTE/eMTC/NB-IoT service cell:",
	 * and EN-DC puts two differently-shaped rows under one single
	 * "LTE-NR EN-DC service cell:" label, so the label is used only to skip
	 * lines: the layout comes from the row's own <IsServiceCell>,<rat>, which
	 * are identical in every variant.  A data row always starts with a digit;
	 * every label, blank line and the trailing OK do not.
	 *
	 * fm160_resp_lines() is unusable here -- it only returns text sitting on
	 * the same line as the prefix, and "+GTCCINFO:" occupies a line of its
	 * own, so on hardware it silently produced zero rows. */
	if (!resp)
		return;
	p = strstr(resp, "+GTCCINFO");
	if (!p) {
		fm160_log(LOG_DEBUG, "GTCCINFO: no data line");
		return;
	}
	p = strchr(p, '\n');
	if (!p)
		return;
	p++;

	while (*p) {
		char line[FM160_STR_MAX];
		const char *eol = p + strcspn(p, "\r\n");
		size_t len = (size_t)(eol - p);
		int is_service, rat;
		struct cc_cols c;
		struct fm160_cell *cell;

		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = '\0';

		/* Advance first: several branches below use `continue`. */
		p = *eol ? eol + 1 : eol;

		if (line[0] < '0' || line[0] > '9')
			continue;         /* row label, blank line, OK, ERROR */

		is_service = csv_int(line, 0, 0);
		rat = csv_int(line, 1, 0);

		if (rat != 2 && rat != 4 && rat != 9)
			continue;

		if (is_service == 1) {
			switch (rat) {
			case 4: case 9:      /* LTE / NR service */
				c = (struct cc_cols){ 8, 9, 10, 11, 12, 13, -1, -1 };
				break;
			case 2:              /* WCDMA service  */
				c = (struct cc_cols){ 8, -1, -1, 12, -1, -1, 10, 9 };
				break;
			default:
				continue;
			}
			if (svc_n >= 2)
				continue;         /* more than 2 service cells: ignore */
			cell = &svc[svc_n++];
		} else {
			switch (rat) {
			case 4:              /* LTE neighbour  */
				c = (struct cc_cols){ -1, 8, -1, 9, 10, 11, -1, -1 };
				break;
			case 9:              /* NR neighbour: 8 is ss-sinr! */
				c = (struct cc_cols){ -1, -1, 8, 9, 10, 11, -1, -1 };
				break;
			case 2:              /* WCDMA neighbour */
				c = (struct cc_cols){ -1, -1, -1, 13, -1, -1, 14, 11 };
				break;
			default:
				continue;
			}
			if (neigh >= (int)ARRAY_SIZE(g_state.neigh))
				continue;
			cell = &g_state.neigh[neigh++];
		}

		memset(cell, 0, sizeof(*cell));
		cell->valid = true;
		cell->is_service = (is_service == 1);
		cell->rat = rat;
		cell->mcc = csv_int(line, 2, 0);
		cell->mnc = csv_int(line, 3, 0);
		/* <lac>/<tac>, <cell_id>, <arfcn>/<earfcn>/<narfcn> and
		 * <physicalcellId> are printed in hex; <mcc>/<mnc> are decimal.
		 * See csv_hex_ll() for the hardware evidence. */
		cell->tac = csv_hex_ll(line, 4, 0);
		cell->cellid = csv_hex_ll(line, 5, 0);
		cell->earfcn = csv_hex_ll(line, 6, 0);
		cell->pci = (int)csv_hex_ll(line, 7, 0);
		cell->band = c.band >= 0 ?
			fm160_band_decode(rat, csv_int(line, c.band, 0)) : 0;

		cell->bandwidth_raw = c.bw >= 0 ? csv_int(line, c.bw, -1) : -1;
		cell->bandwidth_mhz = decode_bandwidth_mhz(rat, cell->bandwidth_raw);

		cell->sinr_raw = c.sinr >= 0 ? csv_int(line, c.sinr, 255) : 255;
		cell->sinr_db10 = decode_sinr_db10(rat, cell->sinr_raw);

		cell->rxlev_raw = c.rxlev >= 0 ? csv_int(line, c.rxlev, 255) : 255;

		cell->rsrp_raw = c.rsrp >= 0 ? csv_int(line, c.rsrp, 255) : 255;
		cell->rsrp_dbm = decode_rsrp_dbm(rat, cell->rsrp_raw);

		cell->rsrq_raw = c.rsrq >= 0 ? csv_int(line, c.rsrq, 255) : 255;
		cell->rsrq_db10 = decode_rsrq_db10(rat, cell->rsrq_raw);

		/* WCDMA has no RSRP/RSRQ; keep rscp/ecno in their own fields. */
		if (rat == 2) {
			int rscp = c.rscp >= 0 ? csv_int(line, c.rscp, 255) : 255;

			cell->rsrp_raw = rscp;
			cell->rsrp_dbm = rscp <= 96 ? -120 + rscp : FM160_NONE;
		}
	}

	g_state.neigh_count = neigh;

	if (!svc_n) {
		/* No service cell in this response: keep old data, but do not claim
		 * a fresh reading. */
		fm160_log(LOG_DEBUG, "GTCCINFO: %d neighbour row(s), no service cell",
			  neigh);
		g_state.serving.valid = false;
		g_state.serving2.valid = false;
		return;
	}

	/* EN-DC reports two service rows (LTE anchor + NR).  Show the highest RAT
	 * as the headline and keep the other one next to it. */
	if (svc_n == 2 && svc[0].rat < svc[1].rat) {
		g_state.serving  = svc[1];
		g_state.serving2 = svc[0];
	} else {
		g_state.serving  = svc[0];
		g_state.serving2 = svc_n == 2 ? svc[1] : (struct fm160_cell){ 0 };
	}
	g_state.cell_last_ok_ms = fm160_now_ms();
}

/* ------------------------------------------------------------------ */
/* identity chain                                                       */
/* ------------------------------------------------------------------ */

static int ident_step;

static void ident_store(struct at_req *req, enum at_status status,
			const char *response, void *arg);

enum ident_kind {
	IDENT_STR = 0,        /* copy the first value line into dst          */
	IDENT_USBMODE,        /* +GTUSBMODE: <n>                             */
	IDENT_GTACT_CAPS,     /* +GTACT: (…),(…) x9                          */
	IDENT_CELLLOCK_CAPS,  /* +GTCELLLOCK: ranges x7                      */
};

struct ident_item {
	const char *cmd;
	char *dst;
	size_t dstlen;
	enum ident_kind kind;
};

/*
 * Read once per boot, in order.  Nothing here is polled: these either cannot
 * change while the daemon runs (identity, capability lists) or are handled by
 * a dedicated tier (see sched.c).
 *
 * The two "=?" capability reads belong here rather than in a poll tier for a
 * sharper reason than cost: they are the *permission* for the M4 write path.
 * fm160_cmd_set_bands()/fm160_cmd_set_celllock() refuse to send anything until
 * the corresponding enumeration has succeeded, so a module that will not tell
 * us what it supports never gets written to.
 */
static struct ident_item ident_plan[] = {
	{ "AT+CGMI",       g_state.manufacturer, sizeof(g_state.manufacturer), IDENT_STR },
	{ "AT+CGMM",       g_state.model,        sizeof(g_state.model),        IDENT_STR },
	{ "AT+CGMR",       g_state.revision,     sizeof(g_state.revision),     IDENT_STR },
	{ "AT+CGSN",       g_state.imei,         sizeof(g_state.imei),         IDENT_STR },
	{ "AT+CFSN",       g_state.sn,           sizeof(g_state.sn),           IDENT_STR },
	/* AT+ICCID, not AT+CCID: on FM160-CN 89614.1000.00.04.01.02 a card-less
	 * AT+CCID blocks for the full 10 s timeout, while AT+ICCID answers in
	 * 18 ms (+CME ERROR: 13 without a card).  This is the same preference
	 * QModem's fibocom.sh encodes when it tries ICCID first and only falls
	 * back to CCID.
	 *
	 * Caveat, recorded deliberately: the AT manual lists AT+CCID (3.1.12)
	 * but NOT AT+ICCID, so ICCID is apparently an unlisted alias.  It is safe
	 * here because (a) hardware proves the command is understood - it answers
	 * +CME ERROR rather than bare ERROR - and (b) identity is read once per
	 * boot and never polled, so a missing alias costs a "-" in the UI rather
	 * than a stalled queue.  If it ever regresses, fall back to AT+CCID behind
	 * a long timeout inside the quiet window, never in the poll loop. */
	{ "AT+ICCID",      g_state.iccid,        sizeof(g_state.iccid),        IDENT_STR },
	{ "AT+GTUSBMODE?", NULL,                 0,                            IDENT_USBMODE },
	/* M4.  Both answer in 21 ms / 16 ms on the live module. */
	{ "AT+GTACT=?",    NULL,                 0,                            IDENT_GTACT_CAPS },
	{ "AT+GTCELLLOCK=?", NULL,               0,                            IDENT_CELLLOCK_CAPS },
};

static const char *first_value_line(const char *resp)
{
	static char buf[FM160_STR_MAX];
	const char *p, *e;

	buf[0] = '\0';
	if (!resp)
		return buf;

	/* Skip the echoed command and any blank lines; take the first line that
	 * is neither blank nor a bare result code. */
	p = resp;
	while (*p) {
		size_t len;

		while (*p == '\r' || *p == '\n')
			p++;
		if (!*p)
			break;
		e = p;
		while (*e && *e != '\r' && *e != '\n')
			e++;
		len = (size_t)(e - p);
		if (!(len == 2 && !strncmp(p, "OK", 2)) &&
		    !(len >= 5 && !strncmp(p, "ERROR", 5)) &&
		    !(len >= 3 && !strncmp(p, "AT+", 3)) &&
		    !(len >= 3 && !strncmp(p, "at+", 3))) {
			/* Strip a leading "+PREFIX:" when the command puts one
			 * there.  AT+CFSN answers `+CFSN: "FP62PE002F"` while
			 * AT+CGMI answers a bare string, and without this the UI
			 * showed the whole protocol line as the serial number --
			 * observed on the board as sn = "+CFSN: \"FP62PE002F\"".
			 * Applied generically rather than naming +CFSN, because
			 * AT+ICCID/+CCID are the same shape. */
			if (*p == '+') {
				const char *c = p;

				while (c < e && *c != ':')
					c++;
				if (c < e) {
					p = c + 1;
					while (p < e && (*p == ' ' || *p == '\t'))
						p++;
					len = (size_t)(e - p);
				}
			}
			/* ...then the quotes a few commands wrap the value in. */
			if (len >= 2 && *p == '"' && p[len - 1] == '"') {
				p++;
				len -= 2;
			}
			if (len >= FM160_STR_MAX)
				len = FM160_STR_MAX - 1;
			memcpy(buf, p, len);
			buf[len] = '\0';
			return buf;
		}
		p = e;
	}
	return buf;
}

static void ident_next(void)
{
	if (ident_step >= (int)ARRAY_SIZE(ident_plan)) {
		g_state.ident_done = true;
		fm160_log(LOG_INFO, "identity: %s %s fw=%s imei=%s mode=%d",
			  g_state.manufacturer, g_state.model, g_state.revision,
			  g_state.imei, g_state.usb_mode);
		fm160_log(LOG_INFO, "boot read-outs: gtact caps=%s celllock caps=%s",
			  g_state.gtact_caps.valid ? "ok" : "unavailable",
			  g_state.celllock_caps.valid ? "ok" : "unavailable");
		fm160_state_mark_dirty();
		fm160_state_publish();
		return;
	}

	atq_submit(AT_PRIO_STATE, ident_plan[ident_step].cmd, NULL, 3000,
		   ident_store, NULL);
}

static void ident_store(struct at_req *req, enum at_status status,
			const char *response, void *arg)
{
	struct ident_item *it = &ident_plan[ident_step];

	if (status == AT_STATUS_OK) {
		switch (it->kind) {
		case IDENT_USBMODE:
			fm160_parse_usbmode(response);
			break;
		case IDENT_GTACT_CAPS:
			fm160_parse_gtact_caps(response);
			break;
		case IDENT_CELLLOCK_CAPS:
			fm160_parse_celllock_caps(response);
			break;
		case IDENT_STR:
		default:
			if (it->dst)
				snprintf(it->dst, it->dstlen, "%s",
					 first_value_line(response));
			break;
		}
	} else {
		fm160_log(LOG_DEBUG, "%s failed (status %d)", it->cmd, status);
	}

	ident_step++;
	ident_next();
}

void fm160_cmd_ident_start(void)
{
	ident_step = 0;
	fm160_ident_reset();
	fm160_log(LOG_INFO, "reading module identity");
	ident_next();
}

void fm160_ident_reset(void)
{
	g_state.manufacturer[0] = '\0';
	g_state.model[0] = '\0';
	g_state.revision[0] = '\0';
	g_state.imei[0] = '\0';
	g_state.sn[0] = '\0';
	g_state.iccid[0] = '\0';
	g_state.usb_mode = -1;
	g_state.ident_done = false;

	/* Capabilities are invalidated with the identity they belong to.  If the
	 * re-read fails the M4 write path is blocked, which is the safe
	 * direction: writing a persistent setting to a module that just failed
	 * to describe itself is worse than a temporarily greyed-out button. */
	g_state.gtact_caps.valid = false;
	g_state.celllock_caps.valid = false;
}

/* ------------------------------------------------------------------ */
/* periodic polls                                                       */
/* ------------------------------------------------------------------ */

static void reg_cb_csq(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_csq(resp);
}

static void reg_cb_cereg(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_greg(resp, "CEREG", &g_state.cereg);
}

static void reg_cb_c5greg(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_greg(resp, "C5GREG", &g_state.c5greg);
}

void fm160_cmd_poll_reg(void)
{
	/* Three cheap queries per tier tick.  AT+CSQ carries RSSI, the two
	 * registration queries are the only reliable way to know whether the
	 * modem is on an LTE or an NR cell (FM160 is NR-capable). */
	atq_submit(AT_PRIO_POLL, "AT+CSQ",    NULL, 3000, reg_cb_csq,    NULL);
	atq_submit(AT_PRIO_POLL, "AT+CEREG?", NULL, 3000, reg_cb_cereg,  NULL);
	atq_submit(AT_PRIO_POLL, "AT+C5GREG?", NULL, 3000, reg_cb_c5greg, NULL);
}

static void sig_cb_cesq(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_cesq(resp);
}

void fm160_cmd_poll_signal(void)
{
	atq_submit(AT_PRIO_POLL, "AT+CESQ", NULL, 3000, sig_cb_cesq, NULL);
}

static void cell_cb(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_ccinfo(resp);
	else
		fm160_log(LOG_DEBUG, "GTCCINFO failed (status %d)", s);
}

void fm160_cmd_poll_cell(void)
{
	/* One command returns the serving cell plus up to ten neighbours with
	 * RSRP/RSRQ/SINR - by far the best value per AT transaction. */
	atq_submit(AT_PRIO_POLL, "AT+GTCCINFO?", NULL, 8000, cell_cb, NULL);
}

/* ================================================================== */
/* M4: band lock / cell lock / carrier aggregation                      */
/* ================================================================== */

/*
 * The three commands behind this milestone have one property that shapes every
 * decision below: they are all Persistent = Yes, i.e. they end up in an EFS
 * file on the module.  AT+GTCELLLOCK additionally does not take effect until
 * the UE is reset, and the manual forbids combining it with
 * GTFREQLOCK / COPS / GTACT / GTCELLLOCK / GTRAT.
 *
 * So the write path here is deliberately narrow:
 *   - nothing is written before the corresponding "=?" enumeration succeeded
 *     (a modem that will not tell us what it supports does not get written to);
 *   - the value is written, then read back, and only the read-back is reported
 *     as the new state;
 *   - the module is never reset by this daemon.  A cell lock needs a reset to
 *     take effect, and that is a decision for the user, not for a poll loop.
 */

/* Split "(1,2),(3),( )" into its groups, parens and padding removed. */
static int split_groups(const char *s, char out[][FM160_RESP_LINE_MAX], int max)
{
	int n = 0;
	const char *p = s;

	if (!s)
		return 0;

	while (*p && n < max) {
		const char *open, *close;
		size_t len, b, e;

		open = strchr(p, '(');
		if (!open)
			break;
		close = strchr(open, ')');
		if (!close)
			break;

		/* Trim the spaces the modem pads empty groups with: "( )". */
		b = 1;
		e = (size_t)(close - open);
		while (b < e && (open[b] == ' ' || open[b] == '\t'))
			b++;
		while (e > b && (open[e - 1] == ' ' || open[e - 1] == '\t'))
			e--;

		len = e - b;
		if (len >= FM160_RESP_LINE_MAX)
			len = FM160_RESP_LINE_MAX - 1;
		memcpy(out[n], open + b, len);
		out[n][len] = '\0';
		n++;
		p = close + 1;
	}
	return n;
}

/* "1,8,101" -> { 1, 8, 101 }; skips blank tokens, stops at max. */
static int parse_int_list(const char *csv, int *out, int max)
{
	int n = 0;
	const char *p = csv;

	if (!csv)
		return 0;
	while (*p && n < max) {
		const char *e = p + strcspn(p, ",");
		char tmp[32];
		size_t len = (size_t)(e - p);

		if (len >= sizeof(tmp))
			len = sizeof(tmp) - 1;
		memcpy(tmp, p, len);
		tmp[len] = '\0';
		if (tmp[0] && tmp[0] != ' ')
			out[n++] = atoi(tmp);

		if (!*e)
			break;
		p = e + 1;
	}
	return n;
}

/*
 * Parse a band list into an array.  forced_rat != 0 is used for the AT+GTACT=?
 * groups, where the group index already says which RAT it is; forced_rat == 0
 * is used for the flat AT+GTACT? list, where the encoding itself decides.
 * Tokens that fit no scheme are counted in *unknown, never guessed.
 */
static int parse_band_list(const char *csv, struct fm160_band *out, int max,
			   int forced_rat, int *unknown)
{
	int n = 0;
	const char *p = csv;

	if (!csv)
		return 0;
	while (*p && n < max) {
		const char *e = p + strcspn(p, ",");
		char tmp[32];
		size_t len = (size_t)(e - p);
		int raw, rat;

		if (len >= sizeof(tmp))
			len = sizeof(tmp) - 1;
		memcpy(tmp, p, len);
		tmp[len] = '\0';

		if (tmp[0] && tmp[0] != ' ') {
			raw = atoi(tmp);
			rat = forced_rat ? forced_rat : fm160_band_rat_of(raw);
			if (raw > 0 && !rat) {
				if (unknown)
					(*unknown)++;
			} else {
				out[n].raw = raw;
				out[n].band = fm160_band_decode(rat, raw);
				out[n].rat = rat;
				n++;
			}
		}
		if (!*e)
			break;
		p = e + 1;
	}
	return n;
}

/*
 * Walk the flat AT+GTACT? band list once, dispatching each token by its own
 * encoding.  Nothing is guessed: a token that matches no documented scheme is
 * counted in unknown_n, and a bare 0 (AT+GTACT's "automatic band selection")
 * sets auto_seen instead of being stored as a band.
 */
static void parse_flat_bands(const char *csv, struct fm160_gtact_state *st)
{
	const char *p = csv;

	if (!csv)
		return;
	while (*p) {
		const char *e = p + strcspn(p, ",");
		char tmp[32];
		size_t len = (size_t)(e - p);
		struct fm160_band *dst = NULL;
		int *n = NULL;
		int raw, rat;

		if (len >= sizeof(tmp))
			len = sizeof(tmp) - 1;
		memcpy(tmp, p, len);
		tmp[len] = '\0';

		if (tmp[0] && tmp[0] != ' ') {
			raw = atoi(tmp);
			rat = fm160_band_rat_of(raw);

			switch (rat) {
			case 2:  dst = &st->umts[st->umts_n]; n = &st->umts_n; break;
			case 4:  dst = &st->lte[st->lte_n];   n = &st->lte_n;  break;
			case 9:  dst = &st->nr[st->nr_n];     n = &st->nr_n;   break;
			default:
				if (raw > 0)
					st->unknown_n++;
				else if (raw == 0)
					st->auto_seen = true;
				break;
			}
			if (dst && n && *n < FM160_BAND_MAX) {
				dst->raw = raw;
				dst->rat = rat;
				dst->band = fm160_band_decode(rat, raw);
				(*n)++;
			}
		}
		if (!*e)
			break;
		p = e + 1;
	}
}

/* AT+GTACT? -> +GTACT: <rat>,<pref1>,<pref2>,[<band_1>,...<band_n>] */
void fm160_parse_gtact(const char *resp)
{
	char line[FM160_RESP_LINE_MAX];
	struct fm160_gtact_state st;
	const char *bands;

	if (!fm160_resp_find(resp, "+GTACT", line, sizeof(line)))
		return;
	/* "+GTACT: (…)" is the =? form; it must not be mistaken for state. */
	if (line[0] == '(')
		return;

	memset(&st, 0, sizeof(st));
	st.rat   = csv_int(line, 0, -1);
	st.pref1 = csv_int(line, 1, -1);
	st.pref2 = csv_int(line, 2, -1);

	/* The bands start at field 3: one flat list mixing all RATs. */
	bands = csv_at(line, 3);
	parse_flat_bands(bands, &st);

	st.valid = true;
	st.last_ok_ms = fm160_now_ms();
	g_state.gtact = st;

	fm160_log(LOG_INFO,
		  "GTACT: rat=%d pref=%d,%d bands umts=%d lte=%d nr=%d"
		  "%s%s",
		  st.rat, st.pref1, st.pref2, st.umts_n, st.lte_n, st.nr_n,
		  st.unknown_n ? " (undecodable tokens!)" : "",
		  st.auto_seen ? " (automatic band selection)" : "");
	fm160_state_mark_dirty();
}

/*
 * AT+GTACT=? -> nine groups:
 *   rat, pref1, pref2, gsm, umts, lte, cdma, evdo, nr
 * On FM160-CN the gsm/cdma/evdo groups are literally "( )".  Recording which
 * groups were empty is what lets the UI show the truth instead of the manual.
 */
void fm160_parse_gtact_caps(const char *resp)
{
	char line[FM160_RESP_LINE_MAX];
	char groups[FM160_GTACT_GROUPS][FM160_RESP_LINE_MAX];
	struct fm160_gtact_caps cp;
	int n, unknown = 0, i;

	if (!fm160_resp_find(resp, "+GTACT", line, sizeof(line)))
		return;
	if (line[0] != '(') {
		fm160_log(LOG_WARNING,
			  "GTACT=? did not answer with a capability list (%s)", line);
		return;
	}

	n = split_groups(line, groups, FM160_GTACT_GROUPS);
	memset(&cp, 0, sizeof(cp));
	for (i = 0; i < n && i < FM160_GTACT_GROUPS; i++)
		cp.group_nonempty[i] = groups[i][0] != '\0';

	if (n > 0) cp.rat_n   = parse_int_list(groups[0], cp.rat,   FM160_CAP_INT_MAX);
	if (n > 1) cp.pref1_n = parse_int_list(groups[1], cp.pref1, FM160_CAP_INT_MAX);
	if (n > 2) cp.pref2_n = parse_int_list(groups[2], cp.pref2, FM160_CAP_INT_MAX);
	if (n > 4) cp.umts_n  = parse_band_list(groups[4], cp.umts, FM160_BAND_MAX, 2, &unknown);
	if (n > 5) cp.lte_n   = parse_band_list(groups[5], cp.lte,  FM160_BAND_MAX, 4, &unknown);
	if (n > 8) cp.nr_n    = parse_band_list(groups[8], cp.nr,   FM160_BAND_MAX, 9, &unknown);

	/* A capability list with no usable RAT is worse than none: the write path
	 * treats "valid" as permission to send. */
	if (!cp.rat_n) {
		fm160_log(LOG_WARNING, "GTACT=? gave %d group(s) but no RAT list", n);
		return;
	}

	cp.valid = true;
	cp.last_ok_ms = fm160_now_ms();
	g_state.gtact_caps = cp;

	fm160_log(LOG_INFO,
		  "GTACT caps: %d rat, %d pref1, %d pref2, umts=%d lte=%d nr=%d "
		  "(groups gsm=%d cdma=%d evdo=%d)",
		  cp.rat_n, cp.pref1_n, cp.pref2_n, cp.umts_n, cp.lte_n, cp.nr_n,
		  cp.group_nonempty[3], cp.group_nonempty[6], cp.group_nonempty[7]);
}

/* AT+GTCELLLOCK? -> +GTCELLLOCK: <mode>[,<rat>,<type>,<earfcn>[,...]] */
void fm160_parse_celllock(const char *resp)
{
	char line[FM160_RESP_LINE_MAX];
	struct fm160_celllock_state cl;

	if (!fm160_resp_find(resp, "+GTCELLLOCK", line, sizeof(line)))
		return;
	if (line[0] == '(')
		return;                      /* the =? form */

	memset(&cl, 0, sizeof(cl));
	cl.enabled = csv_int(line, 0, 0) == 1;

	/* Everything after <mode> is present only while the function is on.
	 * earfcn can reach 4294967295, so it is read as unsigned long long
	 * straight from its field rather than through csv_int(). */
	cl.rat = csv_int(line, 1, -1);
	cl.type = csv_int(line, 2, -1);
	cl.earfcn = strtoull(csv_at(line, 3), NULL, 0);

	cl.pci = csv_at(line, 4)[0] ? csv_int(line, 4, 0) : -1;
	cl.scs = csv_at(line, 5)[0] ? csv_int(line, 5, 0) : -1;
	cl.nrband = csv_at(line, 6)[0] ? csv_int(line, 6, 0) : -1;

	/* The trailing fields are optional; -1 means "the modem did not say". */
	cl.has_pci    = cl.pci != -1;
	cl.has_scs    = cl.scs != -1;
	cl.has_nrband = cl.nrband != -1;
	if (!cl.has_pci)
		cl.pci = 0;
	if (!cl.has_scs)
		cl.scs = 0;
	if (!cl.has_nrband)
		cl.nrband = 0;

	cl.valid = true;
	cl.last_ok_ms = fm160_now_ms();
	g_state.celllock = cl;

	fm160_log(LOG_INFO, "celllock: %s rat=%d type=%d earfcn=%llu pci=%d",
		  cl.enabled ? "enabled" : "disabled", cl.rat, cl.type,
		  cl.earfcn, cl.pci);
	fm160_state_mark_dirty();
}

/*
 * AT+GTCELLLOCK=? -> ranges instead of lists:
 *   (0,1,2),(0-2),(0,1),(0-4294967295),(0-1007),(0-1),(501-50261)
 *
 * ⚠️ The live modem advertises mode "(0,1,2)".  The manual only defines 0
 * (disable) and 1 (enable).  mode 2 is therefore recorded but never written:
 * this is exactly the case where "the device supports it" and "we may use it"
 * are different questions.
 */
void fm160_parse_celllock_caps(const char *resp)
{
	char line[FM160_RESP_LINE_MAX];
	char groups[FM160_GTACT_GROUPS][FM160_RESP_LINE_MAX];
	struct fm160_celllock_caps cp;
	int n;

	if (!fm160_resp_find(resp, "+GTCELLLOCK", line, sizeof(line)))
		return;
	if (line[0] != '(') {
		fm160_log(LOG_WARNING,
			  "GTCELLLOCK=? did not answer with a capability list (%s)",
			  line);
		return;
	}

	n = split_groups(line, groups, FM160_GTACT_GROUPS);
	memset(&cp, 0, sizeof(cp));

	if (n > 0)
		cp.mode_n = parse_int_list(groups[0], cp.mode, (int)ARRAY_SIZE(cp.mode));
	/* Groups 1..6 are "lo-hi" ranges, and <earfcn> reaches 4294967295, so
	 * these are read with sscanf into explicitly typed locals rather than
	 * through the int-returning csv helpers. */
	if (n > 1) {
		int lo = 0, hi = 0;

		if (sscanf(groups[1], "%d-%d", &lo, &hi) == 2) {
			cp.rat_min = lo;
			cp.rat_max = hi;
		}
	}
	if (n > 2)
		cp.type_n = parse_int_list(groups[2], cp.type, (int)ARRAY_SIZE(cp.type));
	if (n > 3) {
		unsigned int lo = 0, hi = 0;

		if (sscanf(groups[3], "%u-%u", &lo, &hi) == 2)
			cp.earfcn_max = hi;
	}
	if (n > 4) {
		int lo = 0, hi = 0;

		if (sscanf(groups[4], "%d-%d", &lo, &hi) == 2)
			cp.pci_max = hi;
	}
	if (n > 5)
		sscanf(groups[5], "%d-%d", &cp.scs_min, &cp.scs_max);
	if (n > 6)
		sscanf(groups[6], "%d-%d", &cp.nrband_min, &cp.nrband_max);

	if (!cp.mode_n) {
		fm160_log(LOG_WARNING, "GTCELLLOCK=? gave no mode list");
		return;
	}

	cp.valid = true;
	cp.last_ok_ms = fm160_now_ms();
	g_state.celllock_caps = cp;

	fm160_log(LOG_INFO,
		  "GTCELLLOCK caps: mode=%d%s rat=%d-%d type=%d earfcn_max=%u "
		  "pci<=%d scs=%d-%d nrband=%d-%d",
		  cp.mode_n, cp.mode_n > 2 ? " (includes an undocumented value!)" : "",
		  cp.rat_min, cp.rat_max, cp.type_n, cp.earfcn_max, cp.pci_max,
		  cp.scs_min, cp.scs_max, cp.nrband_min, cp.nrband_max);
}

/* Bandwidth code -> MHz: the same two tables GTCCINFO uses. */
static int ca_bw_mhz(int rat, int raw)
{
	return decode_bandwidth_mhz(rat, raw);
}

/*
 * One "PCC: ..." / "SCCn: ..." row.  LTE and NR rows have the same column
 * count; what differs is the PCC/SCC offset, because an SCC row leads with
 * <scell_state>,<ul_configured> and the PCC row does not:
 *
 *   PCC: <band>,<pci>,<freq>,<dl_bw>,<dl_mimo>,<ul_mimo>,<dl_mod>,<ul_mod>,<rsrp>
 *   SCC: <state>,<ul_cfg>,<band>,<pci>,<freq>,<dl_bw>,<ul_bw>,<dl_mimo>,
 *        <ul_mimo>,<dl_mod>,<ul_mod>,<rsrp>
 *
 * ⚠️ <freq> is parsed as DECIMAL here, unlike GTCCINFO's <earfcn> which is hex.
 * The manual gives GTCCINFO's ranges as "0-0xFFFFFFF" but GTCAINFO's as
 * "0-65535" (earfcn) and "0-2229167" (narfcn) - no 0x anywhere - so the same
 * looking column has two different radices in two different responses.
 * Nothing in this milestone can verify that (CA needs a registered SIM), so the
 * decode stays as documented and the raw string is not re-derived from it.
 */
static bool ca_parse_row(const char *csv, int rat, bool is_pcc,
			 struct fm160_ca_cell *out)
{
	int off = is_pcc ? 0 : 2;      /* SCC rows lead with state,ul_configured */

	if (!csv || csv[0] < '0' || csv[0] > '9')
		return false;

	memset(out, 0, sizeof(*out));
	out->valid = true;
	out->is_pcc = is_pcc;

	if (!is_pcc) {
		out->state = csv_int(csv, 0, 0);
		out->ul_configured = csv_int(csv, 1, 0);
	}
	out->band = fm160_band_decode(rat, csv_int(csv, off + 0, 0));
	out->pci = csv_int(csv, off + 1, -1);
	out->freq = strtoull(csv_at(csv, off + 2), NULL, 10);
	out->dl_bw_mhz = ca_bw_mhz(rat, csv_int(csv, off + 3, 0));
	out->ul_bw_mhz = is_pcc ? FM160_NONE :
			 ca_bw_mhz(rat, csv_int(csv, off + 4, 0));
	out->dl_mimo = csv_int(csv, off + (is_pcc ? 4 : 5), 0);
	out->ul_mimo = csv_int(csv, off + (is_pcc ? 5 : 6), 0);
	out->dl_mod = csv_int(csv, off + (is_pcc ? 6 : 7), -1);
	out->ul_mod = csv_int(csv, off + (is_pcc ? 7 : 8), -1);
	out->rsrp_dbm = decode_rsrp_dbm(rat, csv_int(csv, off + (is_pcc ? 8 : 9), 255));
	return true;
}

/*
 * AT+GTCAINFO? -> a labelled block:
 *
 *   1. LTE
 *   PCC: 103,484,1650,100,2,1,4,4,47
 *   SCC1:2,0,141,123,3900,75,50,2,2,3,3,40
 *   2. NR
 *   PCC: ...
 *
 * Measured with no SIM it answers a bare "OK" - no data block at all - so
 * "nothing parsed" is a normal outcome and clears the state instead of being
 * logged as a failure.  Without that, the UI would keep showing a stale CA
 * table from before the module deregistered.
 */
void fm160_parse_cainfo(const char *resp)
{
	const char *p;
	struct fm160_ca_state ca;
	int cur_rat = 0;

	if (!resp)
		return;
	p = strstr(resp, "+GTCAINFO");
	if (!p) {
		/* No block: not aggregated (or not registered).  Clear, do not warn. */
		g_state.ca.valid = false;
		g_state.ca.scc_n = 0;
		g_state.ca.last_ok_ms = fm160_now_ms();
		g_state.ca.has_nr = false;
		return;
	}
	p = strchr(p, '\n');
	if (!p)
		return;
	p++;

	memset(&ca, 0, sizeof(ca));

	while (*p) {
		char line[FM160_RESP_LINE_MAX];
		const char *eol = p + strcspn(p, "\r\n");
		size_t len = (size_t)(eol - p);

		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = '\0';
		p = *eol ? eol + 1 : eol;

		if (line[0] < '0' || line[0] > '9') {
			/* A header ("1. LTE" / "2. NR").  cur_rat tracks the block we
			 * are inside; ca.rat keeps the FIRST one, because that is the
			 * PCC the UI reports as the headline. */
			if (strstr(line, "LTE")) {
				cur_rat = 4;
			} else if (strstr(line, "NR")) {
				cur_rat = 9;
				ca.has_nr = true;
			}
			if (!ca.rat && cur_rat)
				ca.rat = cur_rat;
			continue;
		}
		if (!cur_rat || cur_rat != ca.rat)
			continue;      /* a second RAT block: see the note below */

		if (!strncmp(line, "PCC", 3)) {
			const char *colon = strchr(line, ':');

			if (colon)
				ca_parse_row(colon + 1, ca.rat, true, &ca.pcc);
			continue;
		}
		if (!strncmp(line, "SCC", 3)) {
			const char *colon = strchr(line, ':');

			if (colon && ca.scc_n < FM160_CA_MAX &&
			    ca_parse_row(colon + 1, ca.rat, false, &ca.scc[ca.scc_n]))
				ca.scc_n++;
		}
	}

	/* Only the first block (the one whose PCC we keep) is expanded into the
	 * SCC array.  EN-DC reports a second block for the NR leg; ca.has_nr
	 * records that it was there so the UI can say "EN-DC" without pretending
	 * it has two full CA tables. */
	if (!ca.pcc.valid) {
		ca.valid = false;
		ca.scc_n = 0;
	}

	ca.last_ok_ms = fm160_now_ms();
	g_state.ca = ca;

	if (ca.valid)
		fm160_log(LOG_INFO, "CA: %s PCC band %d + %d SCC%s",
			  ca.rat == 9 ? "NR" : "LTE", ca.pcc.band, ca.scc_n,
			  ca.has_nr && ca.rat != 9 ? " (EN-DC: NR leg present)" : "");
	else
		fm160_log(LOG_DEBUG, "CA: not aggregated");
	fm160_state_mark_dirty();
}

/* ------------------------------------------------------------------ */
/* M4 polls                                                             */
/* ------------------------------------------------------------------ */

static void gtact_cb(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_gtact(resp);
}

static void celllock_cb(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_celllock(resp);
}

static void ca_cb(struct at_req *r, enum at_status s, const char *resp, void *a)
{
	if (s == AT_STATUS_OK)
		fm160_parse_cainfo(resp);
}

void fm160_cmd_poll_gtact(void)
{
	/* 26 ms measured.  This is the only cheap way to learn that a persistent
	 * band restriction is in place - the UI must not present "unlocked" just
	 * because nobody asked. */
	atq_submit(AT_PRIO_POLL, "AT+GTACT?", NULL, 3000, gtact_cb, NULL);
}

void fm160_cmd_poll_celllock(void)
{
	atq_submit(AT_PRIO_POLL, "AT+GTCELLLOCK?", NULL, 5000, celllock_cb, NULL);
}

void fm160_cmd_poll_ca(void)
{
	atq_submit(AT_PRIO_POLL, "AT+GTCAINFO?", NULL, 5000, ca_cb, NULL);
}

/* ------------------------------------------------------------------ */
/* M4 write path                                                        */
/* ------------------------------------------------------------------ */

/*
 * Build the AT+GTACT set command.
 *
 * The manual's rule is that leaving <rat> and both <PreferredAct*> blank keeps
 * the current RAT selection and only restricts bands, and that the flat band
 * list is what follows:
 *   AT+GTACT=,,,160,155      -> LTE B60 + B55 only
 *   AT+GTACT=,,,103,5078     -> LTE B3 + NR n78
 *
 * Both fields are omitted rather than defaulted, so this builder only ever
 * emits the "leave the RAT alone" form.  Changing the RAT is AT+GTRAT's job and
 * it is deliberately not implemented in this milestone: the manual forbids
 * combining GTACT/COPS/GTRAT/GTCELLLOCK in the first place.
 *
 * bands_csv is written verbatim after validation, because the RAT-prefixed
 * tokens (101.. / 501..) are the only form the modem accepts.
 */
int fm160_bands_command(char *out, size_t outlen, const char *bands_csv)
{
	int n = 0;
	const char *p;

	if (!bands_csv || !bands_csv[0])
		return -1;

	/* Validate before building: only digits and commas get into a command
	 * line.  A stray space or letter would change the meaning of the band
	 * list, so anything else is rejected rather than trimmed. */
	for (p = bands_csv; *p; p++) {
		if (*p >= '0' && *p <= '9') {
			n++;
			continue;
		}
		if (*p != ',')
			return -1;
	}
	/* One band per slot and no empty slot in the middle: "101,,103" would
	 * shift every following parameter, and a trailing comma would add one. */
	if (!n || strstr(bands_csv, ",,") ||
	    bands_csv[0] == ',' || bands_csv[strlen(bands_csv) - 1] == ',')
		return -1;

	n = snprintf(out, outlen, "AT+GTACT=,,,%s", bands_csv);
	if (n < 0 || (size_t)n >= outlen)
		return -1;
	return 0;
}

/*
 * Build the AT+GTCELLLOCK set command.
 *
 *   AT+GTCELLLOCK=<mode>[,<rat>,<type>,<earfcn>[,<PCI>][,<scs>[,<nrband>]]]
 *
 * The trailing fields nest, so they are appended strictly in order and each one
 * has an explicit "present" marker rather than a zero default:
 *   pci < 0          -> omit <PCI> (and therefore <scs>/<nrband> as well)
 *   rat != 1 (NR)    -> omit <scs>/<nrband>, which exist only for NR
 *   scs < 0          -> omit <scs>, even for NR
 *   nrband < 0       -> omit <nrband>
 * Writing a filler 0 for an absent <PCI> would silently claim "lock PCI 0", and
 * a filler 0 for <nrband> claims NR band 0, which does not exist.  -1 is the
 * "absent" marker for all three; 0 is a legal value for <scs>.
 *
 * mode is restricted to the two values the manual defines.  The live modem
 * additionally advertises mode 2 in its capability list; writing an
 * undocumented value into a persistent EFS setting is not something a
 * "make band locking stable" milestone should do.
 */
int fm160_celllock_command(char *out, size_t outlen, int mode, int rat, int type,
			   unsigned long long earfcn, int pci, int scs, int nrband)
{
	size_t used;
	int n;

	if (mode != 0 && mode != 1)
		return -1;

	if (mode == 0) {
		/* Disabling takes no other argument: AT+GTCELLLOCK=0 */
		n = snprintf(out, outlen, "AT+GTCELLLOCK=0");
		return (n < 0 || (size_t)n >= outlen) ? -1 : 0;
	}

	if (rat < 0 || rat > 2 || type < 0 || type > 1)
		return -1;

	n = snprintf(out, outlen, "AT+GTCELLLOCK=1,%d,%d,%llu", rat, type, earfcn);
	if (n < 0 || (size_t)n >= outlen)
		return -1;
	used = (size_t)n;

	if (pci >= 0) {
		n = snprintf(out + used, outlen - used, ",%d", pci);
		if (n < 0 || (size_t)n >= outlen - used)
			return -1;
		used += (size_t)n;

		if (rat == 1) {
			/* <scs> and <nrband> are both individually optional, so each
			 * one is only written when the caller actually supplied it. */
			if (scs >= 0) {
				n = snprintf(out + used, outlen - used, ",%d", scs);
				if (n < 0 || (size_t)n >= outlen - used)
					return -1;
				used += (size_t)n;

				if (nrband >= 0) {
					n = snprintf(out + used, outlen - used, ",%d",
						     nrband);
					if (n < 0 || (size_t)n >= outlen - used)
						return -1;
				}
			}
		}
	}
	return 0;
}

struct m4_write_ctx {
	at_done_cb user_cb;
	void *user_arg;
	int quiet_kind;
	char readback[FM160_AT_CMD_MAX];
};

/*
 * Serialisation + debounce for the two persistent writers.
 *
 * AT+GTACT and AT+GTCELLLOCK both touch an EFS file, and the manual forbids
 * combining them with each other and with GTRAT/COPS.  atq_submit() already
 * serialises individual commands, but it would happily queue a GTACT= right
 * behind a GTCELLLOCK= - which is exactly the combination the manual rules
 * out.  A single in-flight flag, cleared only when the last read-back of the
 * sequence has finished, is enough to keep that from happening.  The quiet
 * window afterwards keeps the poll tiers off the module while it re-registers
 * (a band change drops the current cell).
 */
static bool m4_writing;

static void m4_readback_cb(struct at_req *req, enum at_status status,
			   const char *response, void *arg)
{
	struct m4_write_ctx *c = arg;

	/* The read-back, not the write, is what gets reported as the new state:
	 * the module may clamp, reject or reinterpret a value, and only the
	 * read-back shows which of those happened. */
	if (status == AT_STATUS_OK) {
		if (strstr(c->readback, "GTCELLLOCK"))
			fm160_parse_celllock(response);
		else
			fm160_parse_gtact(response);
	}
	if (c->user_cb) {
		struct at_req fake = { 0 };

		snprintf(fake.cmd, sizeof(fake.cmd), "%s", c->readback);
		c->user_cb(&fake, status, response, c->user_arg);
	}

	/* The band list may have changed, so the derived CA view is stale. */
	fm160_cmd_poll_ca();
	fm160_state_mark_dirty();
	m4_writing = false;
	free(c);
}

static void m4_write_cb(struct at_req *req, enum at_status status,
			const char *response, void *arg)
{
	struct m4_write_ctx *c = arg;
	int ret;

	if (status != AT_STATUS_OK) {
		if (c->user_cb)
			c->user_cb(req, status, response, c->user_arg);
		/* The write never landed, so nothing is re-registering: drop the
		 * window we opened instead of keeping the polls away for 8 s. */
		if (g_state.quiet_kind == c->quiet_kind)
			atq_clear_quiet();
		m4_writing = false;
		free(c);
		return;
	}

	/* The read-back is a fresh request; if the queue will not take it (no
	 * port, quiet window, full) the caller still has to be answered, and the
	 * context still has to be freed - otherwise the ubus request stays
	 * deferred forever and the caller hangs. */
	ret = atq_submit(AT_PRIO_STATE, c->readback, NULL, 5000, m4_readback_cb, c);
	if (ret) {
		fm160_log(LOG_WARNING, "read-back %s rejected (rc=%d)",
			  c->readback, ret);
		if (c->user_cb)
			c->user_cb(req, AT_STATUS_BUSY, response, c->user_arg);
		m4_writing = false;
		free(c);
	}
}

int fm160_cmd_set_bands(const char *bands_csv, at_done_cb cb, void *arg)
{
	struct m4_write_ctx *c;
	char cmd[FM160_AT_CMD_MAX];
	int ret;

	if (!g_state.gtact_caps.valid) {
		fm160_log(LOG_WARNING,
			  "refusing to write bands: AT+GTACT=? never enumerated");
		return -1;
	}
	if (m4_writing)
		return -1;
	if (fm160_bands_command(cmd, sizeof(cmd), bands_csv))
		return -1;

	c = calloc(1, sizeof(*c));
	if (!c)
		return -1;
	c->user_cb = cb;
	c->user_arg = arg;
	c->quiet_kind = QUIET_BANDS;
	snprintf(c->readback, sizeof(c->readback), "AT+GTACT?");

	fm160_log(LOG_WARNING, "band lock write: %s", cmd);
	/* The flag goes up BEFORE the submit: atq_dispatch() answers NOPORT and
	 * "could not send" synchronously, so the callback may already have run
	 * (and cleared the flag) by the time atq_submit() returns.  Setting it
	 * afterwards would leave the flag stuck true and lock the write path
	 * out for the rest of the process's life. */
	m4_writing = true;
	ret = atq_submit(AT_PRIO_INTERACTIVE, cmd, NULL, 8000, m4_write_cb, c);
	if (ret) {
		m4_writing = false;
		free(c);
		return -1;
	}
	/* Only when the sequence is really in flight: a rejected write must not
	 * leave a quiet window behind with nothing to justify it. */
	if (m4_writing)
		atq_set_quiet(QUIET_BANDS, "band lock write, re-registering", 8);
	return 0;
}

int fm160_cmd_set_celllock(int mode, int rat, int type, unsigned long long earfcn,
			   int pci, int scs, int nrband, at_done_cb cb, void *arg)
{
	struct m4_write_ctx *c;
	char cmd[FM160_AT_CMD_MAX];
	int ret;

	if (!g_state.celllock_caps.valid) {
		fm160_log(LOG_WARNING,
			  "refusing to write cell lock: AT+GTCELLLOCK=? never enumerated");
		return -1;
	}
	if (m4_writing)
		return -1;
	if (fm160_celllock_command(cmd, sizeof(cmd), mode, rat, type, earfcn,
				   pci, scs, nrband))
		return -1;

	c = calloc(1, sizeof(*c));
	if (!c)
		return -1;
	c->user_cb = cb;
	c->user_arg = arg;
	c->quiet_kind = QUIET_CELLLOCK;
	snprintf(c->readback, sizeof(c->readback), "AT+GTCELLLOCK?");

	fm160_log(LOG_WARNING, "cell lock write: %s", cmd);
	m4_writing = true;
	ret = atq_submit(AT_PRIO_INTERACTIVE, cmd, NULL, 10000, m4_write_cb, c);
	if (ret) {
		m4_writing = false;
		free(c);
		return -1;
	}
	/* The write only takes effect after a UE reset, which fm160d never
	 * performs; the quiet window only covers the EFS commit. */
	if (m4_writing)
		atq_set_quiet(QUIET_CELLLOCK, "cell lock write to EFS", 8);
	return 0;
}

