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

static long long csv_ll(const char *s, int idx, long long fallback)
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
	return strtoll(p, NULL, 10);
}

/* GTCCINFO prints <tac>, <cell_id>, <earfcn> and <physicalcellId> as hex with
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
 * cell it describes as B3.  decode_band() still also accepts a bare band
 * number, because <band> is documented as BAND_INVALID (0) whenever the modem
 * is not registered and the firmware could report either form.
 */
static int decode_band(int rat, int raw)
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
			decode_band(rat, csv_int(line, c.band, 0)) : 0;

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

struct ident_item {
	const char *cmd;
	char *dst;
	size_t dstlen;
	bool usb_mode;
};

static struct ident_item ident_plan[] = {
	{ "AT+CGMI",      g_state.manufacturer, sizeof(g_state.manufacturer), false },
	{ "AT+CGMM",      g_state.model,        sizeof(g_state.model),        false },
	{ "AT+CGMR",      g_state.revision,     sizeof(g_state.revision),     false },
	{ "AT+CGSN",      g_state.imei,         sizeof(g_state.imei),         false },
	{ "AT+CFSN",      g_state.sn,           sizeof(g_state.sn),           false },
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
	{ "AT+ICCID",     g_state.iccid,        sizeof(g_state.iccid),        false },
	{ "AT+GTUSBMODE?", NULL,                0,                            true  },
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
		if (it->usb_mode)
			fm160_parse_usbmode(response);
		else if (it->dst)
			snprintf(it->dst, it->dstlen, "%s",
				 first_value_line(response));
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
