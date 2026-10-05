/* NightMare Gateway dashboard.
 *
 * Polls /api/live once a second (devices + counters) and /api/vault every few
 * seconds. Counters from the gateway are cumulative; every rate shown here is
 * derived in the browser from two consecutive samples.
 *
 * Open the page with ?demo to run on synthetic data (no gateway needed).
 */
(() => {
  'use strict';

  const LIVE_MS = 1000;
  const VAULT_MS = 3000;
  const HISTORY = 120;           // samples kept in the charts (2 minutes)
  // Longer than the gateway's own send timeout (3 s, webserver.cpp), so the ESP
  // gives up on a stuck response before the browser abandons the request.
  const REQUEST_TIMEOUT_MS = 5000;
  const RETRY_INITIAL_MS = 1000; // after a failed poll: 1 s, 2 s, 4 s, 8 s, then every 10 s
  const RETRY_MAX_MS = 10000;
  const DEMO = new URLSearchParams(location.search).has('demo');

  const $ = (id) => document.getElementById(id);
  const esc = (value) => String(value ?? '').replace(/[&<>"']/g, (c) =>
    ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  /* ---------- formatting ---------------------------------------------------- */
  const fmtInt = (n) => Math.round(n).toLocaleString();
  const fmtRate = (n) => (n >= 100 ? Math.round(n) : n >= 10 ? n.toFixed(1) : n.toFixed(2)).toString();
  function fmtBytes(n) {
    if (n < 1024) return `${Math.round(n)} B`;
    if (n < 1048576) return `${(n / 1024).toFixed(1)} KB`;
    return `${(n / 1048576).toFixed(2)} MB`;
  }
  function fmtDuration(ms) {
    let s = Math.max(0, Math.floor(ms / 1000));
    const d = Math.floor(s / 86400); s %= 86400;
    const h = Math.floor(s / 3600); s %= 3600;
    const m = Math.floor(s / 60); s %= 60;
    if (d) return `${d}d ${h}h`;
    if (h) return `${h}h ${m}m`;
    if (m) return `${m}m ${s}s`;
    return `${s}s`;
  }
  const fmtAgo = (ms) => (ms < 1500 ? 'just now' : `${fmtDuration(ms)} ago`);

  /* ---------- state ---------------------------------------------------------- */
  let prev = null;               // previous /api/live sample, for rates
  let lastLive = null;
  let vaultRows = [];
  let vaultVersion = null;       // what the gateway last sent; echoed back so it can answer "no change"
  let vaultFetchedAt = 0;        // Date.now() when vaultRows' age_ms values were true
  let vaultSeen = new Map();     // topic -> revisions, to flash changed rows
  const expanded = new Set();    // device MACs whose subscription list is open

  const history = {
    labels: [],
    rx: [], tx: [],
    toRemote: [], fromRemote: [], fromLocal: [], toLocal: [],
    temp: [],
  };
  const tempRange = { min: Infinity, max: -Infinity };   // over this page's lifetime

  /* ---------- rates ---------------------------------------------------------- */
  // 32-bit counters on the gateway wrap; a smaller number just means it wrapped.
  const delta = (now, before) => (now >= before ? now - before : now + 4294967296 - before);

  function computeRates(sample) {
    if (!prev || sample.uptime_ms <= prev.uptime_ms) return null;   // first sample, or the gateway rebooted
    const dt = (sample.uptime_ms - prev.uptime_ms) / 1000;
    if (dt <= 0) return null;
    const rate = (a, b) => delta(a, b) / dt;
    return {
      rx: rate(sample.espnow.rx_packets, prev.espnow.rx_packets),
      tx: rate(sample.espnow.tx_packets, prev.espnow.tx_packets),
      failed: rate(sample.espnow.tx_failed, prev.espnow.tx_failed),
      rxBytes: rate(sample.espnow.rx_bytes, prev.espnow.rx_bytes),
      txBytes: rate(sample.espnow.tx_bytes, prev.espnow.tx_bytes),
      toRemote: rate(sample.messages.to_remote, prev.messages.to_remote),
      fromRemote: rate(sample.messages.from_remote, prev.messages.from_remote),
      fromLocal: rate(sample.messages.from_local, prev.messages.from_local),
      toLocal: rate(sample.messages.to_local, prev.messages.to_local),
    };
  }

  /* ---------- charts --------------------------------------------------------- */
  const charts = {};
  const cssVar = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

  function makeChart(canvasId, series, options = {}) {
    if (!window.Chart) return null;
    const ctx = $(canvasId).getContext('2d');
    const datasets = series.map((s) => ({
      label: s.label, data: history[s.key], borderWidth: 2, tension: 0.35, pointRadius: 0, fill: true,
      _key: s.key, _color: s.color, _unit: options.unit ?? '/s',
    }));
    const chart = new Chart(ctx, {
      type: 'line',
      data: { labels: history.labels, datasets },
      options: {
        responsive: true, maintainAspectRatio: false, animation: false,
        interaction: { mode: 'index', intersect: false },
        plugins: {
          legend: { display: false },
          tooltip: {
            callbacks: {
              title: (items) => `${HISTORY - 1 - items[0].dataIndex}s ago`,
              label: (item) => item.dataset._unit === '/s'
                ? ` ${item.dataset.label}: ${fmtRate(item.parsed.y)}/s`
                : ` ${item.dataset.label}: ${item.parsed.y.toFixed(1)}${item.dataset._unit}`,
            },
          },
        },
        scales: {
          x: { display: false },
          y: options.unit
            ? { beginAtZero: false, grace: '15%', border: { display: false }, ticks: { maxTicksLimit: 5 } }
            : { beginAtZero: true, suggestedMax: 4, border: { display: false }, ticks: { maxTicksLimit: 5 } },
        },
      },
    });
    charts[canvasId] = chart;
    return chart;
  }

  function styleCharts() {
    const grid = cssVar('--border-soft');
    const text = cssVar('--faint');
    for (const chart of Object.values(charts)) {
      chart.options.scales.y.grid = { color: grid };
      chart.options.scales.y.ticks.color = text;
      for (const ds of chart.data.datasets) {
        const color = cssVar(ds._color);
        ds.borderColor = color;
        const g = chart.ctx.createLinearGradient(0, 0, 0, 200);
        g.addColorStop(0, color + '38');
        g.addColorStop(1, color + '00');
        ds.backgroundColor = g;
      }
      chart.update('none');
    }
  }

  function initCharts() {
    makeChart('chart-packets', [
      { key: 'rx', label: 'rx', color: '--accent' },
      { key: 'tx', label: 'tx', color: '--violet' },
    ]);
    makeChart('chart-messages', [
      { key: 'toRemote', label: 'device → MQTT', color: '--info' },
      { key: 'fromRemote', label: 'MQTT → device', color: '--good' },
      { key: 'toLocal', label: 'gateway → devices', color: '--violet' },
      { key: 'fromLocal', label: 'devices → gateway', color: '--accent' },
    ]);
    makeChart('chart-temp', [{ key: 'temp', label: 'chip', color: '--bad' }], { unit: ' °C' });
    styleCharts();
    matchMedia('(prefers-color-scheme: light)').addEventListener?.('change', styleCharts);
  }

  function pushHistory(rates, temp) {
    for (const key of Object.keys(history)) {
      if (key === 'labels') history.labels.push('');
      else if (key === 'temp') history.temp.push(temp);
      else history[key].push(rates ? rates[key] : null);
      if (history[key].length > HISTORY) history[key].shift();
    }
    for (const chart of Object.values(charts)) chart.update('none');
  }

  /* ---------- header / KPIs --------------------------------------------------- */
  function setPill(id, level, text) {
    const el = $(id);
    el.className = `pill ${level}`;
    el.querySelector('em').textContent = text;
  }

  function renderStatus(d) {
    const wifi = d.wifi || {};
    setPill('pill-wifi', wifi.connected ? 'ok' : 'bad', wifi.connected ? (wifi.ssid || 'connected') : 'down');
    setPill('pill-mqtt', d.mqtt.connected ? 'ok' : 'bad', d.mqtt.connected ? 'connected' : 'offline');

    const e = d.espnow;
    const beaconOk = e.beacon_active && e.seconds_since_last_beacon != null && e.seconds_since_last_beacon <= 15;
    setPill('pill-espnow', beaconOk ? 'ok' : e.beacon_active ? 'warn' : 'bad',
      beaconOk ? 'beaconing' : e.beacon_active ? 'beacon stalled' : 'inactive');

    const bits = [`v${d.gateway.version}`];
    if (wifi.ip) bits.push(wifi.ip);
    bits.push(`up ${fmtDuration(d.uptime_ms)}`);
    $('gw-sub').textContent = bits.join(' · ');
    $('foot-info').textContent = `NightMare Gateway v${d.gateway.version} · built ${d.gateway.build}`;
    const sys = d.system;
    $('foot-sys').textContent = [
      wifi.rssi != null ? `Wi-Fi ${wifi.rssi} dBm ch ${wifi.channel}` : null,
      sys.temperature_c != null ? `${sys.temperature_c.toFixed(1)} °C` : null,
      `gateway ${d.gateway.state.replace(/_/g, ' ')}`,
    ].filter(Boolean).join(' · ');
  }

  function renderKpis(d, rates) {
    const e = d.espnow;
    const connected = d.devices.filter((x) => x.state === 'connected').length;
    $('k-devices').textContent = d.devices.length;
    $('k-devices-foot').textContent = d.devices.length
      ? `${connected} connected · ${d.devices.length - connected} handshaking` : 'none yet';
    $('k-subs').textContent = e.subscribers;
    const totalSubs = d.devices.reduce((n, x) => n + x.subs, 0);
    $('k-subs-foot').textContent = `${totalSubs} subscription${totalSubs === 1 ? '' : 's'} total`;

    $('k-pps-rx').textContent = rates ? fmtRate(rates.rx) : '—';
    $('k-pps-tx').textContent = rates ? fmtRate(rates.tx) : '—';
    $('k-pps-fail').textContent = rates ? fmtRate(rates.failed) : '0';
    $('k-mps-up').textContent = rates ? fmtRate(rates.toRemote) : '—';
    $('k-mps-down').textContent = rates ? fmtRate(rates.fromRemote) : '—';

    $('k-vault').textContent = d.vault.count;
    const temp = d.system.temperature_c;
    const tile = $('kpi-temp');
    if (temp == null) {
      $('k-temp').textContent = '—';
      $('k-temp-foot').textContent = 'sensor unavailable';
      tile.classList.remove('warm', 'hot');
    } else {
      tempRange.min = Math.min(tempRange.min, temp);
      tempRange.max = Math.max(tempRange.max, temp);
      $('k-temp').textContent = temp.toFixed(1);
      $('k-temp-foot').textContent = `${tempRange.min.toFixed(1)} – ${tempRange.max.toFixed(1)} °C this session`;
      tile.classList.toggle('hot', temp >= 80);
      tile.classList.toggle('warm', temp >= 65 && temp < 80);
    }
    $('k-heap').textContent = fmtBytes(d.system.free_heap);
    $('k-heap-foot').textContent = `low-water ${fmtBytes(d.system.min_free_heap)}`;
  }

  /* ---------- devices --------------------------------------------------------- */
  function signalLevel(dbm) {
    if (dbm == null) return { bars: 0, cls: '', label: 'no data' };
    if (dbm >= -60) return { bars: 4, cls: 'good', label: 'excellent' };
    if (dbm >= -70) return { bars: 3, cls: 'good', label: 'good' };
    if (dbm >= -80) return { bars: 2, cls: 'warn', label: 'fair' };
    return { bars: 1, cls: 'bad', label: 'poor' };
  }

  function renderDevice(device, e) {
    const stateBadge = {
      connected: '<span class="badge good">Connected</span>',
      securing: '<span class="badge info">Securing</span>',
      authenticated: '<span class="badge accent">Handshaking</span>',
    }[device.state] || `<span class="badge">${esc(device.state)}</span>`;

    const sig = signalLevel(device.avg_rssi);
    const bars = [1, 2, 3, 4].map((n) => `<i class="${n <= sig.bars ? 'on' : ''}"></i>`).join('');
    const signal = device.avg_rssi == null
      ? '<span class="muted">—</span>'
      : `<div class="signal"><span class="bars ${sig.cls}" title="${sig.label}">${bars}</span>
           <span class="dbm">${device.avg_rssi.toFixed(1)} <small>dBm avg</small><br><small>last ${device.rssi} dBm</small></span></div>`;

    const lastWill = device.last_will
      ? `<div class="stack"><span class="badge good">Set</span><span class="mono muted" title="${esc(device.last_will_topic)}">${esc(device.last_will_topic)}</span></div>`
      : '<span class="badge plain">None</span>';

    // Health = how far the session is from the gateway's silence timeout.
    const timeout = e.session_timeout_ms;
    const remaining = Math.max(0, timeout - device.last_seen_ms);
    const pct = Math.max(0, Math.min(100, (remaining / timeout) * 100));
    const level = device.disconnect_candidate ? (pct < 25 ? 'bad' : 'warn') : '';
    const health = device.disconnect_candidate
      ? `<span class="badge ${pct < 25 ? 'bad' : 'warn'}">Disconnect candidate</span>`
      : '<span class="badge good">Healthy</span>';
    const healthNote = device.suspended
      ? 'handshake in progress'
      : device.state !== 'connected' ? 'completing handshake'
      : device.disconnect_candidate ? `drops in ${fmtDuration(remaining)} if silent` : `${device.rx_frames.toLocaleString()} frames`;

    const name = device.name
      ? `<div class="dev-name">${esc(device.name)}</div>`
      : '<div class="dev-name muted">Unnamed device</div>';
    const open = expanded.has(device.mac);

    const topicsRow = open && device.topics.length
      ? `<tr class="topics-row" data-for="${esc(device.mac)}"><td colspan="8"><div class="topics">${
          device.topics.map((t) => `<span class="topic-chip">${esc(t)}</span>`).join('')}</div></td></tr>`
      : '';

    return `
      <tr class="${device.disconnect_candidate ? 'candidate' : ''}">
        <td>${name}<div class="dev-mac">${esc(device.mac)} <span class="dev-cid">cid ${device.cid}</span></div></td>
        <td><span class="badge ${device.authenticated ? 'good' : 'bad'}">${device.authenticated ? 'Authenticated' : 'Unauthenticated'}</span></td>
        <td>${stateBadge}<div class="muted" style="font-size:12px;margin-top:4px">${esc(fmtDuration(device.session_ms))} session</div></td>
        <td class="num">${device.subs ? `<button class="sub-toggle" data-mac="${esc(device.mac)}" title="Show subscriptions">${device.subs} ${open ? '▾' : '▸'}</button>` : '<span class="muted">0</span>'}</td>
        <td>${lastWill}</td>
        <td>${signal}</td>
        <td>${esc(fmtAgo(device.last_seen_ms))}<div class="muted" style="font-size:12px">${
          device.rtt_ms == null ? 'rtt n/a' : `rtt ${device.rtt_ms.toFixed(1)} ms`}</div></td>
        <td><div class="health">${health}
          <div class="meter" title="time left before the gateway times this session out"><span class="${level}" style="width:${pct.toFixed(0)}%"></span></div>
          <span class="muted" style="font-size:12px">${esc(healthNote)}</span></div></td>
      </tr>${topicsRow}`;
  }

  function renderDevices(d) {
    const body = $('devices-body');
    const list = [...d.devices].sort((a, b) => (!a.name - !b.name) || (a.name || a.mac).localeCompare(b.name || b.mac));
    $('devices-empty').hidden = list.length > 0;
    $('devices-table').hidden = list.length === 0;
    const candidates = list.filter((x) => x.disconnect_candidate).length;
    $('devices-count').textContent = list.length ? `· ${list.length}` : '';
    $('devices-note').textContent = candidates
      ? `${candidates} disconnect candidate${candidates === 1 ? '' : 's'} — silent for over ${fmtDuration(d.espnow.heartbeat_ms * 2)}`
      : list.length ? `Sessions time out after ${fmtDuration(d.espnow.session_timeout_ms)} of silence` : '';
    body.innerHTML = list.map((x) => renderDevice(x, d.espnow)).join('');
  }

  $('devices-body').addEventListener('click', (event) => {
    const button = event.target.closest('.sub-toggle');
    if (!button) return;
    const mac = button.dataset.mac;
    if (!expanded.delete(mac)) expanded.add(mac);
    if (lastLive) renderDevices(lastLive);
  });

  /* ---------- vault ----------------------------------------------------------- */
  // An unchanged vault is not resent, so ages are counted up from when they were last true.
  const vaultAge = (row) => row.age_ms + (Date.now() - vaultFetchedAt);

  const filterInput = $('vault-filter');
  filterInput.addEventListener('input', renderVault);

  function renderVault() {
    const q = filterInput.value.trim().toLowerCase();
    const rows = vaultRows.filter((r) => !q || r.topic.toLowerCase().includes(q) || r.payload.toLowerCase().includes(q));
    $('vault-count').textContent = vaultRows.length
      ? (q ? `· ${rows.length} of ${vaultRows.length}` : `· ${vaultRows.length}`) : '';
    $('vault-table').hidden = rows.length === 0;
    $('vault-empty').hidden = rows.length > 0;
    $('vault-empty-text').textContent = vaultRows.length ? 'No retained message matches that filter.' : 'Nothing retained yet.';

    $('vault-body').innerHTML = rows.map((r) => {
      const changed = vaultSeen.has(r.topic) && vaultSeen.get(r.topic) !== r.revisions;
      const preview = r.payload.replace(/\s+/g, ' ').slice(0, 140);
      return `<tr class="vault-row${changed ? ' flash' : ''}" data-topic="${esc(r.topic)}">
        <td><span class="topic">${esc(r.topic)}</span></td>
        <td><div class="payload" title="${esc(preview)}">${r.encoding === 'hex' ? '<span class="badge plain info">hex</span> ' : ''}${esc(preview)}${r.truncated ? '…' : ''}</div></td>
        <td><span class="badge ${r.origin === 'remote' ? 'info' : 'accent'}">${r.origin === 'remote' ? 'MQTT' : 'ESP-NOW'}</span></td>
        <td class="num">${fmtBytes(r.size)}</td>
        <td class="num">${r.revisions}</td>
        <td class="num muted">${esc(fmtAgo(vaultAge(r)))}</td>
      </tr>`;
    }).join('');
    vaultSeen = new Map(vaultRows.map((r) => [r.topic, r.revisions]));
  }

  const dialog = $('vault-dialog');
  function highlightJson(text) {
    return esc(text).replace(
      /(&quot;(?:\\.|[^&\\]|&(?!quot;))*?&quot;)(\s*:)?|\b(true|false|null)\b|-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?/g,
      (m, str, colon, lit) => {
        if (str) return colon ? `<span class="j-key">${str}</span>${colon}` : `<span class="j-str">${str}</span>`;
        if (lit) return `<span class="j-lit">${m}</span>`;
        return `<span class="j-num">${m}</span>`;
      });
  }

  $('vault-body').addEventListener('click', (event) => {
    const row = event.target.closest('.vault-row');
    if (!row) return;
    const entry = vaultRows.find((r) => r.topic === row.dataset.topic);
    if (!entry) return;

    let shown = entry.payload, html = null;
    if (entry.encoding === 'text') {
      try { shown = JSON.stringify(JSON.parse(entry.payload), null, 2); html = highlightJson(shown); } catch { /* not JSON */ }
    } else {
      shown = entry.payload.replace(/(.{32})/g, '$1\n').trim().replace(/(..)/g, '$1 ');
    }
    $('dlg-topic').textContent = entry.topic;
    $('dlg-meta').innerHTML = [
      ['Origin', entry.origin === 'remote' ? 'MQTT' : 'ESP-NOW device'],
      ['Size', fmtBytes(entry.size) + (entry.truncated ? ' (truncated)' : '')],
      ['Encoding', entry.encoding],
      ['Updates', entry.revisions],
      ['Updated', fmtAgo(vaultAge(entry))],
    ].map(([k, v]) => `<div><dt>${k}</dt><dd>${esc(v)}</dd></div>`).join('');
    if (html) $('dlg-payload').innerHTML = html; else $('dlg-payload').textContent = shown;
    $('dlg-copy').onclick = async () => {
      try { await navigator.clipboard.writeText(entry.payload); $('dlg-copy').textContent = 'Copied'; }
      catch { $('dlg-copy').textContent = 'Copy failed'; }
      setTimeout(() => { $('dlg-copy').textContent = 'Copy payload'; }, 1500);
    };
    dialog.showModal();
  });
  dialog.addEventListener('click', (event) => { if (event.target === dialog) dialog.close(); });

  /* ---------- transport ------------------------------------------------------- */
  // `signal` cancels the request early (the poller does that when it restarts).
  async function getJson(url, signal) {
    if (DEMO) return demo(url);
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
    const cancel = () => controller.abort();
    signal?.addEventListener('abort', cancel);
    try {
      const response = await fetch(url, { signal: controller.signal, cache: 'no-store' });
      if (!response.ok) throw new Error(`${response.status}`);
      return await response.json();
    } finally {
      clearTimeout(timer);
      signal?.removeEventListener('abort', cancel);
    }
  }

  /* One chain of requests per endpoint. The next request is only scheduled once
   * the current one has finished, so requests to the same endpoint never
   * overlap. Failures back off exponentially up to RETRY_MAX_MS and a success
   * goes straight back to the normal interval. A hidden tab makes no requests;
   * showing it again restarts the chain immediately. */
  function makePoller(task, normalMs) {
    let timer = null;
    let controller = null;
    let retryMs = RETRY_INITIAL_MS;
    let generation = 0;                       // a restarted chain invalidates the old one

    async function step(mine) {
      timer = null;
      if (mine !== generation) return;

      let next = normalMs;
      if (!document.hidden) {
        controller = new AbortController();
        try {
          await task(controller.signal);
          retryMs = RETRY_INITIAL_MS;
        } catch {
          if (controller.signal.aborted && mine !== generation) return;   // superseded, not failed
          next = retryMs;
          retryMs = Math.min(retryMs * 2, RETRY_MAX_MS);
        }
      }
      if (mine === generation) timer = setTimeout(() => step(mine), next);
    }

    return {
      restart() {
        generation++;
        clearTimeout(timer);
        controller?.abort();
        retryMs = RETRY_INITIAL_MS;
        step(generation);
      },
    };
  }

  function setOnline(online) {
    $('live').className = `live ${online ? 'on' : ''}`;
    $('live').querySelector('span').textContent = DEMO ? 'demo data' : online ? 'live' : 'offline';
    $('banner').hidden = online;
  }

  async function pollLive(signal) {
    try {
      const d = await getJson('/api/live', signal);
      const rates = computeRates(d);
      if (prev && d.uptime_ms < prev.uptime_ms) {            // rebooted: restart the graphs
        for (const key of Object.keys(history)) history[key].length = 0;
        tempRange.min = Infinity; tempRange.max = -Infinity;
      }
      prev = d; lastLive = d;
      renderStatus(d);
      renderKpis(d, rates);
      renderDevices(d);
      if (rates) pushHistory(rates, d.system.temperature_c);
      setOnline(true);
    } catch (error) {
      if (!signal.aborted) setOnline(false);   // an aborted request is a restart, not an outage
      throw error;                              // the poller backs off
    }
  }

  // The live poll reports connectivity; a vault failure only backs its own chain off.
  async function pollVault(signal) {
    const query = vaultVersion == null ? '' : `?v=${vaultVersion}`;
    const v = await getJson(`/api/vault${query}`, signal);
    if (v.changed !== false) {          // the demo data has no flag and is always full
      vaultRows = v.messages;
      vaultVersion = v.version ?? null;
      vaultFetchedAt = Date.now();
    }
    renderVault();                      // also when unchanged, so the "updated … ago" column keeps counting
  }

  const liveLoop = makePoller(pollLive, LIVE_MS);
  const vaultLoop = makePoller(pollVault, VAULT_MS);

  /* ---------- demo data ------------------------------------------------------- */
  const demoState = { t0: Date.now(), counters: { rx: 5200, tx: 6100, rxb: 0, txb: 0, fl: 3, tr: 400, fr: 380, fL: 900, tL: 760, inv: 2, ign: 14 }, rev: {} };
  function demo(url) {
    const up = Date.now() - demoState.t0 + 7_260_000;
    const c = demoState.counters;
    const jitter = (n) => Math.round(n * (0.6 + Math.random() * 0.8));
    c.rx += jitter(9); c.tx += jitter(7); c.fl += Math.random() < 0.1 ? 1 : 0;
    c.rxb += jitter(1300); c.txb += jitter(1100);
    c.tr += jitter(2); c.fr += jitter(2); c.fL += jitter(3); c.tL += jitter(2);
    const mk = (mac, name, cid, subs, topics, lw, rssi, seen, state = 'connected') => ({
      mac, name, cid, state, authenticated: true, secured: state === 'connected', suspended: false,
      subs, topics, last_will: lw, last_will_topic: lw ? `${name}/status` : undefined, last_will_size: lw ? 7 : undefined,
      rssi: Math.round(rssi + (Math.random() * 4 - 2)), avg_rssi: rssi, rx_frames: 4000 + cid * 311,
      rtt_ms: +(2 + Math.random() * 6).toFixed(1), last_seen_ms: seen, session_ms: up - cid * 400_000, disconnect_candidate: seen > 30000,
    });
    const devices = [
      mk('A4:CF:12:8B:3E:10', 'greenhouse-1', 1, 3, ['greenhouse/+/set', 'Control/time', 'greenhouse-1/in'], true, -52, Math.random() * 900),
      mk('24:6F:28:AA:51:7C', 'door-sensor', 2, 2, ['door/#', 'Control/time'], true, -67.4, Math.random() * 1400),
      mk('30:AE:A4:07:0D:64', 'pump-controller', 3, 4, ['pump/+/cmd', 'pump/state', 'Control/time', 'pump-controller/in'], false, -78.8, 38_000 + (up % 9000)),
      mk('7C:DF:A1:00:B2:9E', '', 4, 0, [], false, -88.1, 300, 'securing'),
    ];
    if (url.startsWith('/api/vault')) {
      const topics = [
        ['greenhouse-1/status', 'online', 'local'],
        ['greenhouse-1/telemetry', JSON.stringify({ temp: 24.6, humidity: 61, light: 810, ts: Math.floor(Date.now() / 1000) }), 'local'],
        ['door-sensor/status', 'online', 'local'],
        ['pump/state', JSON.stringify({ running: Math.random() > .5, pressure: +(1.8 + Math.random()).toFixed(2) }), 'remote'],
        ['Control/config', JSON.stringify({ interval: 30, mode: 'eco', zones: [1, 2, 3], nested: { ok: true, note: null } }), 'remote'],
        ['pump-controller/status', 'offline', 'local'],
      ];
      return Promise.resolve({
        uptime_ms: up, count: topics.length,
        messages: topics.map(([topic, payload, origin], i) => {
          demoState.rev[topic] = (demoState.rev[topic] || 3 + i) + (Math.random() < 0.2 ? 1 : 0);
          return { topic, origin, size: payload.length, truncated: false, encoding: 'text', payload, age_ms: jitter(20000 * (i + 1)), revisions: demoState.rev[topic] };
        }),
      });
    }
    return Promise.resolve({
      uptime_ms: up,
      gateway: { state: 'running', ready: true, version: '0.1.94', build: '2026-10-02 00:29' },
      wifi: { connected: true, ssid: 'wake-iot', rssi: -58, channel: 6, ip: '192.168.1.42' },
      mqtt: { connected: true },
      system: { free_heap: 183_400, min_free_heap: 151_200, largest_free_block: 110_592, temperature_c: +(41 + 6 * Math.sin(Date.now() / 20000) + Math.random()).toFixed(2) },
      espnow: {
        beacon_active: true, seconds_since_last_beacon: Math.floor(Math.random() * 5), devices: devices.length, subscribers: 3,
        heartbeat_ms: 15000, session_timeout_ms: 60000,
        rx_packets: c.rx, tx_packets: c.tx, rx_bytes: c.rxb, tx_bytes: c.txb, tx_failed: c.fl, rx_dropped: 0, invalid_frames: c.inv, ignored_frames: c.ign,
      },
      messages: { from_local: c.fL, to_local: c.tL, from_remote: c.fr, to_remote: c.tr },
      vault: { count: 6 },
      devices,
    });
  }

  /* ---------- go -------------------------------------------------------------- */
  function start() {
    initCharts();
    if (DEMO) $('gw-sub').textContent = 'demo mode';
    liveLoop.restart();
    vaultLoop.restart();
    document.addEventListener('visibilitychange', () => {
      if (document.hidden) return;
      liveLoop.restart();
      vaultLoop.restart();
    });
    // Chart.js is deferred and comes from a CDN: build the graphs once it arrives.
    if (!window.Chart) {
      window.addEventListener('load', () => { if (window.Chart && !Object.keys(charts).length) initCharts(); });
    }
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start); else start();
})();
