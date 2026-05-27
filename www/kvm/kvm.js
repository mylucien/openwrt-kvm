/**
 * kvm.js — IP-KVM 前端控制
 *
 * 职责：
 *   1. WebSocket 连接 kvm_manager (:8765)
 *   2. 显示 mjpg_streamer 视频流
 *   3. 鼠标捕获（rAF 60Hz 节流）→ 二进制 HID 帧
 *   4. 键盘捕获（含 Ctrl+Alt+Del 等组合键）
 */

/* ── 配置 ─────────────────────────────────────────── */
const WS_URL        = `ws://${location.hostname}:8765`;
const MJPG_BASE     = `http://${location.hostname}:8080`;
const PING_INTERVAL = 5000;   /* ms */
const MOUSE_SCALE   = 4095;   /* CH9329 坐标范围 */

/* 构建 MJPEG URL（含质量参数，部分 mjpg_streamer 构建支持） */
function streamUrl() {
    const q = document.getElementById('quality-select').value;
    return `${MJPG_BASE}/?action=stream&quality=${q}`;
}

/* ── HID 命令常量 ────────────────────────────────── */
const HID_MOUSE = 0x02;
const HID_KEY   = 0x01;

/* ── DOM 元素 ────────────────────────────────────── */
const img         = document.getElementById('kvm-screen');
const canvas      = document.getElementById('mouse-canvas');
const statusInd   = document.getElementById('status-indicator');
const statusText  = document.getElementById('status-text');
const fpsDisplay  = document.getElementById('fps-display');
const connInfo    = document.getElementById('connection-info');
const qualitySel  = document.getElementById('quality-select');
const osd         = document.getElementById('osd');

let ws            = null;
let connected     = false;
let streamReady   = false;
let reconnectTimer = null;

/* ── 鼠标状态 ────────────────────────────────────── */
let pendingMouse  = null;
let lastMousePos  = { x: 2048, y: 2048 };  /* 记录上次鼠标位置，供纯按键事件使用 */
let rafScheduled  = false;
let currentBtn    = 0;   /* 鼠标按键位掩码 */

/* ── 键盘状态 ────── Set<string>, code 如 "KeyA", "ControlLeft" 等 */
const keysDown    = new Set();

/* ── 性能统计 ────────────────────────────────────── */
let frameCount    = 0;

/* ════════════════════════════════════════════════════
   WebSocket
   ════════════════════════════════════════════════════ */

function connectWS() {
    setStatus('connecting', '正在连接...');

    if (ws) {
        ws.onclose = null;
        ws.close();
    }

    ws = new WebSocket(WS_URL);

    ws.onopen = () => {
        connected = true;
        setStatus('connecting', '等待视频流...');
        if (reconnectTimer) {
            clearTimeout(reconnectTimer);
            reconnectTimer = null;
        }
    };

    ws.onmessage = (e) => {
        if (typeof e.data === 'string') {
            try {
                const msg = JSON.parse(e.data);
                if (msg.type === 'stream_ready') {
                    streamReady = true;
                    setStatus('online', '已连接');
                    startVideo();
                    connInfo.textContent = `ws://${location.hostname}:8765 | mjpg://${location.hostname}:${msg.port}`;
                }
            } catch (_) { /* ignore */ }
        } else if (e.data instanceof Blob) {
            /* 二进制帧（预留：未来可能接收服务端消息） */
        }
    };

    ws.onclose = () => {
        connected = false;
        streamReady = false;
        setStatus('offline', '已断开');
        stopVideo();
        /* 确保旧连接完全关闭后再重连 */
        if (reconnectTimer) clearTimeout(reconnectTimer);
        reconnectTimer = setTimeout(() => {
            reconnectTimer = null;
            connectWS();
        }, 3000);
    };

    ws.onerror = () => {
        /* onclose 会随后触发 */
    };
}

/* scheduleReconnect 已内联到 ws.onclose */


/* ── 心跳 ────────────────────────────────────────── */
setInterval(() => {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send('ping');
    }
}, PING_INTERVAL);

/* ════════════════════════════════════════════════════
   视频流
   ════════════════════════════════════════════════════ */

function startVideo() {
    /* 添加时间戳防止浏览器缓存旧的 MJPEG 流 */
    img.src = streamUrl() + '&_t=' + Date.now();
    img.onload = () => {
        setStatus('online', '已连接');
    };
}

function stopVideo() {
    img.src = '';
}

function refreshVideo() {
    if (streamReady) {
        img.src = streamUrl() + '&_t=' + Date.now();
    }
}

/* ── 质量切换（刷新时使用选中的 quality 参数） ───── */
qualitySel.addEventListener('change', () => {
    refreshVideo();
});

/* ════════════════════════════════════════════════════
   鼠标控制
   ════════════════════════════════════════════════════ */

/* 鼠标移动 — rAF 节流到 ~60Hz */
img.addEventListener('mousemove', (e) => {
    if (!connected || !streamReady) return;

    const rect = img.getBoundingClientRect();
    pendingMouse = {
        x: Math.round(((e.clientX - rect.left) / rect.width)  * MOUSE_SCALE),
        y: Math.round(((e.clientY - rect.top)  / rect.height) * MOUSE_SCALE),
    };
    /* 限制在有效范围内 */
    pendingMouse.x = Math.max(0, Math.min(MOUSE_SCALE, pendingMouse.x));
    pendingMouse.y = Math.max(0, Math.min(MOUSE_SCALE, pendingMouse.y));
    lastMousePos = { x: pendingMouse.x, y: pendingMouse.y };

    if (!rafScheduled) {
        rafScheduled = true;
        requestAnimationFrame(flushMouse);
    }
});

/* CH9329 按键位掩码: bit0=左键, bit1=右键, bit2=中键
 * 浏览器 e.button: 0=左, 1=中, 2=右 → 需要映射 */
const BTN_MAP = [1, 4, 2];  /* 索引: e.button, 值: CH9329 位掩码 */

img.addEventListener('mousedown', (e) => {
    if (!connected || !streamReady) return;
    e.preventDefault();
    currentBtn |= BTN_MAP[e.button] || 0;
    sendMouse();
});

img.addEventListener('mouseup', (e) => {
    if (!connected || !streamReady) return;
    e.preventDefault();
    currentBtn &= ~(BTN_MAP[e.button] || 0);
    sendMouse();
});

/* 阻止右键菜单（mouseup 无法阻止独立 contextmenu 事件） */
img.addEventListener('contextmenu', (e) => e.preventDefault());

/* 滚轮 → 发送上下滚轮事件 */
img.addEventListener('wheel', (e) => {
    if (!connected || !streamReady || !ws || ws.readyState !== WebSocket.OPEN) return;
    e.preventDefault();
    const delta = Math.sign(e.deltaY);  /* -1=上, 1=下 */
    /* 用鼠标帧 + 滚轮标记：type=0x02, btn=滚轮位(0x10/0x20), 模拟滚动 */
    const buf = new ArrayBuffer(6);
    const view = new DataView(buf);
    view.setUint8(0, HID_MOUSE);
    view.setUint8(1, delta < 0 ? 0x10 : 0x20);  /* 滚轮上/下 */
    view.setUint16(2, lastMousePos.x, true);
    view.setUint16(4, lastMousePos.y, true);
    ws.send(buf);
}, { passive: false });

function flushMouse() {
    rafScheduled = false;
    if (!pendingMouse || !ws || ws.readyState !== WebSocket.OPEN) return;
    sendMouseFrame(pendingMouse.x, pendingMouse.y, currentBtn);
    pendingMouse = null;
}

function sendMouse() {
    if (!pendingMouse) {
        /* 只有按键事件，使用上次记录的位置而非跳回屏幕中心 */
        pendingMouse = { x: lastMousePos.x, y: lastMousePos.y };
    }
    if (!rafScheduled) {
        rafScheduled = true;
        requestAnimationFrame(flushMouse);
    }
}

function sendMouseFrame(x, y, btn) {
    const buf = new ArrayBuffer(6);
    const view = new DataView(buf);
    view.setUint8(0, HID_MOUSE);
    view.setUint8(1, btn);
    view.setUint16(2, x, true);   /* LE */
    view.setUint16(4, y, true);   /* LE */
    ws.send(buf);
}

/* ════════════════════════════════════════════════════
   键盘控制
   ════════════════════════════════════════════════════ */

document.addEventListener('keydown', (e) => {
    if (!connected || !streamReady) return;

    /* 避免重复发送（按住重复） */
    if (keysDown.has(e.code)) return;
    keysDown.add(e.code);

    /* KVM 场景：阻止所有浏览器默认行为（包括方向键滚动、Tab 跳转等） */
    e.preventDefault();

    const keyCode = keyCodeMap[e.code];
    if (keyCode === undefined) return;

    const modifiers = buildModifiers(e);
    sendKeyFrame(keyCode, modifiers, true);
});

document.addEventListener('keyup', (e) => {
    if (!connected || !streamReady) return;

    keysDown.delete(e.code);

    const keyCode = keyCodeMap[e.code];
    if (keyCode === undefined) return;

    const modifiers = buildModifiers(e);
    sendKeyFrame(keyCode, modifiers, false);
});

function buildModifiers(e) {
    /* e.ctrlKey/e.shiftKey/e.altKey/e.metaKey 已经准确反映修饰键状态，
     * 不需要再检查 e.code（e.code 对应的按键会由 sendKeyFrame 单独处理） */
    let mod = 0;
    if (e.ctrlKey)  mod |= 0x01;
    if (e.shiftKey) mod |= 0x02;
    if (e.altKey)   mod |= 0x04;
    if (e.metaKey)  mod |= 0x08;
    return mod;
}

function sendKeyFrame(keyCode, modifiers, pressed) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;

    const buf = new ArrayBuffer(8);
    const view = new DataView(buf);
    view.setUint8(0, HID_KEY);
    view.setUint8(1, modifiers);
    view.setUint8(2, pressed ? 1 : 0);    /* 1=按下, 0=释放 */
    view.setUint8(3, keyCode);             /* 按键码 */
    /* bytes 4-7 保留用于多键同时按下 */
    view.setUint32(4, 0, true);
    ws.send(buf);
}

/* ── 键盘映射（USB HID Usage ID） ───────────────── */
const keyCodeMap = {
    /* 字母 */
    'KeyA': 0x04, 'KeyB': 0x05, 'KeyC': 0x06, 'KeyD': 0x07,
    'KeyE': 0x08, 'KeyF': 0x09, 'KeyG': 0x0A, 'KeyH': 0x0B,
    'KeyI': 0x0C, 'KeyJ': 0x0D, 'KeyK': 0x0E, 'KeyL': 0x0F,
    'KeyM': 0x10, 'KeyN': 0x11, 'KeyO': 0x12, 'KeyP': 0x13,
    'KeyQ': 0x14, 'KeyR': 0x15, 'KeyS': 0x16, 'KeyT': 0x17,
    'KeyU': 0x18, 'KeyV': 0x19, 'KeyW': 0x1A, 'KeyX': 0x1B,
    'KeyY': 0x1C, 'KeyZ': 0x1D,

    /* 数字 */
    'Digit1': 0x1E, 'Digit2': 0x1F, 'Digit3': 0x20,
    'Digit4': 0x21, 'Digit5': 0x22, 'Digit6': 0x23,
    'Digit7': 0x24, 'Digit8': 0x25, 'Digit9': 0x26, 'Digit0': 0x27,

    /* 功能键 */
    'Enter': 0x28, 'Escape': 0x29, 'Backspace': 0x2A, 'Tab': 0x2B,
    'Space': 0x2C, 'Minus': 0x2D, 'Equal': 0x2E,
    'BracketLeft': 0x2F, 'BracketRight': 0x30,
    'Backslash': 0x31, 'Semicolon': 0x33,
    'Quote': 0x34, 'Backquote': 0x35,
    'Comma': 0x36, 'Period': 0x37, 'Slash': 0x38,
    'CapsLock': 0x39,

    'F1': 0x3A, 'F2': 0x3B, 'F3': 0x3C, 'F4': 0x3D,
    'F5': 0x3E, 'F6': 0x3F, 'F7': 0x40, 'F8': 0x41,
    'F9': 0x42, 'F10': 0x43, 'F11': 0x44, 'F12': 0x45,

    'PrintScreen': 0x46, 'ScrollLock': 0x47, 'Pause': 0x48,
    'Insert': 0x49, 'Home': 0x4A, 'PageUp': 0x4B,
    'Delete': 0x4C, 'End': 0x4D, 'PageDown': 0x4E,
    'ArrowRight': 0x4F, 'ArrowLeft': 0x50,
    'ArrowDown': 0x51, 'ArrowUp': 0x52,

    'NumLock': 0x53,
    'NumpadDivide': 0x54, 'NumpadMultiply': 0x55,
    'NumpadSubtract': 0x56, 'NumpadAdd': 0x57,
    'NumpadEnter': 0x58,
    'Numpad1': 0x59, 'Numpad2': 0x5A, 'Numpad3': 0x5B,
    'Numpad4': 0x5C, 'Numpad5': 0x5D, 'Numpad6': 0x5E,
    'Numpad7': 0x5F, 'Numpad8': 0x60, 'Numpad9': 0x61,
    'Numpad0': 0x62, 'NumpadDecimal': 0x63,

    'ControlLeft': 0xE0, 'ShiftLeft': 0xE1,
    'AltLeft': 0xE2, 'MetaLeft': 0xE3,
    'ControlRight': 0xE4, 'ShiftRight': 0xE5,
    'AltRight': 0xE6, 'MetaRight': 0xE7,
};

/* ════════════════════════════════════════════════════
   UI 辅助函数
   ════════════════════════════════════════════════════ */

function setStatus(state, text) {
    statusInd.className = state;
    statusText.textContent = text;
}

function showOSD(msg, duration) {
    osd.textContent = msg;
    osd.classList.add('visible');
    clearTimeout(osd._timer);
    osd._timer = setTimeout(() => osd.classList.remove('visible'), duration || 1500);
}

/* ── 全屏 ────────────────────────────────────────── */
document.getElementById('btn-fullscreen').addEventListener('click', () => {
    if (document.fullscreenElement) {
        document.exitFullscreen();
    } else {
        document.getElementById('container').requestFullscreen();
    }
});

document.addEventListener('fullscreenchange', () => {
    const btn = document.getElementById('btn-fullscreen');
    btn.textContent = document.fullscreenElement ? '⛶ 退出全屏' : '⛶ 全屏';
});

/* ── 刷新 ────────────────────────────────────────── */
document.getElementById('btn-refresh').addEventListener('click', refreshVideo);

/* ════════════════════════════════════════════════════
   连接状态 FPS 统计
   ════════════════════════════════════════════════════ */

/* 统计视频帧率（MJPEG 流不触发 img.onload，改用 requestAnimationFrame 帧计数器） */
let lastFrameCheck = performance.now();
function fpsCounter() {
    frameCount++;
    const now = performance.now();
    if (now - lastFrameCheck >= 1000) {
        fpsDisplay.textContent = `${frameCount} FPS`;
        frameCount = 0;
        lastFrameCheck = now;
    }
    requestAnimationFrame(fpsCounter);
}
requestAnimationFrame(fpsCounter);

/* ════════════════════════════════════════════════════
   页面失焦处理 — 清空键盘状态，防止按键残留
   ════════════════════════════════════════════════════ */
window.addEventListener('blur', () => {
    if (keysDown.size > 0) {
        keysDown.clear();
        /* 发送空键盘帧通知服务端释放所有按键 */
        if (ws && ws.readyState === WebSocket.OPEN) {
            const buf = new ArrayBuffer(8);
            const view = new DataView(buf);
            view.setUint8(0, HID_KEY);    /* type */
            view.setUint8(1, 0);           /* modifiers */
            view.setUint8(2, 0);           /* pressed */
            view.setUint8(3, 0);           /* keyCode */
            view.setUint32(4, 0, true);    /* reserved */
            ws.send(buf);
        }
    }
});

/* ════════════════════════════════════════════════════
   启动
   ════════════════════════════════════════════════════ */

connectWS();
