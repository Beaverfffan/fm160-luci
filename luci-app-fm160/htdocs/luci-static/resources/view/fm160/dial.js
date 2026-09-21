'use strict';
'require view';
'require poll';
'require ui';
'require fs';
'require rpc';
'require fm160.api as api';

/*
 * Dial page (M2): the data plane.
 *
 * This page owns one thing - bringing the ECM link up and down - and it used to
 * share a page with the USB profile switch, which is a different kind of
 * operation entirely: dialling is a sequence of AT commands that can fail and be
 * retried, while switching the profile restarts the module and can leave it
 * beyond reach from the host.  The switch now has its own page; what is left here
 * is the part a user touches every day.
 *
 * Three rules shape everything below.
 *
 *   1. THE PAGE IS NOT THE GUARD.  Every button here is a request.  The daemon
 *      decides: fm160_dial_start() refuses a profile it does not dial, and
 *      fm160_dial_config() validates every value before it stores anything.  The
 *      filtering below is a convenience so the user is not offered a button that
 *      can only fail - it is never the reason something is safe.
 *
 *   2. THE CONNECT BUTTON DIALS ECM ONLY.  fm160d drives ECM because the module
 *      itself hands out the address over DHCP on usb0.  A QMI or MBIM profile is
 *      dialled by the kernel stack (uqmi / umbim) through netifd, and this daemon
 *      answers -ENOTSUP rather than pretending.  So the button is disabled with a
 *      reason instead of enabled and useless.
 *
 *   3. UNPLUGGING IS NOT DISCONNECTING.  The vendor's dial-up document is
 *      explicit: the context is deactivated by AT+GTWWAN=0,<cid> and by nothing
 *      else.  Pulling the cable, power-cycling the module or rebooting the router
 *      leaves the PDP context up on the network side.  A user who unplugged the
 *      stick and believes they are disconnected is a user who keeps being billed,
 *      so the page says so rather than leaving it implied.
 *
 * The AT port can also be absent because a profile switch is in progress - that
 * is expected, it is not a fault, and the wording of the banner says so.  The
 * switch itself, and the state of the module behind it, is on the USB mode page.
 *
 * Two settings on this page are NOT dial settings, and they are the two the
 * daemon has no opinion about: the defaults the package ships (offered at the
 * moment Connect is pressed, below) and the LAN's IPv6, which is a uci option
 * read by an init script rather than by fm160d.  They are handled here rather
 * than on a page of their own because they are both things that decide whether
 * the link a user just asked for is usable, and a link that comes up without
 * IPv6 is exactly the surprise this page exists to prevent.
 */

/* --- small helpers, in the house style of the cells page --------------- */

function info(text) {
	ui.addNotification(null, E('p', {}, text), 'info');
}

function warn(text) {
	ui.addNotification(null, E('p', {}, text), 'warning');
}

function fail(text) {
	ui.addNotification(null, E('p', {}, text), 'danger');
}

/* A coloured level as a plain table cell class. */
function levelClass(level) {
	return level === 'ok' ? 'cbi-value-description' : '';
}

function row(label, value, cls) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td', 'style': 'width:34%' }, label),
		E('td', { 'class': 'td ' + (cls || '') }, value)
	]);
}

function section(title, children) {
	return E('div', { 'class': 'cbi-section' }, [ E('h3', {}, title) ].concat(children));
}

/* --- the one uci option this page owns -------------------------------- */

/*
 * fm160.main.lan_ipv6 is read by /etc/init.d/fm160-prefix, not by fm160d, so
 * there is no ubus method for it and the front end uses the uci rpc directly.
 * The app's ACL already grants read and write on the fm160 config, and it is
 * granted per-config on purpose: this page can touch fm160 and nothing else.
 *
 * The value is written and committed and then the service is asked to reload,
 * in that order.  Asking the service first would have it read the old value,
 * and a toggle that appears to do nothing until the next hotplug event is worse
 * than one that reports the failure.
 */
var callUciGet = rpc.declare({
	object: 'uci', method: 'get',
	params: [ 'config', 'section', 'option' ], expect: {}
});

var callUciSet = rpc.declare({
	object: 'uci', method: 'set',
	params: [ 'config', 'section', 'option', 'value' ], expect: {}
});

var callUciCommit = rpc.declare({
	object: 'uci', method: 'commit', params: [ 'config' ], expect: {}
});

function readLanIPv6() {
	return callUciGet('fm160', 'main', 'lan_ipv6')
		.then(function(r) { return (r && r.value) === '1'; })
		/* Unset reads as off, which is also how the script that owns the
		 * behaviour reads it - it tests for a literal "1". */
		.catch(function() { return false; });
}

/*
 * The LAN's IPv6 state, straight from the script that owns it, so the page
 * shows what is on the wire rather than what was requested.
 */
function readPrefixStatus() {
	return fs.exec('/usr/libexec/fm160-prefix-lan.sh', [ 'status' ])
		.then(function(res) { return (res && res.stdout) || ''; })
		.catch(function() { return null; });
}

function prefixField(txt, name) {
	var m = new RegExp('^' + name + '\\s+(.*)$', 'm').exec(txt || '');
	return m ? m[1].trim() : '';
}

/* --- a v4-only context, noticed at the moment it matters -------------- */

/*
 * The package now ships a dual-stack profile (dial_pdp IPV4V6) with the LAN's
 * IPv6 on by default, so a fresh install has IPv6 from its first dial and
 * nothing below fires.  What this catches is the other case: a PDP type of IP,
 * which is IPv4-only by definition.
 *
 * The reason that deserves a dialogue is that the failure is invisible from
 * here.  An IP context activates perfectly happily, gets an address and routes
 * IPv4; the only symptom is that the link -- and therefore the LAN -- has no
 * IPv6 at all, and "the router has no IPv6" is a search that starts in the
 * wrong place and usually ends at odhcpd.
 *
 * It is a question and not a rewrite, because IPv4-only is a legitimate choice:
 * a SIM that bills the two families separately is exactly the case for it.  The
 * ask is also skipped once the user has answered durably elsewhere --
 * connect-at-boot on means somebody configured this link on purpose, and
 * re-asking on every dial is how people learn to click through without reading.
 *
 * Resolves true (override then dial), false (dial as configured), or null
 * (cancelled).
 */
function askDialDefaults(state) {
	return new Promise(function(resolve) {
		function done(v) {
			ui.hideModal();
			resolve(v);
		}

		var btn = 'btn cbi-button';

		ui.showModal(_('This link would have no IPv6'), [
			E('p', {}, [
				_('The PDP type is IP, which asks the network for an IPv4-only context, and connect-at-boot is off.'),
				' ',
				_('An IPv4-only context carries no IPv6 at all - not on this link, and not on the LAN, whatever the LAN settings say.')
			]),
			E('p', {}, [
				_('That is a legitimate choice on a SIM that bills the two families separately.'),
				' ',
				_('If it is not what you meant, the context has to be asked for as IPV4V6.')
			]),
			E('p', { 'class': 'hint' }, [
				_('Overriding sets the PDP type to IPV4V6 and turns connect-at-boot on, so this link comes back by itself after a reboot.'),
				' ',
				_('An APN is still required either way - with an empty APN, Connect is refused.')
			]),
			E('div', { 'class': 'right' }, [
				E('button', {
					'class': btn,
					'click': function() { done(null); return Promise.resolve(); }
				}, _('Cancel')),
				' ',
				E('button', {
					'class': btn,
					'click': function() { done(false); return Promise.resolve(); }
				}, _('Connect as configured')),
				' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-apply',
					'click': function() { done(true); return Promise.resolve(); }
				}, _('Override and connect'))
			])
		]);
	});
}

/*
 * The foreground hold makes the daemon shorten its polling intervals and switch
 * on the tiers that only run for an open page.
 *
 * This page wants that while a dial is in flight - it is the only time the step
 * can change under the user's eyes - and NOT otherwise.  The tiers it turns on
 * include the cell poll (AT+GTCCINFO?), which this page has no use for and which
 * costs the one serial port real time.  Holding the foreground permanently would
 * be the easy thing to do and the wrong one.
 */
function holdForeground(st) {
	return api.dialWorking(st);
}

return view.extend({
	load: function() {
		/* Three independent reads.  The dial snapshot comes from fm160d; the
		 * other two are what this page needs to tell the truth about the LAN,
		 * and neither of them is in that snapshot. */
		return Promise.all([
			api.status(),
			readLanIPv6(),
			readPrefixStatus()
		]);
	},

	render: function(data) {
		var self = this;
		var state = data[0];

		this.lanIPv6 = !!data[1];
		this.prefixStatus = data[2];

		this.container = E('div', {});
		/* The settings form is built ONCE.  It holds text inputs, and the rest
		 * of the page is repainted every two seconds from the snapshot - a form
		 * rebuilt on that timer would delete the APN as it was being typed. */
		this.settings = this.renderSettings(state);
		this.paint(state);

		poll.add(function() {
			if (!document.body.contains(self.container)) {
				poll.stop();
				return Promise.resolve();
			}

			var hold = holdForeground(self.state);

			return (hold ? api.profile(true) : Promise.resolve())
				.then(api.status)
				.then(function(s) {
					self.paint(s);
				});
		}, 2);

		return this.container;
	},

	paint: function(state) {
		this.state = state;
		this.container.innerHTML = '';

		this.container.appendChild(this.renderBanner(state));
		this.container.appendChild(this.renderLink(state));
		this.container.appendChild(this.settings);
		this.container.appendChild(this.renderLanIPv6());
		this.container.appendChild(this.renderNotes(state));
	},

	/* --- what is wrong, if anything --------------------------------- */

	renderBanner: function(state) {
		var box = E('div', {}), d = api.dialOf(state);

		/* Ordered by severity, and the page shows ALL that apply: a link that
		 * gave up and a missing APN are independent problems, and fixing one
		 * does not reveal the other if only the first is shown. */
		if (api.dialStoppedHealing(state))
			box.appendChild(E('div', { 'class': 'alert-message warning' }, [
				_('fm160d has stopped trying to bring the link up: the reset allowance for the last 24 hours is spent.'),
				' ',
				d.allow_reset ?
					_('Reconnect to start again from the beginning of the ladder.') :
					_('Module resets are disabled in the settings below.')
			]));

		if (api.dialConfigError(state))
			box.appendChild(E('div', { 'class': 'alert-message warning' },
				_('Connect is refused until a usable APN is set below.')));

		if (!state.port_found)
			box.appendChild(E('div', { 'class': 'alert-message warning' },
				_('No AT port. The modem is either absent, or has not finished re-enumerating after a profile change.')));

		if (api.dialKindKnown(state) && !api.dialIsDaemons(api.dialKind(state)))
			box.appendChild(E('div', { 'class': 'alert-message notice' }, [
				_('This USB profile is not an ECM profile, so fm160d does not dial it.'),
				' ',
				_('QMI and MBIM links are brought up by the kernel stack through netifd - use the network configuration for that, and leave the button below alone.')
			]));

		return box;
	},

	/* --- the link --------------------------------------------------- */

	renderLink: function(state) {
		var self = this, d = api.dialOf(state);
		var usable = api.dialUsable(state), up = api.dialUp(state);
		var verb = api.dialVerb(state);
		var level = api.dialLevel(state);

		var colour = level === 'ok' ? 'green' : level === 'bad' ? 'red'
			   : level === 'warn' ? 'orange' : 'grey';

		var buttons = E('div', { 'class': 'cbi-page-actions', 'style': 'margin-top:8px' }, [
			E('button', {
				'class': 'btn cbi-button cbi-button-apply',
				'disabled': (!usable || up || d.wanted) ? 'disabled' : null,
				'click': ui.createHandlerFn(this, function() {
					return self.connect();
				})
			}, _('Connect')),
			' ',
			E('button', {
				'class': 'btn cbi-button cbi-button-reset',
				/* Disconnect stays enabled whenever anything was ever
				 * attempted: the one case that must not be silently removed is
				 * a link that is wanted but not yet up, because that is a
				 * dial in progress and stopping it is exactly what the user
				 * wants. */
				'disabled': (!d.wanted && !up) ? 'disabled' : null,
				'click': ui.createHandlerFn(this, function() {
					return api.dialStop().then(function(res) {
						self.reportDial(res);
					});
				})
			}, _('Disconnect', 'fm160 dial action'))
		]);

		return section(_('Link'), [
			E('p', { 'style': 'font-size:15px' }, [
				E('span', { 'style': 'display:inline-block;width:10px;height:10px;border-radius:50%;background:' + colour + ';margin-right:6px' }, ''),
				api.dialStateText(state)
			]),
			E('table', { 'class': 'table' }, [
				row(_('Step'), E('span', { 'class': levelClass(level) }, _(api.dialStep(state)))),
				row(_('USB profile'), api.dialKindKnown(state) ?
					(api.dialKind(state) + ' (' + _('mode') + ' ' +
					 (api.modeswCurrent(state) === null ? '?' : api.modeswCurrent(state)) + ')') :
					_('not read yet')),
				row(_('Address'), api.dialAddress(state) || '-'),
				row('DNS', [ d.dns1 || '', d.dns2 || '' ].filter(function(x) { return x; }).join(', ') || '-'),
				row(_('PDP context'), api.dialPdp(state) + ' / ' + _('cid') + ' ' + api.dialCid(state)),
				row(_('Activation verb'),
					verb === null ?
						E('span', {}, _('not probed yet - it is learned from the first attempt')) :
						'AT+' + verb),
				row(_('Attempts'), api.fmtNum(api.dialAttempt(state)) + ' / ' +
					api.fmtNum(d.ladder_len)),
				row(_('Next try'), api.reported(d.next_try_in_ms) ?
					_('in') + ' ' + api.fmtAge(d.next_try_in_ms) :
					d.wanted ? _('now') : '-'),
				row(_('Module resets'), api.dialResetText(state) || '-')
			]),
			d.last_error ? E('p', { 'class': 'hint' }, _('Last error') + ': ' + d.last_error) : '',
			buttons,
			E('p', { 'class': 'hint' }, [
				E('strong', {}, _('Press Disconnect to release the session.')),
				' ',
				_('Unplugging the stick, power-cycling the module or rebooting the router does NOT deactivate the PDP context on the network side - the modem keeps it up until it is told otherwise.'),
				' ',
				_('Automatic recovery is a ladder: three tries, then a module reset, and at most three resets in 24 hours.')
			])
		]);
	},

	reportDial: function(res) {
		var result = (res && res.result) || 'unknown';
		var detail = (res && res.detail) || '';

		if (result === 'started')
			info(_('The dial has started. The steps above follow it.'));
		else if (result === 'bad-config')
			fail(_('Refused: no usable APN. An APN may contain letters, digits, dot, hyphen and underscore, and nothing else - a quote, a comma or a semicolon would end the AT argument early and turn the rest of the APN into further fields.'));
		else if (result === 'wrong-profile')
			warn(_('Refused: this USB profile is not an ECM profile. QMI and MBIM links are dialled by the kernel stack, not by fm160d.'));
		else if (result === 'not-ready')
			warn(_('Refused for now: there is no AT port, or the modem has not been asked which profile it is in. Try again once it is visible.'));
		else
			fail(_('The dial request failed') + (detail ? ': ' + detail : ''));
	},

	refresh: function() {
		var self = this;

		return api.status().then(function(s) {
			self.state = s;
			self.paint(s);
		});
	},

	/*
	 * Connect, with a v4-only context called out once - and only when there is
	 * something to call out.
	 *
	 * The package ships IPV4V6, so this does nothing on a fresh install.  It
	 * fires for a context that is IP, and only then while connect-at-boot is
	 * off: a user who turned that on has configured this link on purpose, and
	 * re-asking on every dial attempt would be the kind of dialogue people
	 * learn to click through without reading.
	 */
	connect: function() {
		var self = this, d = api.dialOf(this.state);
		var v4Only = (api.dialPdp(this.state) === 'IP' && !d.autostart);

		var ask = v4Only ? askDialDefaults(this.state) : Promise.resolve(false);

		return ask.then(function(override) {
			if (override === null)
				return null;

			if (!override)
				return api.dialStart();

			/* The override goes through the daemon like every other write on
			 * this page, so it is validated and stored in one step and the
			 * reply is what was actually stored.  autostart is set alongside
			 * the PDP type because a dual-stack context that nobody brings
			 * back up after a reboot is half an override. */
			return api.dialConfig(api.dialApn(self.state), 'IPV4V6',
					      api.dialCid(self.state), d.allow_reset, true)
				.then(function(res) {
					info(_('Context set to IPV4V6, connect-at-boot on.'));
					/* res is the stored configuration; the dial starts with
					 * whatever the daemon just confirmed, not with what was
					 * typed here. */
					return api.dialStart();
				});
		}).then(function(res) {
			if (res === null)
				return null;		/* cancelled, nothing to report */

			self.reportDial(res);
			return self.refresh();
		});
	},

	/* --- the dial settings ------------------------------------------ */

	/*
	 * Saved through fm160.dial_config, never by writing uci from here.
	 *
	 * That method validates every value before it stores anything and then
	 * re-reads what it wrote, so the file and the running daemon cannot
	 * disagree.  A page that wrote uci itself would have to ask for a reload
	 * afterwards, and the window between the two writes is a state where the
	 * displayed settings and the effective ones are different things.
	 */
	renderSettings: function(state) {
		var self = this;
		var d = api.dialOf(state);

		var apn = E('input', {
			'type': 'text',
			'class': 'cbi-input-text',
			'value': api.dialApn(state),
			'placeholder': 'internet',
			'style': 'width:20em'
		});

		function select(values, current) {
			return E('select', { 'class': 'cbi-input-select' }, values.map(function(v) {
				return E('option', { 'value': v, 'selected': v === current ? 'selected' : null }, v);
			}));
		}

		var pdp  = select([ 'IP', 'IPV6', 'IPV4V6' ], api.dialPdp(state));
		var cid  = E('input', {
			'type': 'text',
			'class': 'cbi-input-text',
			'value': String(api.dialCid(state)),
			'style': 'width:5em'
		});
		var autostart = E('input', { 'type': 'checkbox', 'checked': d.autostart ? '' : null });
		var allowReset = E('input', { 'type': 'checkbox', 'checked': d.allow_reset ? '' : null });

		function save(ev) {
			ev.currentTarget.blur();

			var body = {
				apn: apn.value.trim(),
				pdp: pdp.value,
				cid: parseInt(cid.value, 10),
				autostart: !!autostart.checked,
				allow_reset: !!allowReset.checked
			};

			if (!(body.cid >= 1)) {
				fail(_('The PDP context number has to be 1 or higher.'));
				return;
			}

			return api.dialConfig(body.apn, body.pdp, body.cid,
					      body.allow_reset, body.autostart)
				.then(function(res) {
					/* The reply is what was actually stored, so the form is
					 * reseeded from it rather than from what was typed. */
					apn.value = res.apn || '';
					pdp.value = res.pdp || 'IP';
					cid.value = String(res.cid);
					autostart.checked = !!res.autostart;
					allowReset.checked = !!res.allow_reset;
					info(_('Settings saved.'));
					self.paint(self.state);
				})
				.catch(function(e) {
					fail(_('The daemon rejected these settings:') + ' ' +
					     _('the APN, the PDP type or the context number is not valid.') +
					     ' (' + String((e && e.message) || e) + ')');
				});
		}

		return section(_('Dial settings'), [
			E('table', { 'class': 'table' }, [
				row(_('APN'),
					E('div', {}, [ apn, ' ',
						E('span', { 'class': 'hint' }, _('empty means Connect is refused')) ])),
				row(_('PDP type'), E('div', {}, [ pdp, ' ',
					E('span', { 'class': 'hint' }, _('IP is the common case; IPV4V6 asks for a dual-stack context')) ])),
				row(_('Context (cid)'), cid),
				row(_('Connect at boot'), E('div', {}, [ autostart, ' ',
					E('span', { 'class': 'hint' }, _('bring the link up as soon as the daemon starts')) ])),
				row(_('Allow module resets'), E('div', {}, [ allowReset, ' ',
					E('span', { 'class': 'hint' }, _('after three failed attempts fm160d may reset the module - at most three times in 24 hours. Off by default: a reset drops every bearer, including one another process may be using.')) ]))
			]),
			E('div', { 'class': 'cbi-page-actions' }, [
				E('button', {
					'class': 'btn cbi-button cbi-button-save',
					'click': save
				}, _('Save'))
			])
		]);
	},

	/* --- the LAN's IPv6 --------------------------------------------- */

	/*
	 * This is the one thing on the page that is not the modem's business, and it
	 * is here because it is the difference between "the link is up" and "the link
	 * is usable".
	 *
	 * The ECM link gets a single /64 from the network and no IA_PD.  With no
	 * prefix to hand out, odhcpd has nothing to advertise but the router's ULA -
	 * and a ULA only becomes a usable prefix after ra_default is set, so by
	 * default LAN clients get a router lifetime of zero, which is the protocol
	 * saying "do not use me as a gateway".  The LAN has no global IPv6 at all.
	 *
	 * Turning fm160.main.lan_ipv6 on hands that same /64 to the LAN.  The router
	 * keeps the operator prefix on the WAN and puts a second address out of it on
	 * br-lan, which is all odhcpd needs: it reads addresses straight off netlink,
	 * advertises the prefix as on-link with a non-zero lifetime, and delegates a
	 * /62 out of it to the downstream router on the LAN.  No NAT, no translation,
	 * no firewall rule - and no NPTv6, which this kernel cannot run correctly
	 * (the rewrite sits at mangle priority -150, behind conntrack at -200, so
	 * replies are classified as new connections before they are un-rewritten).
	 *
	 * The one thing the kernel will not do is answer neighbour solicitations for
	 * a whole prefix: its own proxy_ndp is per-host by design, and the module
	 * resolves every destination that way, so inbound would blackhole.  Which
	 * mechanism covers that was measured, not assumed - a separate proxy daemon
	 * never answered at all on this board, a kernel proxy entry works but needs
	 * one entry per client address, and odhcpd's own ndp relay works with no
	 * daemon, no sysctl and no list to maintain.  So the relay is what this
	 * turns on.
	 *
	 * What is shown here is read back from the script that does the work, not
	 * from the option, so the two can never disagree on screen.
	 */
	renderLanIPv6: function() {
		var self = this;
		var st = this.prefixStatus;

		var cb = E('input', { 'type': 'checkbox', 'checked': this.lanIPv6 ? '' : null });

		function save(ev) {
			ev.currentTarget.blur();
			var want = cb.checked;

			return callUciSet('fm160', 'main', 'lan_ipv6', want ? '1' : '0')
				.then(function() { return callUciCommit('fm160'); })
				.then(function() { return fs.exec('/etc/init.d/fm160-prefix', [ 'reload' ]); })
				.then(function() {
					self.lanIPv6 = want;
					info(_('Saved. The fields above are read back from the router.'));
					return self.refreshLan();
				})
				.catch(function(e) {
					cb.checked = self.lanIPv6 ? '' : null;
					fail(_('Could not change this:') + ' ' + String((e && e.message) || e));
				});
		}

		var rows = [];

		if (st === null) {
			rows.push(row(_('State'), _('the script that owns this is not installed')));
		}
		else {
			var op = prefixField(st, 'operator /64');
			var inst = prefixField(st, 'installed');
			var relay = prefixField(st, 'odhcpd relay');

			rows.push(row(_('Link'), op ?
				E('span', {}, op) :
				E('span', { 'class': 'hint' }, _('no global prefix on the mobile link yet - is it connected?'))));

			rows.push(row(_('On the LAN'), inst ?
				E('span', {}, inst + ' ' + _('(router address and route installed)')) :
				E('span', { 'class': 'hint' }, _('not installed'))));

			rows.push(row(_('Home-side lookup'), relay === 'relay' ?
				E('span', {}, _('odhcpd relays neighbour discovery for the LAN')) :
				E('span', { 'class': 'hint' }, _('not configured - inbound connections would not resolve'))));
		}

		return section(_('LAN IPv6'), [
			E('table', { 'class': 'table' }, rows),
			E('label', { 'style': 'display:block;margin:10px 0' }, [
				cb, ' ',
				_('Give the LAN the operator prefix, so its clients get real global addresses')
			]),
			E('p', { 'class': 'hint' }, [
				_('On: LAN clients get a global address from the operator prefix by ordinary router advertisement, and are reachable from the internet, because nothing is translated.'),
				' ',
				_('The address changes when the operator prefix changes (a reconnect or a module reset), so this is not a stable address for a server.'),
				' ',
				_('Off: the LAN keeps its ULA and its IPv4, and has no global IPv6.')
			]),
			E('div', { 'class': 'cbi-page-actions' }, [
				E('button', {
					'class': 'btn cbi-button cbi-button-save',
					'click': save
				}, _('Save'))
			])
		]);
	},

	refreshLan: function() {
		var self = this;

		return readPrefixStatus().then(function(txt) {
			self.prefixStatus = txt;
			self.paint(self.state);
			return null;
		});
	},

	/* --- what this page deliberately does not do -------------------- */

	renderNotes: function(state) {
		return section(_('Notes', 'fm160 dial notes heading'), [
			E('ul', { 'class': 'hint' }, [
				E('li', {}, [
					_('The ECM profile is dialled by fm160d itself, and a'), ' ',
					E('code', {}, 'proto fm160'), ' ',
					_('interface exists so that netifd can own the address side of it. QMI and MBIM profiles are dialled by the kernel (uqmi / umbim) instead.')
				]),
				E('li', {}, [
					_('The activation verb is probed rather than assumed. The vendor\'s AT manual says ECM uses +GTWWAN and only RNDIS uses +GTRNDIS, while the same vendor\'s dial-up document shows AT+GTRNDIS in its ECM chapter - so fm160d tries one and remembers which answered.')
				]),
				E('li', {}, [
					_('Switching the USB profile restarts the module and is on its own page - it is not part of bringing the link up.')
				]),
				E('li', {}, [
					_('This unit has no SIM card fitted, so the successful path of the dial ladder - registration, activation, address - has never run on real hardware. The refusals and the state machine have; the success path is code under test only.')
				])
			])
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
