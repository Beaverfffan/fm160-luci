'use strict';
'require view';
'require poll';
'require ui';
'require fm160.api as api';

/*
 * Overview: identity, SIM and registration state, link health and traffic.
 *
 * Everything on this page comes from fm160d's cache.  The only thing this page
 * does that touches the daemon's behaviour is the periodic profile() ping, which
 * tells it a UI is open so the faster polling tier becomes active.
 */

function kv(rows) {
	return E('table', { 'class': 'table' }, rows.map(function(r) {
		return E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'style': 'width:34%' }, r[0]),
			E('td', { 'class': 'td left' }, r[1])
		]);
	}));
}

function section(title, body) {
	return E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, title),
		body
	]);
}

var BADGE = {
	ok:   { cls: 'label success', text: null },
	warn: { cls: 'label warning', text: null },
	down: { cls: 'label danger',  text: null }
};

return view.extend({
	load: function() {
		return api.status();
	},

	render: function(state) {
		var self = this;

		this.banner = E('div', {});
		this.body   = E('div', {});
		this.footer = E('div', {});

		var actions = E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Actions')),
			E('div', { 'class': 'cbi-page-actions', 'style': 'text-align:left' }, [
				E('button', {
					'class': 'btn cbi-button-action',
					'click': ui.createHandlerFn(this, function() {
						ui.showModal(_('Re-probe the AT port'),
							[ E('p', {}, _('fm160d will drop the current port and scan /dev/ttyUSB* again. Safe to run at any time.')),
							  E('div', { 'class': 'right' }, [
								E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')),
								' ',
								E('button', { 'class': 'btn cbi-button-action', 'click': function() {
									ui.hideModal();
									api.rescan().then(self.refresh.bind(self));
								} }, _('Re-probe'))
							  ]) ]);
					})
				}, _('Re-probe AT port')),
				' ',
				E('button', {
					'class': 'btn cbi-button-action',
					'click': ui.createHandlerFn(this, function() {
						return api.ident().then(self.refresh.bind(self));
					})
				}, _('Re-read module identity')),
				' ',
				E('button', {
					'class': 'btn cbi-button-' + (state.enabled ? 'reset' : 'apply'),
					'click': ui.createHandlerFn(this, function() {
						var next = !state.enabled;
						if (next === false) {
							return ui.showModal(_('Pause FM160 management'),
								[ E('p', {}, _('This stops all automatic polling. The modem keeps working; only this manager goes quiet until you enable it again.')),
								  E('div', { 'class': 'right' }, [
									E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')),
									' ',
									E('button', { 'class': 'btn cbi-button-reset', 'click': function() {
										ui.hideModal();
										api.setEnabled(false).then(self.refresh.bind(self));
									} }, _('Pause'))
								  ]) ]);
						return api.setEnabled(true).then(self.refresh.bind(self));
					})
				}, state.enabled ? _('Pause management') : _('Resume management'))
			])
		]);

		this.container = E('div', {}, [ this.banner, this.body, actions, this.footer ]);
		this.paint(state);

		poll.add(function() {
			/* Stop as soon as this view is gone, otherwise the poller would
			 * keep the daemon in the fast tier forever. */
			if (!document.body.contains(self.container)) {
				poll.stop();
				return Promise.resolve();
			}
			return api.profile(true)
				.then(api.status)
				.then(function(s) {
					self.state = s;
					self.paint(s);
				});
		}, 2);

		return this.container;
	},

	paint: function(state) {
		var health = api.atHealth(state);

		this.banner.innerHTML = '';
		this.banner.appendChild(E('div', {
			'class': 'alert-message ' + (health.level === 'ok' ? 'success' :
						     health.level === 'warn' ? 'warning' : 'danger')
		}, [
			E('strong', {}, health.text),
			state.port ? ' \u2014 ' + _('port') + ': ' + state.port : '',
			state.quiet ? E('span', {}, ' \u2014 ' + _('quiet window') + ': ' + (state.quiet_reason || '') +
				' (' + Math.ceil((state.quiet_left_ms || 0) / 1000) + ' s)') : ''
		]));

		if (!state.enabled)
			this.banner.appendChild(E('div', { 'class': 'alert-message warning' },
				_('Management is paused. No polling is running.')));

		var identity = [
			[ _('Manufacturer'),   state.manufacturer || '-' ],
			[ _('Model'),          state.model || '-' ],
			[ _('Firmware'),       state.revision || '-' ],
			[ _('IMEI'),           state.imei || '-' ],
			[ _('Serial number'),  state.sn || '-' ],
			[ _('ICCID'),          state.iccid || '-' ],
			[ _('SIM status'),     state.pin_status || '-' ],
			[ _('USB profile'),    state.usb_mode >= 0 ? api.usbModeLabel(state.usb_mode) :
						E('em', {}, _('unknown - not yet read')) ],
			[ _('Queued AT'),      String(state.queue_depth || 0) ],
			[ _('Slowest reply'),  api.fmtAge(state.worst_response_ms) ]
		];

		var reg = [
			[ _('Operator'),            state.operator || '-' ],
			[ _('LTE registration'),    api.regName(state.cereg) ],
			[ _('NR registration'),     api.regName(state.c5greg) ],
			[ _('Serving RAT'),         state.cell_valid ? api.ratName(state.serving.rat) : '-' ],
			[ _('Band'),                state.cell_valid && state.serving.band ?
						    'B' + state.serving.band : '-' ],
			[ _('RSSI (AT+CSQ)'),       state.csq_rssi == 99 ? _('unknown') :
						    (state.csq_rssi + ' (' + api.fmtDbm(state.rssi_dbm) + ')') ],
			[ _('RSRP'),                api.fmtDbm(state.cesq && state.cesq.rsrp_dbm) ],
			[ _('RSRQ'),
			  state.cesq && state.cesq.rsrq_db10 !== undefined ?
				((state.cesq.rsrq_db10 / 10).toFixed(1) + ' dB') : '-' ],
			[ _('Last successful AT'),  api.fmtAge(state.last_ok_age_ms) ]
		];

		var traffic = [
			[ _('Interface'), state.traffic.netdev || _('not detected') ],
			[ _('Received'),  api.fmtBytes(state.traffic.rx_bytes) ],
			[ _('Sent'),      api.fmtBytes(state.traffic.tx_bytes) ],
			[ _('Down'),      api.fmtRate(state.traffic.rx_bps) ],
			[ _('Up'),        api.fmtRate(state.traffic.tx_bps) ]
		];

		this.body.innerHTML = '';
		this.body.appendChild(section(_('Module'), kv(identity)));
		this.body.appendChild(section(_('Network'), kv(reg)));

		if (state.cell_valid) {
			var c = state.serving;
			this.body.appendChild(section(_('Serving cell'), kv([
				[ _('PLMN'),   (c.mcc ? (c.mcc + '-' + c.mnc) : '-') ],
				[ _('TAC'),    c.tac || '-' ],
				[ _('Cell ID'), String(c.cellid || '-') ],
				[ _('EARFCN'), String(c.earfcn || '-') ],
				[ _('PCI'),    c.pci >= 0 ? String(c.pci) : '-' ],
				[ _('Bandwidth'),
				  c.bandwidth ? (c.bandwidth + ' ' + _('RB') ) : '-' ]
			])));
		}

		this.body.appendChild(section(_('Traffic'), kv(traffic)));

		this.footer.innerHTML = '';
		if (state.port_found && state.at_state === 0)
			this.footer.appendChild(E('div', { 'class': 'cbi-section' }, [
				E('p', { 'class': 'hint' },
				  _('Polling is tiered: registration every 5 s while a page is open, cells every 10 s, and byte counters are read from sysfs so they cost no AT commands at all.'))
			]));
	},

	refresh: function() {
		var self = this;

		return api.status().then(function(s) {
			self.state = s;
			self.paint(s);
		});
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
