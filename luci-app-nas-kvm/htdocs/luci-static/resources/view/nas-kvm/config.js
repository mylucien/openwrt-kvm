'use strict';
'require view';
'require form';
'require fs';
'require ui';
'require uci';
'require rpc';
'require poll';

var callServiceList = rpc.declare({
	object: 'service',
	method: 'list',
	params: [ 'name' ],
	expect: { '': {} }
});

var CTL = '/usr/bin/nas-kvm-ctl';
var INIT = '/etc/init.d/nas-kvm';

function genToken() {
	var a = new Uint8Array(16), s = '', i;
	window.crypto.getRandomValues(a);
	for (i = 0; i < a.length; i++) s += ('0' + a[i].toString(16)).slice(-2);
	return s;
}

function notify(msg, cls) {
	ui.addNotification(null, E('p', msg), cls || 'info');
}

function ctl(args, parseJson) {
	return fs.exec_direct(CTL, args, parseJson ? 'json' : 'text');
}

return view.extend({
	load: function() {
		return Promise.all([
			L.resolveDefault(fs.list('/dev'), []),
			uci.load('nas-kvm')
		]);
	},

	renderStatus: function(svc) {
		var inst = (svc && svc['nas-kvm'] && svc['nas-kvm'].instances) || {};
		function line(name, label) {
			var i = inst[name];
			return E('div', {}, [
				E('strong', {}, label + '：'),
				i && i.running
					? E('span', { 'style': 'color:#2e9e5b' }, _('运行中') + ' (PID ' + i.pid + ')')
					: E('span', { 'style': 'color:#d64545' }, _('未运行'))
			]);
		}
		return E('div', { 'class': 'cbi-section-descr' }, [
			line('kvmd', 'nas-kvmd'), line('ustreamer', 'ustreamer')
		]);
	},

	render: function(data) {
		var ttys = (data[0] || []).filter(function(e) { return /^tty(USB|ACM)\d+$/.test(e.name); })
			.map(function(e) { return '/dev/' + e.name; });
		var self = this, m, s, o;

		m = new form.Map('nas-kvm', _('NAS KVM'),
			_('通过光猫远程控制 NAS：HDMI 视频回传、键盘鼠标（CH9329）和开机键宏。') + ' ' +
			_('配置保存在 /etc/config/nas-kvm；「保存并应用」会重启服务。'));

		s = m.section(form.NamedSection, 'main', 'nas-kvm');
		s.addremove = false;
		s.tab('general', _('基本设置'));
		s.tab('serial', _('键鼠（串口）'));
		s.tab('video', _('视频'));
		s.tab('macro', _('开机宏'));

		/* ---- 基本设置 ---- */
		o = s.taboption('general', form.Flag, 'enabled', _('启用'));
		o.default = o.enabled; o.rmempty = false;

		o = s.taboption('general', form.Value, 'listen_port', _('管理页端口'),
			_('KVM 控制页、REST API 与 WebSocket 共用此端口。默认 18080，避开 8080 等常用端口；若被别的程序占用，服务不会强行启动，会在系统日志里写明占用者。'));
		o.datatype = 'port'; o.default = '18080'; o.rmempty = false;

		o = s.taboption('general', form.ListValue, 'bind', _('监听接口'),
			_('服务只绑定内网地址，不提供 0.0.0.0。视频流（ustreamer）使用同一个地址。WAN 防火墙请保持默认拒绝。'));
		o.value('br-lan', _('br-lan（LAN 口 IP，推荐）'));
		o.value('127.0.0.1', _('127.0.0.1（仅本机，配合反向代理）'));
		o.default = 'br-lan';

		o = s.taboption('general', form.Value, 'token', _('访问 Token'),
			_('开机宏、配置等接口都需要此 Token。16 到 64 位，只允许字母、数字、- 和 _。'));
		o.password = true; o.rmempty = false;
		o.validate = function(section_id, value) {
			return /^[A-Za-z0-9_-]{16,64}$/.test(value) ? true : _('Token 需为 16 到 64 位的字母、数字、- 或 _');
		};

		o = s.taboption('general', form.Button, '_gentoken', _('重新生成 Token'));
		o.inputtitle = _('生成');
		o.inputstyle = 'action';
		o.onclick = function(ev, section_id) {
			var opt = this.map.lookupOption('token', section_id);
			var ui_el = opt && opt[0] && opt[0].getUIElement(section_id);
			if (ui_el) { ui_el.setValue(genToken()); notify(_('已生成新 Token，保存并应用后生效。')); }
		};

		o = s.taboption('general', form.Value, 'nas_ip', _('NAS IP（可选）'),
			_('用于页面上的 Ping 状态检测；留空则显示「未配置」。'));
		o.datatype = 'ip4addr'; o.optional = true;

		/* ---- 串口 ---- */
		o = s.taboption('serial', form.Value, 'serial', _('串口设备'),
			_('留空则自动查找：只会打开 USB ID 在下方「自动探测的 USB ID」里的串口（CH340G 通常是 /dev/ttyUSB0），其它串口设备（4G 模块、UPS 等）一律不碰。手动指定后不再检查 USB ID。'));
		o.value('', _('自动查找'));
		ttys.forEach(function(t) { o.value(t, t); });
		o.optional = true;

		o = s.taboption('serial', form.Button, '_serialscan', _('扫描串口设备'),
			_('列出光猫上当前的 USB 串口及其 USB ID，可一键把设备加入下方白名单（之后还需点「保存并应用」）。适配器不是 CH340（例如走 CDC-ACM 的串口模块）时用这个，不用手敲 vid:pid。'));
		o.inputtitle = _('扫描');
		o.inputstyle = 'action';
		o.onclick = function(ev, section_id) {
			var map = this.map;
			return ctl(['serial-scan'], false).then(function(out) {
				var devs = String(out || '').split('\n').map(function(l) {
					var m = l.match(/^(\S+)\s+(\S+)\s+matched=(yes|no)/);
					return m ? { path: m[1], id: m[2], matched: m[3] === 'yes' } : null;
				}).filter(Boolean);
				if (!devs.length) {
					notify(_('没有发现 USB 串口设备（/dev/ttyUSB*、/dev/ttyACM*）。请检查接线，以及固件是否包含 kmod-usb-serial-ch341 / kmod-usb-acm。'), 'danger');
					return;
				}
				var opt = map.lookupOption('serial_ids', section_id);
				var el = opt && opt[0] && opt[0].getUIElement(section_id);
				function addId(id) {
					var list = String((el && el.getValue && el.getValue()) || '').split(',').filter(Boolean);
					if (list.map(function(x) { return x.toLowerCase(); }).indexOf(id) < 0) list.push(id);
					if (el) el.setValue(list.join(','));
					notify(_('已把 %s 加入白名单，点「保存并应用」后生效。').format(id));
					ui.hideModal();
				}
				var rows = devs.map(function(d) {
					var can = d.id !== 'unknown' && !d.matched;
					return E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td' }, d.path),
						E('td', { 'class': 'td' }, d.id),
						E('td', { 'class': 'td' }, d.matched ? _('已在白名单') : _('不在白名单')),
						E('td', { 'class': 'td' }, can
							? E('button', { 'class': 'cbi-button cbi-button-action', 'click': function() { addId(d.id); } }, _('加入白名单'))
							: '')
					]);
				});
				ui.showModal(_('检测到的串口设备'), [
					E('table', { 'class': 'table' }, [
						E('tr', { 'class': 'tr table-titles' }, [
							E('th', { 'class': 'th' }, _('设备')), E('th', { 'class': 'th' }, 'USB ID'),
							E('th', { 'class': 'th' }, _('状态')), E('th', { 'class': 'th' }, '')
						])
					].concat(rows)),
					E('p', { 'class': 'cbi-section-descr' },
						_('提示：通用 CDC 串口（例如 xxxx:5740）可能与其它 CDC 设备（4G 模块等）撞 ID；光猫上还接有别的 USB 串口时，建议改用「串口设备」直接指定路径。')),
					E('div', { 'class': 'right' }, E('button', { 'class': 'cbi-button', 'click': ui.hideModal }, _('关闭')))
				]);
			}).catch(function(e) { notify(_('扫描失败：') + e.message, 'danger'); });
		};

		o = s.taboption('serial', form.Value, 'serial_ids', _('自动探测的 USB ID'),
			_('逗号分隔的 vid:pid，默认 1a86:7523（CH340G）和 1a86:5523（CH341 串口模式）。串口适配器不是 CH340 时，点上面的「扫描」一键加入；也可在光猫上执行 nas-kvm-ctl serial-scan 查看。'));
		o.default = '1a86:7523,1a86:5523'; o.rmempty = false;
		o.validate = function(section_id, value) {
			return /^[0-9A-Fa-f]{4}:[0-9A-Fa-f]{4}(,[0-9A-Fa-f]{4}:[0-9A-Fa-f]{4})*$/.test(value) ? true : _('格式应为 vid:pid，多个用逗号分隔，例如 1a86:7523,1a86:5523');
		};

		o = s.taboption('serial', form.ListValue, 'baud', _('波特率'),
			_('以成品线实际为准（CH9329 出厂默认 9600）。9600 下绝对鼠标 60Hz 会占满约 80% 带宽，守护进程会自动降低鼠标发送频率；成品线支持时建议提高。'));
		['9600', '19200', '38400', '57600', '115200'].forEach(function(b) { o.value(b); });
		o.default = '9600';

		o = s.taboption('serial', form.Button, '_serialtest', _('链路测试'),
			_('发送「获取芯片信息」帧，读取应答并判断 NAS 的 USB 是否已枚举。需要服务正在运行。'));
		o.inputtitle = _('发送测试帧');
		o.inputstyle = 'action';
		o.onclick = function() {
			return ctl(['serial-test'], true).then(function(r) {
				if (r && r.ok)
					notify(_('应答正常，芯片版本 %s，NAS USB %s。').format(r.version, r.usb ? _('已枚举') : _('未枚举')));
				else
					notify(_('串口无应答：') + ((r && r.error) || _('服务未运行')), 'danger');
			}).catch(function(e) { notify(_('测试失败：') + e.message, 'danger'); });
		};

		/* ---- 视频 ---- */
		o = s.taboption('video', form.Value, 'video_device', _('视频设备'), _('MS2130 在 USB3 口上的 UVC 节点。'));
		o.default = '/dev/video0'; o.rmempty = false;

		o = s.taboption('video', form.ListValue, 'resolution', _('分辨率'),
			_('MS2130 硬件输出 MJPEG，光猫只转发不转码。带宽或供电不稳时降到 720p。'));
		o.value('1920x1080', '1920×1080'); o.value('1280x720', '1280×720'); o.default = '1920x1080';

		o = s.taboption('video', form.Value, 'desired_fps', _('目标帧率'));
		o.datatype = 'range(5,60)'; o.default = '30'; o.rmempty = false;

		o = s.taboption('video', form.Value, 'video_port', _('视频流端口'),
			_('ustreamer 监听端口，不能与管理页端口相同。'));
		o.datatype = 'port'; o.default = '18081'; o.rmempty = false;
		o.validate = function(section_id, value) {
			var p = this.map.lookupOption('listen_port', section_id);
			var pv = p && p[0] && p[0].formvalue(section_id);
			return (pv && pv === value) ? _('不能与管理页端口相同') : true;
		};

		/* ---- 开机宏 ---- */
		var KEYS = ['F1','F2','F3','F4','F5','F6','F7','F8','F9','F10','F11','F12','Esc','Del','Tab'];
		o = s.taboption('macro', form.ListValue, 'boot_menu_key', _('启动菜单键'), _('常见为 F11 或 F12，视主板而定。'));
		KEYS.forEach(function(k) { o.value(k); }); o.default = 'F11';

		o = s.taboption('macro', form.ListValue, 'bios_key', _('BIOS 设置键'));
		KEYS.forEach(function(k) { o.value(k); }); o.default = 'Del';

		o = s.taboption('macro', form.Value, 'macro_duration_s', _('持续时间（秒）'), _('点击「连发」后持续发键的时间；请在这段时间内给 NAS 开机。'));
		o.datatype = 'range(1,60)'; o.default = '10'; o.rmempty = false;

		o = s.taboption('macro', form.Value, 'macro_interval_ms', _('发送间隔（ms）'));
		o.datatype = 'range(20,1000)'; o.default = '100'; o.rmempty = false;

		o = s.taboption('macro', form.Value, 'key_hold_ms', _('按键保持时间（ms）'),
			_('每次连发为「按下帧 → 保持 → 释放帧」；波特率较高时保持时间过短，BIOS 的 USB 轮询可能漏键。'));
		o.datatype = 'range(10,200)'; o.default = '30'; o.rmempty = false;

		/* ---- 服务状态 + 控制 ---- */
		var statusBox = E('div', { 'id': 'nas-kvm-status' });
		function refresh() {
			return L.resolveDefault(callServiceList('nas-kvm'), {}).then(function(svc) {
				statusBox.innerHTML = '';
				statusBox.appendChild(self.renderStatus(svc));
			});
		}
		function svcBtn(label, action, style) {
			return E('button', {
				'class': 'cbi-button cbi-button-' + style,
				'click': ui.createHandlerFn(self, function() {
					return fs.exec(INIT, [ action ]).then(function() { return new Promise(function(r) { window.setTimeout(r, 800); }); }).then(refresh);
				})
			}, label);
		}
		var link = E('a', {
			'href': '#', 'target': '_blank', 'rel': 'noopener',
			'click': function(ev) {
				var port = uci.get('nas-kvm', 'main', 'listen_port') || '18080';
				ev.currentTarget.href = 'http://' + window.location.hostname + ':' + port + '/';
			}
		}, _('打开 KVM 控制页'));

		return m.render().then(function(formNode) {
			poll.add(refresh, 5);
			return E('div', {}, [
				E('div', { 'class': 'cbi-section' }, [
					E('h3', {}, _('服务状态')), statusBox,
					E('div', { 'class': 'cbi-section-node', 'style': 'margin:8px 0' }, [
						svcBtn(_('启动'), 'start', 'action'), ' ',
						svcBtn(_('停止'), 'stop', 'reset'), ' ',
						svcBtn(_('重启'), 'restart', 'apply'), ' ', link
					])
				]),
				formNode
			]);
		});
	}
});
