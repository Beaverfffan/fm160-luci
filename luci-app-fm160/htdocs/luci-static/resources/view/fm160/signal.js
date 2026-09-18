'use strict';
'require view';
'require poll';
'require ui';
'require fm160.api as api';

/*
 * Signal page: quality bars, the serving cell and the neighbour list.
 *
 * The cell data comes from a single AT+GTCCINFO? per poll, which returns the
 * serving cell plus up to ten neighbours with RSRP/RSRQ/SINR.  The page only
 * pulls it while it is open - the daemon drops the cell tier back to disabled as
 * soon as the foreground hold expires.
 */

function scale(value, min, max) {
	if (value === undefined || value === null)
		return null;
	var pct = (value - min) / (max - min) * 100;
	return Math.max(0, Math.min(100, pct));
}

function bar(pct, level) {
	return E('div', { 'class': 'cbi-progressbar', 'style': 'height:10px' },
		E('div', { 'style': 'width:' + (pct === null ? 0 : pct).toFixed(0) + '%;height:100%;background:' +
			(level === 'bad' ? '#e24b4a' : level === 'fair' ? '#ef9f27' : '#639922') }));
}

function metric(title, value, sub, pct) {
	var level = pct === null ? 'bad' : pct >= 50 ? 'good' : pct >= 20 ? 'fair' : 'bad';

	return E('div', { 'class': 'cbi-section', 'style': 'display:inline-block;width:23%;min-width:140px;vertical-align:top;margin-right:1%' }, [
		E('h3', {}, title),
		E('div', { 'style': 'font-size:20px;margin-bottom:4px' }, value),
		bar(pct, level),
		E('div', { 'class': 'hint' }, sub || '')
	]);
}

return view.extend({
	load: function() {
		return api.status();
	},

	render: function(state) {
		var self = this;

		this.banner = E('div', {});
		this.metrics = E('div', {});
		this.tables = E('div', {});
		this.container = E('div', {}, [ this.banner, this.metrics, this.tables ]);
		this.paint(state);

		poll.add(function() {
			if (!document.body.contains(self.container)) {
				poll.stop();
				return Promise.resolve();
			}
			return api.profile(true).then(api.status).then(function(s) {
				self.paint(s);
			});
		}, 2);

		return this.container;
	},

	paint: function(state) {
		var cesq = state.cesq || {};
		var rssi = state.rssi_dbm !== undefined ? state.rssi_dbm : cesq.rssi_dbm;
		var rsrp = cesq.rsrp_dbm;
		var rsrq = cesq.rsrq_db10 !== undefined ? cesq.rsrq_db10 / 10 : undefined;
		var sinr = state.cell_valid ? state.serving.sinr : undefined;

		this.banner.innerHTML = '';
		if (!state.cell_valid)
			this.banner.appendChild(E('div', { 'class': 'alert-message warning' },
				_('No cell information yet. Cell data is only polled while this page is open - give it a few seconds.')));

		this.metrics.innerHTML = '';
		this.metrics.appendChild(metric('RSSI',
			api.fmtDbm(rssi),
			_('from AT+CSQ / AT+CESQ rxlev'),
			scale(rssi, -113, -51)));
		this.metrics.appendChild(metric('RSRP',
			api.fmtDbm(rsrp),
			_('reference signal received power'),
			scale(rsrp, -140, -70)));
		this.metrics.appendChild(metric('RSRQ',
			rsrq === undefined ? '-' : (rsrq.toFixed(1) + ' dB'),
			_('reference signal received quality'),
			rsrq === undefined ? null : scale(rsrq, -20, -3)));
		this.metrics.appendChild(metric('SINR',
			sinr === undefined ? '-' : (sinr + ' dB'),
			_('signal to interference plus noise'),
			sinr === undefined ? null : scale(sinr, -20, 30)));

		this.tables.innerHTML = '';

		if (state.cell_valid) {
			var c = state.serving;

			this.tables.appendChild(E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Serving cell')),
				E('table', { 'class': 'table' }, [
					E('tr', { 'class': 'tr' }, [
						E('th', { 'class': 'th' }, _('RAT')),
						E('th', { 'class': 'th' }, _('PLMN')),
						E('th', { 'class': 'th' }, 'TAC'),
						E('th', { 'class': 'th' }, _('Cell ID')),
						E('th', { 'class': 'th' }, 'EARFCN'),
						E('th', { 'class': 'th' }, 'PCI'),
						E('th', { 'class': 'th' }, _('Band')),
						E('th', { 'class': 'th' }, 'RSRP'),
						E('th', { 'class': 'th' }, 'RSRQ'),
						E('th', { 'class': 'th' }, 'SINR')
					]),
					E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td' }, api.ratName(c.rat)),
						E('td', { 'class': 'td' }, c.mcc ? (c.mcc + '-' + c.mnc) : '-'),
						E('td', { 'class': 'td' }, String(c.tac || '-')),
						E('td', { 'class': 'td' }, String(c.cellid || '-')),
						E('td', { 'class': 'td' }, String(c.earfcn || '-')),
						E('td', { 'class': 'td' }, c.pci >= 0 ? String(c.pci) : '-'),
						E('td', { 'class': 'td' }, c.band ? ('B' + c.band) : '-'),
						E('td', { 'class': 'td' }, api.fmtDbm(c.rsrp)),
						E('td', { 'class': 'td' }, c.rsrq ? (c.rsrq + ' dB') : '-'),
						E('td', { 'class': 'td' }, c.sinr ? (c.sinr + ' dB') : '-')
					])
				])
			]));
		}

		var neigh = state.neighbours || [];

		if (neigh.length) {
			var rows = [ E('tr', { 'class': 'tr' }, [
				E('th', { 'class': 'th' }, '#'),
				E('th', { 'class': 'th' }, _('RAT')),
				E('th', { 'class': 'th' }, 'EARFCN'),
				E('th', { 'class': 'th' }, 'PCI'),
				E('th', { 'class': 'th' }, _('Band')),
				E('th', { 'class': 'th' }, 'RSRP'),
				E('th', { 'class': 'th' }, 'RSRQ'),
				E('th', { 'class': 'th' }, 'SINR')
			]) ];

			neigh.forEach(function(n, i) {
				rows.push(E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, String(i + 1)),
					E('td', { 'class': 'td' }, api.ratName(n.rat)),
					E('td', { 'class': 'td' }, String(n.earfcn || '-')),
					E('td', { 'class': 'td' }, n.pci >= 0 ? String(n.pci) : '-'),
					E('td', { 'class': 'td' }, n.band ? ('B' + n.band) : '-'),
					E('td', { 'class': 'td' }, api.fmtDbm(n.rsrp)),
					E('td', { 'class': 'td' }, n.rsrq ? (n.rsrq + ' dB') : '-'),
					E('td', { 'class': 'td' }, n.sinr ? (n.sinr + ' dB') : '-')
				]));
			});

			this.tables.appendChild(E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Neighbour cells')),
				E('table', { 'class': 'table' }, rows),
				E('p', { 'class': 'hint' },
				  _('Locking the modem to one of these cells is part of the band/cell locking page.'))
			]));
		}
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
