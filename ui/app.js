const $ = (sel) => document.querySelector(sel);

const el = {
  statusDot: $('#statusDot'),
  statusText: $('#statusText'),
  ssidTitle: $('#ssidTitle'),
  methodText: $('#methodText'),
  powerBtn: $('#powerBtn'),
  powerLabel: $('#powerLabel'),
  restartBtn: $('#restartBtn'),
  themeBtn: $('#themeBtn'),
  adminBadge: $('#adminBadge'),
  dpBadge: $('#dpBadge'),
  kvSsid: $('#kvSsid'),
  kvPass: $('#kvPass'),
  kvBand: $('#kvBand'),
  kvUplink: $('#kvUplink'),
  devices: $('#devices'),
  devicesEmpty: $('#devicesEmpty'),
  deviceCount: $('#deviceCount'),
  blacklist: $('#blacklist'),
  blacklistEmpty: $('#blacklistEmpty'),
  blackCount: $('#blackCount'),
  rateDown: $('#rateDown'),
  rateUp: $('#rateUp'),
  chart: $('#chart'),
  log: $('#log'),
  tIpv6: $('#tIpv6'),
  tMcast: $('#tMcast'),
  tTtl: $('#tTtl'),
  tAutostart: $('#tAutostart'),
  tAutoHotspot: $('#tAutoHotspot'),
  autostartHint: $('#autostartHint'),
  adminBanner: $('#adminBanner'),
  elevateBtn: $('#elevateBtn'),
  ttlTag: $('#ttlTag'),
  totalMbps: $('#totalMbps'),
  hostPrio: $('#hostPrio'),
  policyHint: $('#policyHint'),
  toast: $('#toast'),
  netDialog: $('#netDialog'),
  deviceDialog: $('#deviceDialog'),
  dAlias: $('#dAlias'),
  dNote: $('#dNote'),
  dHint: $('#dHint'),
  doctorDialog: $('#doctorDialog'),
  doctorOut: $('#doctorOut'),
};

let status = null;
let busy = false;
let editingMac = null;

const BAND_LABEL = { auto: '自动', '2.4': '2.4 GHz', '5': '5 GHz' };
const PRIO_LABEL = { 1: 'P1 · 最高', 2: 'P2', 3: 'P3 · 默认', 4: 'P4', 5: 'P5 · 最低' };
const APP_VERSION = '3';

function showStaleBanner(show) {
  const b = document.getElementById('staleBanner');
  if (b) b.hidden = !show;
}

async function checkBackend() {
  try {
    const r = await fetch('/api/health', { cache: 'no-store' }).then((x) => x.json());
    const stale = r.version !== APP_VERSION;
    showStaleBanner(stale);
    return !stale;
  } catch {
    showStaleBanner(true);
    return false;
  }
}

function fmtRate(bps) {
  if (!bps || bps < 1) return '0 B/s';
  const units = ['B/s', 'KB/s', 'MB/s', 'GB/s'];
  let i = 0;
  let v = bps;
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
  return `${v >= 100 ? Math.round(v) : v.toFixed(1)} ${units[i]}`;
}

function fmtMbps(mbps) {
  return mbps ? `${mbps} Mbps` : '不限';
}

function toast(msg, isError = false) {
  el.toast.textContent = msg;
  el.toast.className = 'toast' + (isError ? ' err' : '');
  el.toast.hidden = false;
  clearTimeout(toast._t);
  toast._t = setTimeout(() => { el.toast.hidden = true; }, 3200);
}

async function api(path, options) {
  const res = await fetch(path, { headers: { 'Content-Type': 'application/json' }, ...options });
  const data = await res.json().catch(() => ({}));
  if (data && data.error === 'unknown endpoint') {
    showStaleBanner(true);
    toast('服务端还是旧版本，请重启「kwifi 服务」窗口', true);
  }
  return data;
}

// ---------------- devices ----------------

function createDeviceCard(c) {
  const card = document.createElement('div');
  card.className = 'device';
  card.dataset.mac = c.mac;
  card.innerHTML = `
    <div class="avatar js-avatar">·</div>
    <div class="device-main">
      <div class="device-name"><span class="js-name">—</span><span class="pill js-pill">在线</span></div>
      <div class="device-meta"><span class="js-kind">—</span><span class="js-ip"></span><span class="js-mac"></span><span class="js-note"></span><span class="js-rate"></span><span class="js-drops" hidden></span><button class="js-merge chip-link" hidden></button></div>
    </div>
    <div class="device-actions">
      <button class="icon-btn" data-act="edit" title="改名 / 备注">
        <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="1.8">
          <path d="M4 20h4l10-10-4-4L4 16v4z"/><path d="M13.5 6.5l4 4"/>
        </svg>
      </button>
      <button class="icon-btn" data-act="del" title="删除这条记录" hidden>
        <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="1.8">
          <path d="M4 7h16M9 7V5h6v2M6 7l1 13h10l1-13"/>
        </svg>
      </button>
      <button class="icon-btn danger" data-act="block" title="封禁 / 解封">
        <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="1.8">
          <circle cx="12" cy="12" r="9"/><path d="M6 6l12 12"/>
        </svg>
      </button>
    </div>
    <div class="limits">
      <div class="limit-row">
        <span class="limit-label">下载</span>
        <input type="checkbox" class="switch mini" data-toggle="down" title="开启/关闭下载限速" />
        <span class="num"><input type="number" min="0" step="0.5" inputmode="decimal" data-dir="down" placeholder="不限" /><em>Mbps</em></span>
      </div>
      <div class="limit-row">
        <span class="limit-label">上传</span>
        <input type="checkbox" class="switch mini" data-toggle="up" title="开启/关闭上传限速" />
        <span class="num"><input type="number" min="0" step="0.5" inputmode="decimal" data-dir="up" placeholder="不限" /><em>Mbps</em></span>
      </div>
      <label class="limit-row">
        <span class="limit-label">优先级</span>
        <select data-role="prio">
          <option value="1">P1 · 最高</option><option value="2">P2</option><option value="3">P3 · 默认</option>
          <option value="4">P4</option><option value="5">P5 · 最低</option>
        </select>
      </label>
    </div>`;

  card.querySelector('[data-act="edit"]').addEventListener('click', () => openDeviceDialog(card.dataset.mac));

  card.querySelector('[data-act="del"]').addEventListener('click', async () => {
    if (!confirm('删除这条设备记录？它的备注、限速、封禁状态会一起清掉。')) return;
    const r = await api('/api/device/delete', {
      method: 'POST',
      body: JSON.stringify({ mac: card.dataset.mac }),
    });
    if (!r.ok) toast(r.error || '删除失败', true);
    else {
      toast('已删除记录');
      card.remove();
    }
  });
  card.querySelector('[data-act="block"]').addEventListener('click', async () => {
    const cur = (status?.clients || []).find((x) => x.mac === card.dataset.mac);
    const r = await api('/api/device', {
      method: 'POST',
      body: JSON.stringify({ mac: card.dataset.mac, blocked: !cur?.blocked }),
    });
    if (!r.ok) {
      toast(r.error || '操作失败', true);
      pollNow();
    } else {
      toast(r.data.blocked ? '已封禁该设备' : '已解封');
      pollNow();
    }
  });

  const downInput = card.querySelector('[data-dir="down"]');
  const upInput = card.querySelector('[data-dir="up"]');
  const downToggle = card.querySelector('[data-toggle="down"]');
  const upToggle = card.querySelector('[data-toggle="up"]');

  const applyDir = async (dir, val) => {
    const body = { mac: card.dataset.mac };
    body[dir === 'down' ? 'downMbps' : 'upMbps'] = val;
    card.dataset.pendingUntil = String(Date.now() + 3000);
    const r = await api('/api/device', { method: 'POST', body: JSON.stringify(body) });
    if (!r.ok) {
      toast(r.error || '保存失败', true);
      return false;
    }
    const label = dir === 'down' ? '下载' : '上传';
    toast(val > 0 ? `${label}限速已开启：${fmtMbps(val)}` : `${label}限速已关闭`);
    if (r.warning) toast(r.warning, true);
    return true;
  };

  const wire = (dir, input, toggle) => {
    toggle.addEventListener('change', async () => {
      if (toggle.checked) {
        let v = Number(input.value) || 0;
        if (v <= 0) {
          v = Number(input.dataset.last) || 10;
          input.value = v;
        }
        input.dataset.last = v;
        const ok = await applyDir(dir, v);
        if (!ok) toggle.checked = false;
      } else {
        if (Number(input.value) > 0) input.dataset.last = input.value;
        const ok = await applyDir(dir, 0);
        if (!ok) toggle.checked = true;
      }
    });

    const commit = async () => {
      const v = Math.max(0, Number(input.value) || 0);
      if (v > 0) input.dataset.last = v;
      toggle.checked = v > 0;
      await applyDir(dir, v);
    };

    input.addEventListener('change', () => {
      if (toggle.checked || Number(input.value) > 0) commit();
    });
    input.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); commit(); }
    });
  };

  wire('down', downInput, downToggle);
  wire('up', upInput, upToggle);

  card.querySelector('[data-role="prio"]').addEventListener('change', async (e) => {
    const r = await api('/api/device', {
      method: 'POST',
      body: JSON.stringify({ mac: card.dataset.mac, prio: Number(e.target.value) }),
    });
    if (!r.ok) toast(r.error || '保存失败', true);
    else {
      toast(`优先级 ${PRIO_LABEL[e.target.value]}`);
      if (r.warning) toast(r.warning, true);
    }
  });

  el.devices.appendChild(card);
  return card;
}

function timeAgo(ts) {
  if (!ts) return '';
  const s = Math.max(0, Math.floor((Date.now() - ts) / 1000));
  if (s < 60) return '刚刚';
  if (s < 3600) return `${Math.floor(s / 60)} 分钟前`;
  if (s < 86400) return `${Math.floor(s / 3600)} 小时前`;
  return `${Math.floor(s / 86400)} 天前`;
}

function updateDeviceCard(card, c) {
  const offline = c.online === false;
  card.classList.toggle('blocked', c.blocked);
  card.classList.toggle('offline', offline);
  card.querySelector('.js-avatar').textContent = (c.name || '?').slice(0, 1).toUpperCase();
  card.querySelector('.js-name').textContent = c.name || '未知设备';
  card.querySelector('.js-kind').textContent =
    [c.kind, c.model && c.model !== c.name ? c.model : ''].filter(Boolean).join(' · ');
  card.querySelector('.js-ip').textContent = c.ip || '—';
  card.querySelector('.js-mac').textContent = c.mac.toUpperCase() + (c.randomMac ? ' (随机)' : '');

  const pill = card.querySelector('.js-pill');
  pill.textContent = offline ? (c.blocked ? '已封禁 · 离线' : '离线') : c.blocked ? '已封禁' : '在线';
  pill.className = 'pill js-pill ' + (c.blocked ? 'off' : offline ? 'muted' : 'on');
  card.querySelector('[data-act="block"]').classList.toggle('active', c.blocked);

  const note = card.querySelector('.js-note');
  note.textContent = c.note ? `备注：${c.note}` : '';

  // when blocking is active, show whether the data plane is actually dropping
  const dropEl = card.querySelector('.js-drops');
  if (dropEl) {
    const drops = Number(c.drops || 0);
    if (c.blocked) {
      dropEl.hidden = false;
      dropEl.textContent = drops > 0
        ? `已拦截 ${drops.toLocaleString()} 个包`
        : '尚未拦截到流量';
      dropEl.classList.toggle('warn-text', drops === 0);
    } else {
      dropEl.hidden = true;
    }
  }
  note.title = c.note || '';

  // a history entry can only be removed once the device is gone
  card.querySelector('[data-act="del"]').hidden = c.online !== false;

  // a randomised MAC usually belongs to a device we already know
  const mergeBtn = card.querySelector('.js-merge');
  const sibling = findSibling(c);
  if (c.randomMac && sibling) {
    mergeBtn.hidden = false;
    mergeBtn.textContent = `合并到 ${sibling.name}`;
    mergeBtn.title = `把 ${c.mac} 归到 ${sibling.mac}，共用同一套限速和封禁`;
    mergeBtn.onclick = async (e) => {
      e.stopPropagation();
      const r = await api('/api/device/link', {
        method: 'POST',
        body: JSON.stringify({ mac: c.mac, aliasTo: sibling.mac }),
      });
      if (!r.ok) toast(r.error || '合并失败', true);
      else toast(`已合并到 ${sibling.name}`);
    };
  } else if (c.mergedCount > 0) {
    mergeBtn.hidden = false;
    mergeBtn.textContent = `＋${c.mergedCount} 个随机地址`;
    mergeBtn.title = '这些随机 MAC 共用本设备的限速和封禁';
    mergeBtn.onclick = async (e) => {
      e.stopPropagation();
      if (!confirm('拆开这些随机地址？')) return;
      for (const x of (status?.clients || []).filter((v) => v.aliasTo === c.mac)) {
        await api('/api/device/link', { method: 'POST', body: JSON.stringify({ mac: x.mac, aliasTo: '' }) });
      }
      toast('已拆开');
    };
  } else {
    mergeBtn.hidden = true;
  }

  const rate = card.querySelector('.js-rate');
  if (offline) {
    rate.textContent = c.lastSeen ? `最后在线 ${timeAgo(c.lastSeen)}` : '';
  } else {
    rate.textContent = c.downBps !== undefined ? `↓ ${fmtRate(c.downBps)}  ↑ ${fmtRate(c.upBps)}` : '';
  }

  const down = card.querySelector('[data-dir="down"]');
  const up = card.querySelector('[data-dir="up"]');
  const prio = card.querySelector('[data-role="prio"]');
  const downT = card.querySelector('[data-toggle="down"]');
  const upT = card.querySelector('[data-toggle="up"]');

  // ignore the echo of an in-flight change so the switch does not flicker
  const pending = Number(card.dataset.pendingUntil || 0) > Date.now();
  if (!pending) {
    if (document.activeElement !== down) down.value = c.downMbps || '';
    if (document.activeElement !== up) up.value = c.upMbps || '';
    if (document.activeElement !== downT) downT.checked = (c.downMbps || 0) > 0;
    if (document.activeElement !== upT) upT.checked = (c.upMbps || 0) > 0;
  }
  if (document.activeElement !== prio) prio.value = String(c.prio || 3);
}

function createBlackCard(c) {
  const card = document.createElement('div');
  card.className = 'device blackrow';
  card.dataset.mac = c.mac;
  card.innerHTML = `
    <div class="avatar js-avatar">·</div>
    <div class="device-main">
      <div class="device-name"><span class="js-name">—</span><span class="pill off">已封禁</span></div>
      <div class="device-meta"><span class="js-ip"></span><span class="js-mac"></span><span class="js-seen"></span></div>
    </div>
    <div class="device-actions"><button class="ghost-btn small" data-act="unblock">解封</button></div>`;

  card.querySelector('[data-act="unblock"]').addEventListener('click', async () => {
    const r = await api('/api/device', {
      method: 'POST',
      body: JSON.stringify({ mac: card.dataset.mac, blocked: false }),
    });
    if (!r.ok) toast(r.error || '解封失败', true);
    else {
      toast('已解封');
      if (r.warning) toast(r.warning, true);
    }
  });

  el.blacklist.appendChild(card);
  return card;
}

function updateBlackCard(card, c) {
  card.querySelector('.js-avatar').textContent = (c.name || '?').slice(0, 1).toUpperCase();
  card.querySelector('.js-name').textContent = c.name || '未知设备';
  card.querySelector('.js-ip').textContent = c.ip || '无 IP';
  card.querySelector('.js-mac').textContent = c.mac.toUpperCase();
  const seen = card.querySelector('.js-seen');
  let text = c.online === false
    ? (c.lastSeen ? `最后在线 ${timeAgo(c.lastSeen)}` : '离线')
    : '在线';
  if (c.online !== false && c.blockedDrops > 0) {
    text += ` · 已拦截 ${Number(c.blockedDrops).toLocaleString()} 个包`;
  }
  seen.textContent = text;
}

function renderBlacklist(list) {
  el.blackCount.textContent = String(list.length);
  el.blacklistEmpty.hidden = list.length > 0;

  const seen = new Set();
  for (const c of list) {
    seen.add(c.mac);
    let card = el.blacklist.querySelector(`[data-mac="${c.mac}"]`);
    if (!card) card = createBlackCard(c);
    updateBlackCard(card, c);
  }
  for (const card of [...el.blacklist.children]) {
    if (!seen.has(card.dataset.mac)) card.remove();
  }
}

function findSibling(c) {
  const all = status?.clients || [];
  if (!c.randomMac || !c.hostname) return null;
  const h = c.hostname.toLowerCase();
  return all.find((x) =>
    x.mac !== c.mac && !x.aliasTo && !x.randomMac &&
    x.hostname && x.hostname.toLowerCase() === h
  ) || null;
}

function renderDevices(all) {
  // randomised MACs merged into a real device are shown on that card instead
  const mergedBy = {};
  for (const c of all) if (c.aliasTo) mergedBy[c.aliasTo] = (mergedBy[c.aliasTo] || 0) + 1;
  const list = all.filter((c) => !c.aliasTo).map((c) => ({ ...c, mergedCount: mergedBy[c.mac] || 0 }));

  const normal = list.filter((c) => !c.blocked);
  const blocked = list.filter((c) => c.blocked);

  const onlineCount = normal.filter((c) => c.online !== false).length;
  el.deviceCount.textContent = onlineCount === normal.length ? `${normal.length}` : `${onlineCount}/${normal.length}`;
  el.devicesEmpty.hidden = normal.length > 0;

  const seen = new Set();
  for (const c of normal) {
    seen.add(c.mac);
    let card = el.devices.querySelector(`[data-mac="${c.mac}"]`);
    if (!card) card = createDeviceCard(c);
    updateDeviceCard(card, c);
  }
  for (const card of [...el.devices.children]) {
    if (!seen.has(card.dataset.mac)) card.remove();
  }

  renderBlacklist(blocked);
}

function openDeviceDialog(mac) {
  const c = (status?.clients || []).find((x) => x.mac === mac);
  if (!c) return;
  editingMac = mac;
  el.dAlias.value = c.alias || '';
  el.dNote.value = c.note || '';
  el.dHint.textContent = `${c.kind || '未知设备'} · ${c.ip} · ${c.mac.toUpperCase()}`;
  el.deviceDialog.showModal();
}

$('#deviceForm').addEventListener('submit', async (e) => {
  if (e.submitter?.value !== 'save') return;
  e.preventDefault();
  const r = await api('/api/device', {
    method: 'POST',
    body: JSON.stringify({ mac: editingMac, alias: el.dAlias.value, note: el.dNote.value }),
  });
  el.deviceDialog.close();
  if (!r.ok) toast(r.error || '保存失败', true);
  else toast('已保存');
});

// ---------------- chart / logs ----------------

function renderChart(history) {
  const canvas = el.chart;
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth;
  const h = 72;
  if (canvas.width !== Math.round(w * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const data = history || [];
  const max = Math.max(1024, ...data.map((d) => Math.max(d.inBps, d.outBps)));

  const line = (key, color) => {
    if (data.length < 2) return;
    ctx.beginPath();
    data.forEach((d, i) => {
      const x = (i / (data.length - 1)) * w;
      const y = h - (d[key] / max) * (h - 6) - 3;
      i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
    });
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.lineJoin = 'round';
    ctx.stroke();
  };

  line('inBps', '#5b8cff');
  line('outBps', '#35d07f');
}

function renderLogs(logs) {
  el.log.textContent = (logs || [])
    .slice(-40)
    .map((l) => `[${new Date(l.t).toLocaleTimeString('zh-CN', { hour12: false })}] ${l.message}`)
    .join('\n');
  el.log.scrollTop = el.log.scrollHeight;
}

// ---------------- render ----------------

function render(s) {
  if (!s) return;
  status = s;
  const on = Boolean(s.active);

  el.statusDot.className = 'dot' + (busy ? ' pending' : on ? ' on' : '');
  el.statusText.textContent = busy ? '处理中…' : on ? '热点运行中' : '热点已关闭';
  el.ssidTitle.textContent = s.ssid || s.config?.ssid || '未设置';
  el.methodText.textContent = on
    ? `通过 ${s.method === 'winrt' ? '系统热点' : '承载网络'} 共享 · 上行 ${s.uplinkNic?.name || '—'}`
    : '开启后，设备连接即可上网';

  el.powerBtn.setAttribute('aria-pressed', String(on));
  el.powerLabel.textContent = on ? '关闭热点' : '开启热点';
  el.adminBadge.hidden = Boolean(s.admin);
  document.body.classList.toggle('hotspot-on', on);

  el.kvSsid.textContent = s.config?.ssid || '—';
  el.kvPass.textContent = s.config?.password ? '•'.repeat(Math.min(12, s.config.password.length)) : '—';
  el.kvBand.textContent = BAND_LABEL[s.config?.band] || '自动';
  el.kvUplink.textContent = s.uplinkNic ? `${s.uplinkNic.name} · ${s.uplinkNic.ipv4?.[0] || ''}` : '—';

  el.rateDown.textContent = fmtRate(s.traffic?.inBps);
  el.rateUp.textContent = fmtRate(s.traffic?.outBps);

  renderDevices(s.clients || []);
  renderChart(s.traffic?.history);
  renderLogs(s.logs);

  const net = s.network || {};
  if (document.activeElement !== el.totalMbps) el.totalMbps.value = net.totalMbps || '';
  if (document.activeElement !== el.hostPrio) el.hostPrio.value = String(net.hostPrio || 3);
  el.policyHint.textContent = net.auto
    ? `自动：按上行链路速率的 90%（当前约 ${net.totalMbps} Mbps），填 0 保持自动`
    : `手动上限 ${net.totalMbps} Mbps`;

  el.tIpv6.checked = s.config?.stealth?.ipv6 ?? false;
  el.tMcast.checked = s.config?.stealth?.multicast ?? false;
  el.tTtl.checked = Boolean(s.config?.stealth?.ttl);

  const ttlState = s.stealth?.ttl || 'missing';
  paintDataPlane(Boolean(s.dataplane));
  const av = $('#appVer');
  if (av && s.appVersion) av.textContent = s.appVersion;
  if (el.ttlTag) {
    el.ttlTag.textContent = ttlState === 'active' ? '运行中' : ttlState === 'ready' ? '已就绪' : '未安装';
    el.ttlTag.style.opacity = ttlState === 'missing' ? '0.5' : '1';
  }

  // the data plane is what actually blocks / shapes traffic, so it must be visible
  if (el.dpBadge) {
    el.dpBadge.hidden = !on;
    if (ttlState === 'active') {
      el.dpBadge.textContent = '数据面 运行中';
      el.dpBadge.className = 'badge badge-ok';
    } else if (ttlState === 'ready') {
      el.dpBadge.textContent = '数据面 未运行';
      el.dpBadge.className = 'badge badge-warn';
    } else {
      el.dpBadge.textContent = '数据面 缺失';
      el.dpBadge.className = 'badge badge-warn';
    }
  }
}

// ---------------- events ----------------

async function toggleHotspot() {
  if (busy) return;
  const on = Boolean(status?.active);
  if (!status?.admin) {
    toast('需要管理员权限：请用“以管理员身份运行”启动', true);
    return;
  }

  busy = true;
  el.powerBtn.disabled = true;
  el.statusText.textContent = on ? '正在关闭…' : '正在启动…';
  el.statusDot.className = 'dot pending';

  try {
    const body = on
      ? { action: 'stop' }
      : { action: 'start', ssid: status.config?.ssid, password: status.config?.password, band: status.config?.band };
    const r = await api('/api/hotspot', { method: 'POST', body: JSON.stringify(body) });
    if (!r.ok) toast(r.error || '操作失败', true);
    else toast(on ? '热点已关闭' : '热点已开启');
    for (const w of r.warnings || []) toast(w, true);
  } catch (e) {
    toast(e.message, true);
  } finally {
    busy = false;
    el.powerBtn.disabled = false;
  }
}

el.powerBtn.addEventListener('click', toggleHotspot);

el.themeBtn.addEventListener('click', () => {
  const next = document.documentElement.dataset.theme === 'light' ? 'dark' : 'light';
  document.documentElement.dataset.theme = next;
  try { localStorage.setItem('rw-theme', next); } catch {}
});

el.restartBtn.addEventListener('click', async () => {
  if (busy) return;
  if (!status?.admin) {
    toast('需要管理员权限：请用“以管理员身份运行”启动', true);
    return;
  }
  busy = true;
  el.restartBtn.disabled = true;
  el.powerBtn.disabled = true;
  el.statusText.textContent = '正在重启…';
  el.statusDot.className = 'dot pending';
  try {
    const r = await api('/api/hotspot/restart', { method: 'POST', body: '{}' });
    if (!r.ok) toast(r.error || '重启失败', true);
    else toast('热点已重启');
    for (const w of r.warnings || []) toast(w, true);
  } catch (e) {
    toast(e.message, true);
  } finally {
    busy = false;
    el.restartBtn.disabled = false;
    el.powerBtn.disabled = false;
  }
});

async function saveStealth() {
  const r = await api('/api/stealth', {
    method: 'POST',
    body: JSON.stringify({ ipv6: el.tIpv6.checked, multicast: el.tMcast.checked, ttl: el.tTtl.checked }),
  });
  if (!r.ok) toast(r.error || '设置失败', true);
  else if (r.ttl && !r.ttl.ok) toast(`数据面启动失败：${r.ttl.error || '需要管理员权限'}`, true);
}

el.tIpv6.addEventListener('change', saveStealth);
el.tMcast.addEventListener('change', saveStealth);
el.tTtl.addEventListener('change', saveStealth);

// ---------------- autostart (HKCU Run) ----------------

function autostartHintText(enabled, command, method) {
  if (!enabled) return '默认关闭。开启后会写入注册表 Run 项，不会创建计划任务。';
  if (method === 'startup') {
    return `已启用（注册表写入被系统拦截，已改用「启动」文件夹快捷方式）：${command || ''}`;
  }
  return `已启用：${command || '注册表 Run 项'}`;
}

async function loadAutostart() {
  try {
    const r = await api('/api/autostart');
    if (!r.ok) return;
    el.tAutostart.checked = Boolean(r.data?.enabled);
    el.autostartHint.textContent = autostartHintText(Boolean(r.data?.enabled), r.data?.command, r.data?.method);
  } catch {}
}

el.tAutostart.addEventListener('change', async () => {
  const want = el.tAutostart.checked;
  el.tAutostart.disabled = true;
  try {
    const r = await api('/api/autostart', {
      method: 'POST',
      body: JSON.stringify({ enabled: want }),
    });
    if (!r.ok) {
      el.tAutostart.checked = !want;
      toast(r.error || '设置失败', true);
      if (r.command) {
        el.autostartHint.textContent =
          `自动写入被拒绝。可手动在注册表 HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run 新建字符串值 Kwifi，内容：${r.command}`;
      }
    } else {
      toast(want ? '已开启开机自启（登录后驻留托盘）' : '已关闭开机自启');
      el.autostartHint.textContent = autostartHintText(want, r.data?.command, r.data?.method);
    }
  } catch (e) {
    el.tAutostart.checked = !want;
    toast(e.message, true);
  } finally {
    el.tAutostart.disabled = false;
  }
});

loadAutostart();

// ---------------- auto hotspot at boot ----------------

async function loadAutoHotspot() {
  try {
    const r = await api('/api/config');
    if (r.ok && r.data) el.tAutoHotspot.checked = Boolean(r.data.autoHotspot);
  } catch {}
}

el.tAutoHotspot.addEventListener('change', async () => {
  const want = el.tAutoHotspot.checked;
  el.tAutoHotspot.disabled = true;
  try {
    const r = await api('/api/config', {
      method: 'POST',
      body: JSON.stringify({ autoHotspot: want }),
    });
    if (!r.ok) {
      el.tAutoHotspot.checked = !want;
      toast(r.error || '设置失败', true);
    } else {
      toast(want ? '已开启：下次启动自动开热点' : '已关闭自动开热点');
    }
  } catch (e) {
    el.tAutoHotspot.checked = !want;
    toast(e.message, true);
  } finally {
    el.tAutoHotspot.disabled = false;
  }
});

loadAutoHotspot();

// ---------------- administrator rights on demand ----------------

async function refreshAdmin() {
  try {
    const r = await api('/api/elevate');
    if (!r.ok) return;
    const admin = Boolean(r.data?.admin);
    if (el.adminBanner) el.adminBanner.hidden = admin;
    document.body.classList.toggle('is-admin', admin);
  } catch {}
}

el.elevateBtn?.addEventListener('click', async () => {
  el.elevateBtn.disabled = true;
  el.elevateBtn.textContent = '正在请求…';
  try {
    const r = await api('/api/elevate', { method: 'POST' });
    if (r.data?.handover) {
      toast('已请求管理员权限，程序会重新启动');
      el.adminBanner.hidden = true;
    } else if (r.data?.admin) {
      el.adminBanner.hidden = true;
    }
  } catch {
    el.elevateBtn.disabled = false;
    el.elevateBtn.textContent = '授予管理员权限';
  }
});

refreshAdmin();

el.doctorCopy?.addEventListener('click', async () => {
  try {
    await navigator.clipboard.writeText($('#doctorOut').textContent || '');
    toast('诊断内容已复制');
  } catch {
    toast('复制失败，请手动选中', true);
  }
});

// ---------------- backend as a Windows service ----------------
// Installing the service is the one action that needs admin; once it is running
// the interface never asks for elevation again.

async function loadService() {
  try {
    const r = await api('/api/service');
    if (!r.ok) return;
    const { installed, running } = r.data || {};
    el.svcState.textContent = !installed
      ? '未安装'
      : running
        ? '已安装，运行中'
        : '已安装，未运行';
    el.svcInstall.hidden = Boolean(installed);
    el.svcRemove.hidden = !installed;
  } catch {}
}

async function setService(action) {
  const btn = action === 'install' ? el.svcInstall : el.svcRemove;
  const label = btn.textContent;
  btn.disabled = true;
  btn.textContent = '处理中…';
  try {
    const r = await api('/api/service', {
      method: 'POST',
      body: JSON.stringify({ action }),
    });
    if (r.ok) {
      toast(action === 'install' ? '服务已安装并在后台运行' : '服务已卸载');
      localStorage.removeItem('kwifiPendingService');
      await loadService();
    } else if (String(r.error || '').includes('管理员')) {
      // remember the intent, then relaunch elevated; the new window finishes it
      localStorage.setItem('kwifiPendingService', action);
      toast('需要管理员权限，正在请求…');
      await api('/api/elevate', { method: 'POST' });
    } else {
      toast(r.error || '操作失败', true);
    }
  } catch (e) {
    toast(e.message, true);
  } finally {
    btn.disabled = false;
    btn.textContent = label;
  }
}

el.svcInstall?.addEventListener('click', () => setService('install'));
el.svcRemove?.addEventListener('click', () => setService('remove'));

(async () => {
  await loadService();
  const pending = localStorage.getItem('kwifiPendingService');
  if (pending) {
    localStorage.removeItem('kwifiPendingService');
    await setService(pending);
  }
})();

// runtime facts shown in the settings card
// the data plane enforces blocking and shaping; show whether it is actually up
function paintDataPlane(up) {
  const dp = $('#dpState');
  if (!dp) return;
  dp.textContent = up ? '运行中' : '未运行';
  dp.classList.toggle('warn-text', !up);
}
function paintFromStatus(s) {
  if (!s) return;
  paintDataPlane(Boolean(s.stealth && s.stealth.ttl === 'active'));
  const av = $('#appVer');
  if (av && s.appVersion) av.textContent = s.appVersion;
}

const listenAddr = $('#listenAddr');
if (listenAddr) listenAddr.textContent = `127.0.0.1:${location.port || 17899}`;

async function saveNetwork() {
  const r = await api('/api/network', {
    method: 'POST',
    body: JSON.stringify({
      totalMbps: Math.max(0, Number(el.totalMbps.value) || 0),
      hostPrio: Number(el.hostPrio.value),
    }),
  });
  if (!r.ok) toast(r.error || '保存失败', true);
  else toast('带宽策略已更新');
}

el.totalMbps.addEventListener('change', saveNetwork);
el.hostPrio.addEventListener('change', saveNetwork);

$('#editNetBtn').addEventListener('click', () => {
  $('#fSsid').value = status?.config?.ssid || '';
  $('#fPass').value = status?.config?.password || '';
  $('#fBand').value = status?.config?.band || 'auto';
  el.netDialog.showModal();
});

$('#netForm').addEventListener('submit', async (e) => {
  if (e.submitter?.value !== 'save') return;
  e.preventDefault();
  const ssid = $('#fSsid').value.trim();
  const password = $('#fPass').value;
  const band = $('#fBand').value;
  await api('/api/config', { method: 'POST', body: JSON.stringify({ ssid, password, band }) });
  el.netDialog.close();
  if (status?.active) {
    await api('/api/hotspot', { method: 'POST', body: JSON.stringify({ action: 'stop' }) });
    const r = await api('/api/hotspot', { method: 'POST', body: JSON.stringify({ action: 'start', ssid, password, band }) });
    if (!r.ok) toast(r.error, true);
    else toast('已按新配置重启热点');
  } else {
    toast('已保存');
  }
});

$('#doctorBtn').addEventListener('click', async () => {
  el.doctorOut.textContent = '正在诊断…';
  el.doctorDialog.showModal();
  const r = await api('/api/doctor');
  el.doctorOut.textContent = JSON.stringify(r.data || r, null, 2);
});

$('#quitBtn').addEventListener('click', async () => {
  await api('/api/quit', { method: 'POST', body: JSON.stringify({ stopHotspot: status?.active }) });
  toast('已退出，可关闭此窗口');
  setTimeout(() => window.close(), 600);
});

// live updates
const es = new EventSource('/api/events');
es.onmessage = (ev) => {
  try {
    const s = JSON.parse(ev.data);
    if (s && s.ts) render(s);
  } catch {}
};

function pollNow() {
  fetch('/api/status')
    .then((r) => r.json())
    .then((r) => r.data && render(r.data))
    .catch(() => {});
}

pollNow();
checkBackend();
setInterval(checkBackend, 10000);
window.addEventListener('resize', () => status && renderChart(status.traffic?.history));