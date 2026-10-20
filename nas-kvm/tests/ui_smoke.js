// 用 jsdom 在真实 kvmd 上运行 www/kvm/index.html，验证前端与后端协议一致。
const { JSDOM, VirtualConsole } = require(process.env.JSDOM_PATH || 'jsdom');
const PORT = process.env.PORT, TOKEN = process.env.TOKEN;
const sleep = ms => new Promise(r => setTimeout(r, ms));
(async () => {
  const errors = [];
  const vc = new VirtualConsole();
  vc.on('jsdomError', e => errors.push(String(e.message || e)));
  const html = await (await fetch(`http://127.0.0.1:${PORT}/`)).text();
  const dom = new JSDOM(html, {
    url: `http://127.0.0.1:${PORT}/#token=${TOKEN}`, runScripts: 'dangerously', pretendToBeVisual: true, virtualConsole: vc,
    beforeParse(w) { w.fetch = (u, o) => fetch(new URL(u, w.location.href), o); },
  });
  const w = dom.window, d = w.document, $ = s => d.querySelector(s);
  const out = {};
  await sleep(2500);
  out.wsChip = $('#cWs').textContent; out.serialChip = $('#cSerial').textContent; out.usbChip = $('#cUsb').textContent;
  out.hdmiChip = $('#cHdmi').textContent; out.loginVisible = $('#login').classList.contains('show');
  out.imgSrc = $('#img').getAttribute('src');
  out.cfgFilled = [$('#kBoot').value, $('#kBios').value, $('#mDur').value, $('#mInt').value];
  out.hashCleared = w.location.hash === '';

  const scr = $('#screen');
  scr.getBoundingClientRect = () => ({ left: 0, top: 0, width: 1600, height: 900, right: 1600, bottom: 900 });
  scr.focus(); scr.dispatchEvent(new w.Event('focus'));
  const K = (t, code) => scr.dispatchEvent(new w.KeyboardEvent(t, { code, bubbles: true, cancelable: true }));
  K('keydown', 'KeyA'); await sleep(80); K('keyup', 'KeyA'); await sleep(80);
  K('keydown', 'ControlLeft'); K('keydown', 'KeyC'); await sleep(80); K('keyup', 'KeyC'); K('keyup', 'ControlLeft'); await sleep(80);
  const M = (t, x, y, b = 0) => scr.dispatchEvent(new w.MouseEvent(t, { clientX: x, clientY: y, button: b, bubbles: true, cancelable: true }));
  M('pointermove', 800, 450); await sleep(80);
  M('pointerdown', 1600, 900, 0); await sleep(80); M('pointerup', 1600, 900, 0); await sleep(80);
  out.mpos = $('#mpos').textContent;
  $('#bCad').click(); await sleep(250);

  // 电源功能已移除：页面里不应再有任何电源控件，宏按钮在空闲时可点
  out.noPower = !['#bPress', '#bForce', '#bCycle', '#pwState', '#cf', '#mPre', '[data-t="power"]'].some(s => $(s));
  out.macroBtns = ['#bMenu', '#bBios'].map(s => $(s).disabled);

  // 宏设置保存 → 测试键
  $('#kBoot').value = 'F12'; $('#bMacroSave').click(); await sleep(300);
  $('#bTestM').click(); await sleep(400);
  out.mState = $('#mState').textContent;
  $('#bRel').click(); await sleep(100);
  scr.dispatchEvent(new w.Event('blur')); await sleep(100);

  // 错误 token → 登录框
  const dom2 = new JSDOM(html, { url: `http://127.0.0.1:${PORT}/#token=wrongwrongwrong`, runScripts: 'dangerously', pretendToBeVisual: true, virtualConsole: vc,
    beforeParse(w2) { w2.fetch = (u, o) => fetch(new URL(u, w2.location.href), o); } });
  await sleep(2000);
  out.badTokenLogin = dom2.window.document.querySelector('#login').classList.contains('show');
  out.errors = errors;
  console.log(JSON.stringify(out));
  dom.window.close(); dom2.window.close(); process.exit(0);
})().catch(e => { console.error('UI test crashed:', e); process.exit(1); });
