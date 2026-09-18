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

var QUICK = [
	'AT',
	'ATI',
	'AT+CGMI',
	'AT+CGMM',
	'AT+CGMR',
	'AT+CGSN',
	'AT+CFSN',
	'AT+CCID',
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
	'AT+GTGPS?'
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
				  _('Commands are sent raw; fm160d appends the carriage return and waits for OK / ERROR / +CME ERROR. Timeout is 5 s.'))
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

		return api.at(cmd, 5000, '').then(function(res) {
			var status = res && res.status || 'unknown';
			var text = res && res.response ? res.response.replace(/\r/g, '') : '';

			this.statusline.innerHTML = '';
			this.statusline.appendChild(E('span', {
				'class': 'label ' + (status === 'ok' ? 'success' :
						     status === 'error' ? 'warning' : 'danger')
			}, status));

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
