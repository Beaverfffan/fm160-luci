'use strict';
'require view';
'require ui';
'require fm160.api as api';

/*
 * AT debug page.
 *
 * This is the only place in the UI that can put an arbitrary command on the
 * wire.  Requests go in at interactive priority (they overtake the polling
 * queue), the daemon opens a 10 s quiet window around them so nothing else
 * interleaves, and the raw response is shown verbatim - including ERROR, which
 * is often the most informative answer.
 */

/*
 * Commands that need longer than the default timeout, measured on real hardware
 * (FM160-CN 89614.1000.00.04.01.02 with no SIM inserted -- the worst case,
 * because several of these block until the modem's own internal timeout):
 *
 *   AT+GTPKGVER?  12.9 s      AT+CCID      10.3 s      AT+CPMS?  10.3 s
 *   AT+CMGF?       5.4 s      AT+CIMI       5.2 s      AT+CGDCONT? 5.2 s
 *
 * At a flat 5 s these come back as "timeout", which reads as a broken modem
 * rather than as a slow query.
 *
 * AT+ICCID answers the same question as AT+CCID in 18 ms, so it is offered
 * instead; AT+CCID stays in the slow table because people type it from habit.
 */
var SLOW_MS = {
	'AT+GTPKGVER?': 20000,
	'AT+CCID':      20000,
	'AT+CPMS?':     20000,
	'AT+CMGF?':     12000,
	'AT+CIMI':      12000,
	'AT+CGDCONT?':  12000,
	/* The one GNSS command whose ANSWER TIME has never been measured: every
	 * other GNSS command came back in well under 500 ms, but this one was
	 * never answered on this hardware at all.  A generous timeout costs
	 * nothing here and turns a possible slow answer into an answer. */
	'AT+GTGPSCFG=?': 12000
};
var DEFAULT_TIMEOUT_MS = 5000;

function timeoutFor(cmd) {
	return SLOW_MS[cmd.toUpperCase()] || DEFAULT_TIMEOUT_MS;
}

var QUICK = [
	'AT',
	'ATI',
	'AT+CGMI',
	'AT+CGMM',
	'AT+CGMR',
	'AT+CGSN',
	'AT+CFSN',
	'AT+ICCID',
	'AT+CPIN?',
	'AT+CFUN?',
	'AT+CSQ',
	'AT+CESQ',
	'AT+CEREG?',
	'AT+C5GREG?',
	'AT+COPS?',
	'AT+GTUSBMODE?',
	'AT+GTUSBMODE=?',
	'AT+GTCCINFO?',
	'AT+GTCAINFO?',
	'AT+GTACT?',
	'AT+GTACT=?',
	'AT+GTCELLLOCK?',
	'AT+CGDCONT?',
	'AT+GTWWAN?',
	'AT+GTRNDIS?',
	'AT+CPMS?',
	'AT+CMGF?',
	'AT+CNMI?',
	'AT+GTGPSPOWER?',
	'AT+GTGPS?',
	/* GNSS, the parts that are safe to poke by hand.  AT+GTGPS=<item> needs
	 * the item in double quotes - unquoted it answers ERROR, which looks like
	 * a broken receiver and is not.  AT+GTGPSCFG=? has never been answered on
	 * this hardware, so it is here to be looked at rather than trusted. */
	'AT+GTGPS="RMC"',
	'AT+GTGPSCFG?',
	'AT+GTGPSCFG=?',
	'AT+GTGPSEPO?',
	'AT+GTAGPSSERV?'
];

return view.extend({
	render: function() {
		var self = this;

		this.input = E('input', {
			'type': 'text',
			'class': 'cbi-input-text',
			'style': 'width:100%;font-family:monospace',
			'placeholder': 'AT+CSQ',
			'keydown': function(ev) {
				if (ev.key === 'Enter') {
					ev.preventDefault();
					self.send();
				}
			}
		});

		this.output = E('pre', {
			'style': 'max-height:26em;overflow:auto;white-space:pre-wrap;word-break:break-all;' +
				 'font-family:monospace;font-size:12px;padding:8px;border:1px solid rgba(0,0,0,0.15);border-radius:8px'
		}, '');

		var quick = E('div', {}, QUICK.map(function(cmd) {
			return E('button', {
				'class': 'btn cbi-button',
				'style': 'margin:0 4px 4px 0;font-family:monospace',
				'click': function() {
					self.input.value = cmd;
					self.send();
				}
			}, cmd);
		}));

		this.statusline = E('p', { 'class': 'hint' }, '');

		var body = E('div', {}, [
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Command')),
				E('div', {}, [ this.input, ' ',
					E('button', {
						'class': 'btn cbi-button-action',
						'click': ui.createHandlerFn(this, this.send)
					}, _('Send'))
				]),
				E('p', { 'class': 'hint' },
				  _('Commands are sent raw; fm160d appends the carriage return and waits for OK / ERROR / +CME ERROR. Most commands time out after 5 s; the ones that are known to be slow on real hardware get up to 20 s.'))
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Common queries')),
				quick
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Response')),
				this.statusline,
				this.output
			])
		]);

		return body;
	},

	send: function() {
		var cmd = (this.input.value || '').trim();

		if (!cmd) {
			ui.addNotification(null, E('p', {}, _('Enter a command first.')), 'warning');
			return Promise.resolve();
		}

		this.append('> ' + cmd);

		var limit = timeoutFor(cmd);

		return api.at(cmd, limit, '').then(function(res) {
			var status = res && res.status || 'unknown';
			var text = res && res.response ? res.response.replace(/\r/g, '') : '';

			this.statusline.innerHTML = '';
			this.statusline.appendChild(E('span', {
				'class': 'label ' + (status === 'ok' ? 'success' :
						     status === 'error' ? 'warning' : 'danger')
			}, status));
			this.statusline.appendChild(E('span', { 'class': 'hint' },
				' ' + _('limit') + ': ' + (limit / 1000) + ' s'));

			this.append(text.trim() ? text.trimEnd() : _('(empty response)'));
		}.bind(this)).catch(function(e) {
			this.append(_('call failed') + ': ' + e);
		}.bind(this));
	},

	append: function(text) {
		var el = this.output;

		el.textContent += (el.textContent ? '\n' : '') + text;
		el.scrollTop = el.scrollHeight;
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
