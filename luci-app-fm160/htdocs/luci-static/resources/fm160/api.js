'use strict';
'require baseclass';
'require rpc';

/*
 * Thin wrapper around the fm160 ubus object.
 *
 * The front end never talks to the AT transport: every read is served from
 * fm160d's cached snapshot, and the only calls that reach the modem are the
 * explicitly user-triggered ones below.
 */

var callStatus   = rpc.declare({ object: 'fm160', method: 'status',   expect: {} });
var callIdentity = rpc.declare({ object: 'fm160', method: 'identity', expect: {} });
var callProfile  = rpc.declare({ object: 'fm160', method: 'profile',  params: [ 'active' ], expect: {} });
var callAt       = rpc.declare({ object: 'fm160', method: 'at',       params: [ 'cmd', 'timeout', 'end_flag' ], expect: {} });
var callRescan   = rpc.declare({ object: 'fm160', method: 'rescan',   expect: {} });
var callIdent    = rpc.declare({ object: 'fm160', method: 'ident',    expect: {} });
var callEnabled  = rpc.declare({ object: 'fm160', method: 'enabled',  params: [ 'enabled' ], expect: {} });

/*
 * USB profiles.
 *
 * The dial-up document carries FOUR port tables, one per platform, and the same
 * mode NUMBER means different things in each of them - mode 19 has an AT port on
 * Qualcomm (2CB7:0x0106) but none at all on 0x05C6:0x9025.  Only 表 1 (Qualcomm,
 * VID 2CB7, PID 010x) describes an FM160, so a switch is only ever offered for a
 * unit that reports exactly that identity.
 *
 * Two independent facts then decide whether a profile may be offered:
 *
 *   has_at   the composite descriptor contains an "AT Device Application
 *            Interface", so an AT port exists after re-enumeration.  A mode
 *            WITHOUT it is a one-way trip: AT+GTUSBMODE can never be sent again.
 *
 *   fm160    the mode is listed in the FM160 AT manual 11.1.2.4.  Modes known
 *            only from the port table are "device dependent" and stay behind an
 *            explicit opt-in.
 *
 * PID is informational: 17 and 32 share 0x0104, and 18 and 33 share 0x0105, so
 * the active profile can NEVER be inferred from VID:PID.  Read it back with
 * AT+GTUSBMODE?.
 */
var USB_PLATFORM = { vid: '2cb7', pidPrefix: '010' };

var USB_MODES = {
	17: { pid: '0x0104', kind: 'qmi',  has_at: true,  fm160: true,
	      layout: 'DIAG+MODEM+AT+PIPE+RMNET+ADB' },
	18: { pid: '0x0105', kind: 'ecm',  has_at: true,  fm160: true,
	      layout: 'DIAG+MODEM+AT+PIPE+ECM+ECM+ADB' },
	19: { pid: '0x0106', kind: 'ecm',  has_at: true,  fm160: false,
	      layout: 'DIAG+MODEM+AT+ECM+ECM' },
	20: { pid: '0x0107', kind: 'none', has_at: false, fm160: true,
	      layout: 'MODEM (no AT, no data)' },
	21: { pid: '0x0108', kind: 'none', has_at: true,  fm160: true,
	      layout: 'MODEM+AT (no data)' },
	22: { pid: '0x0109', kind: 'qmi',  has_at: true,  fm160: false,
	      layout: 'MODEM+AT+RMNET' },
	23: { pid: '0x010A', kind: 'ecm',  has_at: true,  fm160: false,
	      layout: 'MODEM+AT+ECM+ECM' },
	24: { pid: '0x010B', kind: 'none', has_at: false, fm160: true,
	      layout: 'RNDIS+RNDIS+MODEM+DIAG+ADB (no AT)' },
	28: { pid: '0x010F', kind: 'none', has_at: false, fm160: false,
	      layout: 'MBIM only (no AT)' },
	29: { pid: '0x0110', kind: 'mbim', has_at: true,  fm160: true,
	      layout: 'MBIM+MBIM+AT+DIAG' },
	30: { pid: '0x0111', kind: 'mbim', has_at: true,  fm160: true,
	      layout: 'MBIM+MBIM+MODEM+DIAG+AT' },
	31: { pid: null,     kind: 'none', has_at: false, fm160: true,
	      layout: 'DIAG+MODEM+RMNET+DPL+QDSS+ADB (no AT)' },
	32: { pid: '0x0104', kind: 'qmi',  has_at: true,  fm160: true,
	      layout: 'DIAG+MODEM+AT+PIPE+RMNET' },
	33: { pid: '0x0105', kind: 'ecm',  has_at: true,  fm160: true,
	      layout: 'DIAG+MODEM+AT+PIPE+ECM+ECM' }
};

/* Modes that expose no AT interface.  Switching to one of these removes the
 * only management channel this whole application depends on, and there is no
 * way back from the OS side: the FM160 has no reset-to-default that we can
 * trigger, so recovery is a manual power cycle.
 *
 * 20, 24 and 31 are all present in this unit's AT+GTUSBMODE=? answer.  28 is
 * not reported here but does appear in the vendor port tables, so it stays as
 * a guard for other firmware builds. */
var USB_MODE_BLACKLIST = [ 20, 24, 28, 31 ];

/*
 * Hardware-verified mode set.
 *
 * FM160-CN 89614.1000.00.04.01.02 answered AT+GTUSBMODE=? with
 *     +GTUSBMODE: (17-18,20-21,24,29-33)
 * i.e. exactly { 17, 18, 20, 21, 24, 29, 30, 31, 32, 33 }, and
 * AT+GTUSBMODE? currently reads 32.
 *
 * 19, 22, 23 and 28 are described in the vendor's per-platform port tables but
 * are NOT offered by this firmware.  They stay in USB_MODES only so a modem
 * that does report them gets a label instead of a bare number; they must never
 * be proposed as a switch target.
 */
var USB_MODE_HW = [ 17, 18, 20, 21, 24, 29, 30, 31, 32, 33 ];

/* Preference order per dial kind, restricted to USB_MODE_HW.
 *
 * This firmware offers no NCM, RNDIS or GobiNet target, so only the QMI, ECM
 * and MBIM families are reachable -- which matches the project's scope of
 * "QMI / MBIM / ECM only". */
var USB_MODE_PREFERRED = {
	qmi:  [ 32, 17 ],
	mbim: [ 30, 29 ],
	ecm:  [ 33, 18 ]
};

function normHex(v, width) {
	if (v === undefined || v === null)
		return '';
	var s = String(v).toLowerCase().replace(/^0x/, '').replace(/^0+/, '');

	if (!s)
		s = '0';
	return s.padStart(width, '0');
}

/* true only for the platform whose mode table we actually know. */
function usbPlatformKnown(idVendor, idProduct) {
	if (!idVendor || !idProduct)
		return false;
	return normHex(idVendor, 4) === USB_PLATFORM.vid &&
	       normHex(idProduct, 4).indexOf(USB_PLATFORM.pidPrefix) === 0;
}

/* Any mode number the modem reports that this table does not describe. */
function usbModeKnown(mode) {
	return Object.prototype.hasOwnProperty.call(USB_MODES, mode);
}

function usbModeHasAt(mode) {
	var e = USB_MODES[mode];
	return e ? e.has_at : false;
}

function usbModeDocumented(mode) {
	var e = USB_MODES[mode];
	return e ? e.fm160 : false;
}

/* Present in this firmware's AT+GTUSBMODE=? answer.  Used as a fallback when
 * the caller has not read the live list yet. */
function usbModeHwSupported(mode) {
	return USB_MODE_HW.indexOf(Number(mode)) >= 0;
}

/*
 * usbModeRisk() -> 'safe' | 'advanced' | 'unknown' | 'forbidden'
 * 'advanced'/'unknown' mean: we cannot prove the AT port survives the switch,
 * so the UI must ask for an explicit confirmation first.
 */
function usbModeRisk(mode) {
	if (!usbModeKnown(mode))
		return 'unknown';
	if (!usbModeHasAt(mode))
		return 'forbidden';
	return USB_MODES[mode].fm160 ? 'safe' : 'advanced';
}

/* 17/32 and 18/33 are indistinguishable over USB, so the numeric mode can only
 * be confirmed with AT+GTUSBMODE?. */
function usbModePidClash(mode) {
	if (mode === 17 || mode === 32)
		return [ 17, 32 ];
	if (mode === 18 || mode === 33)
		return [ 18, 33 ];
	return null;
}

var REG_TEXT = {
	0:  _('not registered'),
	1:  _('registered (home)'),
	2:  _('searching'),
	3:  _('registration denied'),
	4:  _('unknown'),
	5:  _('registered (roaming)'),
	99: _('unknown')
};

var RAT_TEXT = {
	0:  _('no service'),
	2:  'WCDMA',
	4:  'LTE',
	9:  'NR / 5G'
};

function ratName(rat) {
	return RAT_TEXT[rat] || '?';
}

function regName(stat) {
	return REG_TEXT[stat] || _('unknown');
}

function usbModeLabel(mode) {
	var e = USB_MODES[mode];
	if (!e)
		return 'mode ' + mode + ' ' + _('(not in the known table)');
	return mode + ' - ' + e.layout;
}

function isServiceable(stat) {
	return stat === 1 || stat === 5;
}

/* Sentinel used by fm160d for "the modem did not report this". */
var NONE = -1000000;

/*
 * Every numeric field in the status blob is written with blobmsg_add_u32/u64,
 * so the -1000000 sentinel reaches JavaScript as 4293967296, not as -1000000.
 * A plain `value >= 0` test is therefore true for exactly the values it is
 * meant to reject, and a bare `value` test prints seven-digit rubbish.
 *
 * Both encodings are rejected here, along with a missing key -- the whole point
 * is that no view has to know which field happens to use which encoding.  Real
 * values (dBm, tenths of a dB, PCI, MHz, band numbers) are all far inside the
 * magnitude bound.
 */
var NONE_MAGNITUDE = 500000;

function reported(v) {
	return typeof v === 'number' && v > -NONE_MAGNITUDE && v < NONE_MAGNITUDE;
}

/* A power in dBm, or null.  Legitimate values are always negative. */
function dbm(v) {
	return (reported(v) && v < 0) ? v : null;
}

/* A value carried in tenths of a dB, converted to dB, or null. */
function db10(v) {
	return reported(v) ? v / 10 : null;
}

function csqOf(st)     { return (st && st.csq)     || {}; }
function cesqOf(st)    { return (st && st.cesq)    || {}; }
function trafficOf(st) { return (st && st.traffic) || {}; }

/*
 * AT+CSQ's first field carries RSSI on 2G/3G, but on the 5G path the modem puts
 * SS-RSRP in it instead; is_ss_rsrp says which.  When it is really an RSRP,
 * reporting it as an RSSI would be a wrong number under a right label, so the
 * RSSI accessor refuses and the RSRP accessor takes over.
 */
function isSsRsrp(st) { return !!csqOf(st).is_ss_rsrp; }

function rssiDbm(st) {
	return isSsRsrp(st) ? null : dbm(csqOf(st).rssi_dbm);
}

/* Raw AT+CSQ fields, for the views that show what the modem literally said. */
function csqRawRssi(st) { return csqOf(st).raw_rssi; }
function csqRawBer(st)  { return csqOf(st).raw_ber; }

/*
 * RSRP/RSRQ for the serving RAT: which of the two CESQ columns is the live one
 * follows is_ss_rsrp, and if the preferred column is empty the other is used
 * rather than showing nothing.
 */
function rsrpDbm(st) {
	var c = cesqOf(st), lte = dbm(c.lte_rsrp_dbm), nr = dbm(c.nr_ss_rsrp_dbm);

	return isSsRsrp(st) ? (nr !== null ? nr : lte) : (lte !== null ? lte : nr);
}

function rsrqDb(st) {
	var c = cesqOf(st), lte = db10(c.lte_rsrq_db10), nr = db10(c.nr_ss_rsrq_db10);

	return isSsRsrp(st) ? (nr !== null ? nr : lte) : (lte !== null ? lte : nr);
}

function cesqSinrDb(st) { return db10(cesqOf(st).nr_ss_sinr_db10); }

/* --- one cell row, as published by cell_to_blob() ----------------- */

function cellRsrp(cell)      { return dbm(cell && cell.rsrp_dbm); }
function cellRsrqDb(cell)    { return db10(cell && cell.rsrq_db10); }
function cellSinrDb(cell)    { return db10(cell && cell.sinr_db10); }
function cellBand(cell)      { return (cell && reported(cell.band) && cell.band > 0) ? cell.band : null; }
function cellPci(cell)       { return (cell && reported(cell.pci)) ? cell.pci : null; }
function cellBandwidthMhz(cell) {
	return (cell && reported(cell.bandwidth) && cell.bandwidth > 0) ? cell.bandwidth : null;
}
/* 0 and '' both mean "not reported" for these, and neither is worth printing. */
function cellPlmn(cell)      { return (cell && cell.mcc) ? (cell.mcc + '-' + cell.mnc) : null; }
function cellTac(cell)       { return (cell && cell.tac) ? cell.tac : null; }
function cellCellId(cell)    { return (cell && cell.cellid) ? cell.cellid : null; }
function cellEarfcn(cell)    { return (cell && cell.earfcn) ? cell.earfcn : null; }

function fmtDbm(v) {
	return (dbm(v) === null) ? '-' : (v + ' dBm');
}

/* Values that are transmitted in tenths of a dB. */
function fmtDb10(v) {
	var d = db10(v);

	return (d === null) ? '-' : (d.toFixed(1) + ' dB');
}

function fmtNum(v, unit) {
	return reported(v) ? (v + (unit ? ' ' + unit : '')) : '-';
}

/* NR/LTE bandwidth arrives decoded into MHz by fm160d. */
function fmtBandwidth(cell) {
	var mhz = cellBandwidthMhz(cell);

	return (mhz === null) ? '-' : (mhz + ' MHz');
}

function fmtBand(band) {
	return (band === undefined || band === null || band <= 0) ? '-' : ('B' + band);
}

function fmtBytes(v) {
	var units = [ 'B', 'KiB', 'MiB', 'GiB', 'TiB' ], i = 0, n = Number(v) || 0;

	while (n >= 1024 && i < units.length - 1) {
		n /= 1024;
		i++;
	}
	return (i === 0 ? n : n.toFixed(n >= 100 ? 0 : n >= 10 ? 1 : 2)) + ' ' + units[i];
}

function fmtRate(v) {
	var n = Number(v) || 0;

	if (n < 1000)
		return n + ' B/s';
	if (n < 1000 * 1000)
		return (n / 1024).toFixed(1) + ' KiB/s';
	return (n / 1024 / 1024).toFixed(2) + ' MiB/s';
}

function fmtAge(ms) {
	if (ms === undefined || ms === null)
		return '-';
	if (ms < 1000)
		return ms + ' ms';
	if (ms < 60000)
		return (ms / 1000).toFixed(1) + ' s';
	return Math.floor(ms / 60000) + ' min';
}

/* Health of the AT link, as a level used by the views for colouring. */
function atHealth(state) {
	if (!state || !state.port_found)
		return { level: 'down', text: _('AT port not found') };
	if (state.at_state === 2)
		return { level: 'down', text: _('AT unresponsive - automatic polling stopped') };
	if (state.at_state === 1)
		return { level: 'warn', text: _('AT degraded - polling slowed down') };
	return { level: 'ok', text: _('AT healthy') };
}

return baseclass.extend({
	USB_MODES: USB_MODES,
	USB_MODE_BLACKLIST: USB_MODE_BLACKLIST,
	USB_MODE_PREFERRED: USB_MODE_PREFERRED,
	USB_MODE_HW: USB_MODE_HW,
	USB_MODE_NONE: NONE,

	status: callStatus,
	identity: callIdentity,
	profile: callProfile,
	at: callAt,
	rescan: callRescan,
	ident: callIdent,
	setEnabled: callEnabled,

	ratName: ratName,
	regName: regName,
	usbModeLabel: usbModeLabel,
	usbPlatformKnown: usbPlatformKnown,
	usbModeKnown: usbModeKnown,
	usbModeHasAt: usbModeHasAt,
	usbModeDocumented: usbModeDocumented,
	usbModeHwSupported: usbModeHwSupported,
	usbModeRisk: usbModeRisk,
	usbModePidClash: usbModePidClash,
	isServiceable: isServiceable,
	reported: reported,
	dbm: dbm,
	db10: db10,
	csqOf: csqOf,
	cesqOf: cesqOf,
	trafficOf: trafficOf,
	isSsRsrp: isSsRsrp,
	rssiDbm: rssiDbm,
	csqRawRssi: csqRawRssi,
	csqRawBer: csqRawBer,
	rsrpDbm: rsrpDbm,
	rsrqDb: rsrqDb,
	cesqSinrDb: cesqSinrDb,
	cellRsrp: cellRsrp,
	cellRsrqDb: cellRsrqDb,
	cellSinrDb: cellSinrDb,
	cellBand: cellBand,
	cellPci: cellPci,
	cellBandwidthMhz: cellBandwidthMhz,
	cellPlmn: cellPlmn,
	cellTac: cellTac,
	cellCellId: cellCellId,
	cellEarfcn: cellEarfcn,
	fmtDbm: fmtDbm,
	fmtDb10: fmtDb10,
	fmtNum: fmtNum,
	fmtBandwidth: fmtBandwidth,
	fmtBand: fmtBand,
	fmtBytes: fmtBytes,
	fmtRate: fmtRate,
	fmtAge: fmtAge,
	atHealth: atHealth,

	/*
	 * Modes that may be offered for a given dial kind.
	 *   supported      the modem's own AT+GTUSBMODE=? answer (numbers)
	 *   allowAdvanced  include modes present in the port table but not in the
	 *                  FM160 manual's value list
	 *   platformOk     result of usbPlatformKnown(); when false nothing is
	 *                  offered at all, because on any other platform the mode
	 *                  numbers mean something different
	 * Never returns a blacklisted mode or one known to lack an AT port.
	 */
	candidates: function(kind, supported, allowAdvanced, platformOk) {
		if (platformOk === false)
			return [];

		var prefs = USB_MODE_PREFERRED[kind] || [];
		/* Prefer the live list; fall back to the hardware-verified set rather
		 * than to "no filter", so a caller that never read AT+GTUSBMODE=?
		 * still cannot be offered a mode this firmware does not have. */
		var ok = (supported && supported.length) ? supported : USB_MODE_HW;

		return prefs.filter(function(m) {
			var risk = usbModeRisk(m);

			if (ok.indexOf(m) < 0)
				return false;
			if (risk === 'forbidden')
				return false;
			if (risk !== 'safe' && !allowAdvanced)
				return false;
			return true;
		});
	},

	/* Modes the modem reports that we cannot classify - callers must confirm. */
	unknownSupported: function(supported) {
		return (supported || []).filter(function(m) { return !usbModeKnown(m); });
	}
});
