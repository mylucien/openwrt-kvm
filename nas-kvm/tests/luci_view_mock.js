// 在最小 LuCI mock 里执行 view/nas-kvm/config.js，检查没有运行时错误，并核对注册的选项集合。
const fs_ = require('fs'), path = process.argv[2];
const src = fs_.readFileSync(path, 'utf8');
const opts = {}; let sectionTabs = [];
String.prototype.format = function() { let a = arguments, i = 0; return this.replace(/%s/g, () => a[i++]); };
const optProto = { value() {}, validate: null };
const sec = {
  tab(id) { sectionTabs.push(id); },
  taboption(tab, type, name) { const o = Object.assign({ tab, type, name, values: [] }, optProto, { value(v) { this.values.push(v); }, formvalue() { return '18080'; } }); opts[name] = o; return o; },
};
const form = { Map: class { constructor(c) { this.c = c; } section() { return sec; } render() { return Promise.resolve({ node: 'form' }); } lookupOption() { return [{ getUIElement: () => ({ setValue: v => { sec.lastToken = v; } }), formvalue: () => '18080' }]; } },
  NamedSection: class {}, Flag: 'Flag', Value: 'Value', ListValue: 'ListValue', Button: 'Button' };
const calls = [];
const g = {
  view: { extend: o => o }, form,
  fs: { list: async p => [{ name: 'ttyUSB0' }, { name: 'tty0' }, { name: 'video0' }],
        exec: async (c, a) => { calls.push(['exec', c, ...a]); return {}; },
        exec_direct: async (c, a) => { calls.push(['direct', c, ...a]); if (a[0] === 'serial-scan') return '/dev/ttyACM0 0843:5740 matched=no\n/dev/ttyUSB0 1a86:7523 matched=yes\n'; return a[0] === 'serial-test' ? { ok: true, version: '0x30', usb: true } : { ok: true }; } },
  ui: { addNotification: (t, n, c) => calls.push(['notify', c]), createHandlerFn: (s, f) => f, showModal: (t, ch) => { g.modal = ch; calls.push(['modal', t]); }, hideModal: () => calls.push(['hideModal']) },
  uci: { load: async () => {}, get: () => '18080' },
  rpc: { declare: () => async () => ({ 'nas-kvm': { instances: { kvmd: { running: true, pid: 1 }, ustreamer: { running: false } } } }) },
  poll: { add: (fn, s) => calls.push(['poll', s]) },
  E: (tag, attrs, children) => ({ tag, attrs, children }),
  _: s => s, L: { resolveDefault: p => Promise.resolve(p).catch(() => ({})), bind: f => f },
  window: { crypto: require('crypto').webcrypto, confirm: () => true, location: { hostname: 'x' }, setTimeout },
};
const names = Object.keys(g);
// 去掉 'require xxx' 指令语句，在 mock 作用域内执行并取回 view 对象
const body = src.replace(/^'require [^']+';$/mg, '').replace(/^'use strict';$/m, '');
const view = new Function(...names, body)(...names.map(n => g[n]));
(async () => {
  const data = await view.load.call(view).then(r => r); // load 里的 fs.list 已 mock
  const node = await view.render.call(view, [ await g.fs.list('/dev') ]);
  const want = ['enabled','listen_port','bind','token','_gentoken','nas_ip','serial','_serialscan','serial_ids','baud','_serialtest','video_device','resolution','desired_fps','video_port',
    'boot_menu_key','bios_key','macro_duration_s','macro_interval_ms','key_hold_ms'];
  const missing = want.filter(k => !opts[k]);
  const stale = ['power_led','press_ms','force_ms','cycle_wait_s','min_interval_s','_powertest','pre_press'].filter(k => opts[k]);
  console.log('removed power options absent:', stale.length ? 'NO: ' + stale.join(',') : 'yes', '| power tab absent:', !sectionTabs.includes('power'));
  console.log('tabs:', sectionTabs.join(','));
  console.log('options registered:', Object.keys(opts).length, '| missing:', missing.join(',') || 'none');
  console.log('serial choices:', JSON.stringify(opts.serial.values));
  console.log('token validate:', opts.token.validate(null, 'short'), '/', opts.token.validate(null, 'a1b2c3d4e5f6a7b8c9d0') );
  console.log('serial_ids validate:', opts.serial_ids.validate(null, '1a86:7523,1A86:5523'), '/', opts.serial_ids.validate(null, '1a86:7523;x'), '/', opts.serial_ids.validate(null, ''));
  console.log('video_port vs listen_port clash:', opts.video_port.validate.call({ map: new form.Map() }, 's', '18080'));
  await opts._serialtest.onclick();
  await opts._serialscan.onclick.call({ map: new form.Map() }, null, 's');
  const flat = JSON.stringify(g.modal);
  const btns = []; (function walk(n) { if (!n) return; if (Array.isArray(n)) return n.forEach(walk);
    if (typeof n === 'object') { if (n.tag === 'button' && n.attrs && n.attrs.click && n.children === '加入白名单') btns.push(n); walk(n.children); } })(g.modal);
  console.log('scan modal lists both devices:', flat.includes('/dev/ttyACM0') && flat.includes('/dev/ttyUSB0'));
  console.log('bind buttons (only the unmatched device):', btns.length);
  if (btns.length === 1) { btns[0].attrs.click(); console.log('bind adds 0843:5740 to serial_ids:', sec.lastToken === '0843:5740'); }
  opts._gentoken.onclick.call({ map: new form.Map() }, null, 's');
  console.log('generated token:', /^[0-9a-f]{32}$/.test(sec.lastToken));
  console.log('calls:', JSON.stringify(calls));
  process.exit(missing.length || stale.length ? 1 : 0);
})().catch(e => { console.error('MOCK RUN FAILED:', e); process.exit(1); });
