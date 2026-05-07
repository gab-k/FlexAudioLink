// Reactive device state using Svelte 5 runes

import { createTransport, supportsWebSerial } from '../transport/transport.js';

function createDeviceStore() {
  let transport = $state(null);
  let connectionStatus = $state('disconnected'); // disconnected | connecting | connected | error
  let transportType = $state(supportsWebSerial() ? 'webserial' : 'websocket');
  let demoMode = $state(false);
  let connectionError = $state(null);

  // Status data from device
  let status = $state({
    rssi: null,
    battery: null,
    packetLoss: null,
    fwVersion: null,
    peerConnected: false,
    txPower: null,
    packetsRx: 0,
    packetsTx: 0,
    packetsLost: 0,
    bufferUnderruns: 0,
    bufferOverruns: 0,
    codecErrors: 0,
  });

  // History buffers for charts
  let rssiHistory = $state([]);
  let packetLossHistory = $state([]);
  let timestamps = $state([]);
  const HISTORY_LENGTH = 120; // 60s at 500ms polling

  // Raw console log
  let consoleLog = $state([]);
  const MAX_CONSOLE_LINES = 500;

  // Pending ack tracking
  let pendingAcks = $state(new Map());
  let lastCommandStatus = $state(null); // { param, status, message }

  // Toast notifications for status bar
  let toast = $state(null); // { text, type: 'success'|'error', id }
  let _toastTimer = null;

  function showToast(text, type = 'success', durationMs = 4000) {
    if (_toastTimer) clearTimeout(_toastTimer);
    const id = Date.now();
    toast = { text, type, id };
    _toastTimer = setTimeout(() => {
      if (toast?.id === id) toast = null;
    }, durationMs);
  }

  // Config loaded flag — true after device responds to 'get all'
  let configLoaded = $state(false);

  // Current device config — zero values until populated by 'get all'
  let config = $state({
    sampleRateSpk: 0, bitWidthSpk: 0, channelsSpk: '', codecSpk: '',
    volume: 0, sidetone: 0,
    sampleRateMic: 0, bitWidthMic: 0, channelsMic: '', codecMic: '',
    micGain: 0, micMute: false,
    phyRate: 0, txPower: 0, fhssExclusion: [],
    payloadMsDl: 0, payloadMsUl: 0, jitterBufferMs: 0,
    operatingMode: '',
    audioIo: '', deviceAddr: '', peerAddr: '',
    autoSleep: 0, lowBatteryThreshold: 0,
    eq: [
      { freq: 0, gain: 0 }, { freq: 0, gain: 0 }, { freq: 0, gain: 0 },
      { freq: 0, gain: 0 }, { freq: 0, gain: 0 },
    ],
  });

  // Track unsaved changes per-panel
  let dirtyPanels = $state(new Set());

  // Drafts: persist unsaved local config per panel across tab switches
  let drafts = $state({});

  // Map from wire key=value names to config store property names
  const PARAM_TO_CONFIG = {
    sample_rate_spk: 'sampleRateSpk',
    bit_width_spk: 'bitWidthSpk',
    channels_spk: 'channelsSpk',
    codec_spk: 'codecSpk',
    volume: 'volume',
    sidetone: 'sidetone',
    sample_rate_mic: 'sampleRateMic',
    bit_width_mic: 'bitWidthMic',
    channels_mic: 'channelsMic',
    codec_mic: 'codecMic',
    mic_gain: 'micGain',
    mic_mute: 'micMute',
    phy_rate: 'phyRate',
    tx_power: 'txPower',
    fhss_exclusion: 'fhssExclusion',
    payload_ms_dl: 'payloadMsDl',
    payload_ms_ul: 'payloadMsUl',
    jitter_buffer_ms: 'jitterBufferMs',
    audio_io: 'audioIo',
    device_addr: 'deviceAddr',
    peer_addr: 'peerAddr',
    mode: 'operatingMode',
    auto_sleep: 'autoSleep',
    low_battery_threshold: 'lowBatteryThreshold',
  };

  // Validation rules for config values
  const oneOf = (...vals) => (v) => vals.includes(v);
  const isInt = (min, max) => (v) => Number.isInteger(v) && v >= min && v <= max;
  const isNum = (min, max) => (v) => typeof v === 'number' && v >= min && v <= max;
  const isBool = (v) => typeof v === 'boolean';
  const isStr = (v) => typeof v === 'string';
  const isEq = (v) => Array.isArray(v) && v.length === 5 &&
    v.every(b => typeof b === 'object' && Number.isFinite(b.freq) && Number.isFinite(b.gain));

  const CONFIG_VALIDATORS = {
    sampleRateSpk: oneOf(8000, 16000, 24000, 32000, 48000, 96000),
    bitWidthSpk: oneOf(8, 16, 24, 32),
    channelsSpk: oneOf('mono', 'stereo'),
    codecSpk: oneOf('pcm', 'adpcm', 'lc3', 'opus'),
    volume: isInt(0, 100),
    sidetone: isInt(0, 100),
    sampleRateMic: oneOf(8000, 16000, 24000, 32000, 48000, 96000),
    bitWidthMic: oneOf(8, 16, 24),
    channelsMic: oneOf('mono', 'stereo'),
    codecMic: oneOf('pcm', 'adpcm', 'lc3', 'opus'),
    micGain: isInt(0, 24),
    micMute: isBool,
    phyRate: oneOf(1, 2, 4),
    txPower: oneOf(-20, -16, -12, -8, -4, 0, 4, 8),
    fhssExclusion: (v) => Array.isArray(v) && v.every(n => Number.isInteger(n) && n >= 0 && n < 80),
    payloadMsDl: isNum(0.5, 50),
    payloadMsUl: isNum(0.5, 50),
    jitterBufferMs: isNum(1, 50),
    operatingMode: oneOf('usb', 'prop_dongle', 'prop_headset'),
    audioIo: oneOf('wired', 'usb', 'codec'),
    deviceAddr: isStr,
    peerAddr: isStr,
    autoSleep: oneOf(0, 5, 10, 15, 30, 60),
    lowBatteryThreshold: oneOf(5, 10, 15, 20),
    eq: isEq,
  };

  function validateConfig(obj) {
    const errors = [];
    for (const [key, value] of Object.entries(obj)) {
      const validator = CONFIG_VALIDATORS[key];
      if (!validator) {
        errors.push(`Unknown key: ${key}`);
      } else if (!validator(value)) {
        errors.push(`Invalid value for ${key}: ${JSON.stringify(value)}`);
      }
    }
    return errors;
  }

  // Status push abbreviated keys → status store properties
  const STATUS_KEY_MAP = {
    rssi: 'rssi',
    bat: 'battery',
    loss: 'packetLoss',
    conn: 'peerConnected',
    tx: 'packetsTx',
    rx_ok: 'packetsRx',
    lost: 'packetsLost',
    urun: 'bufferUnderruns',
    orun: 'bufferOverruns',
    cerr: 'codecErrors',
    fw: 'fwVersion',
  };

  function coerceValue(raw) {
    if (raw === 'on' || raw === 'yes') return true;
    if (raw === 'off' || raw === 'no') return false;
    if (raw === 'none') return [];
    if (/^\d+(,\d+)+$/.test(raw)) return raw.split(',').map(Number);
    const num = Number(raw);
    return Number.isFinite(num) ? num : raw;
  }

  function handleLine(line) {
    // #S key=val key=val ... — periodic status push
    if (line.startsWith('#S ')) {
      const pairs = line.slice(3).trim().split(/\s+/);
      for (const pair of pairs) {
        const eq = pair.indexOf('=');
        if (eq < 0) continue;
        const key = pair.slice(0, eq);
        const val = pair.slice(eq + 1);
        const prop = STATUS_KEY_MAP[key];
        if (prop) {
          if (prop === 'peerConnected') {
            status[prop] = val === 'yes' || val === '1';
          } else {
            status[prop] = coerceValue(val);
          }
        }
      }
      const now = Date.now() / 1000;
      timestamps.push(now);
      rssiHistory.push(status.rssi ?? -100);
      packetLossHistory.push(status.packetLoss ?? 0);
      if (timestamps.length > HISTORY_LENGTH) {
        timestamps.shift();
        rssiHistory.shift();
        packetLossHistory.shift();
      }
      return;
    }

    // OK param=value — set succeeded
    if (line.startsWith('OK ')) {
      const rest = line.slice(3).trim();
      const eq = rest.indexOf('=');
      const param = eq >= 0 ? rest.slice(0, eq) : rest;
      lastCommandStatus = { param, status: 'ok', message: line };
      const pending = pendingAcks.get(param);
      if (pending) {
        pending.resolve({ param, status: 'ok' });
        pendingAcks.delete(param);
      }
      return;
    }

    // ERR param reason — set failed
    if (line.startsWith('ERR ')) {
      const parts = line.slice(4).trim().split(/\s+/);
      const param = parts[0] || '';
      const reason = parts.slice(1).join(' ');
      lastCommandStatus = { param, status: 'error', message: reason };
      const pending = pendingAcks.get(param);
      if (pending) {
        pending.reject(new Error(`${param}: ${reason}`));
        pendingAcks.delete(param);
      }
      return;
    }

    // [group] header — ignore
    if (/^\[.+\]$/.test(line)) return;

    // key=value — get response, update config store
    const eq = line.indexOf('=');
    if (eq > 0 && /^[a-z_][a-z0-9_]*$/i.test(line.slice(0, eq))) {
      const key = line.slice(0, eq);
      const val = line.slice(eq + 1);
      // EQ bands: eq0=100,0 eq1=400,2 etc.
      const eqMatch = key.match(/^eq(\d+)$/);
      if (eqMatch) {
        const idx = parseInt(eqMatch[1]);
        const [freq, gain] = val.split(',').map(Number);
        if (idx < config.eq.length) {
          config.eq[idx] = { freq, gain };
          config.eq = [...config.eq];
        }
        return;
      }
      const prop = PARAM_TO_CONFIG[key];
      if (prop) {
        if (prop === 'micMute') {
          config[prop] = val === 'on' || val === 'true' || val === '1';
        } else if (prop === 'fhssExclusion') {
          config[prop] = val === 'none' ? [] : val.split(',').map(Number);
        } else {
          config[prop] = coerceValue(val);
        }
        configLoaded = true;
      }
      return;
    }

    // Anything else — raw console text
    addConsoleLine(line);
  }

  function addConsoleLine(line) {
    consoleLog.push({ time: new Date().toISOString(), text: line });
    if (consoleLog.length > MAX_CONSOLE_LINES) consoleLog.shift();
  }

  async function connect({ demo = false } = {}) {
    if (transport) await disconnect();
    configLoaded = false;
    connectionError = null;
    demoMode = demo;
    transport = createTransport({ demo });
    connectionStatus = 'connecting';
    transport.addEventListener('status', (e) => {
      // Don't let transport overwrite 'error' with 'disconnected'
      if (connectionStatus === 'error' && e.detail === 'disconnected') return;
      connectionStatus = e.detail;
    });
    transport.onMessage = handleLine;
    try {
      await transport.connect();
      connectionStatus = 'connected';
      // Init: suppress echo, start status push, request full config
      await transport.send('echo off');
      await transport.send('status on 500');
      await transport.send('get all');
    } catch (err) {
      connectionStatus = 'error';
      if (!demo && !supportsWebSerial()) {
        connectionError = 'Could not connect to WebSocket bridge. Is websocket_serial_bridge.py running?';
      } else if (!demo) {
        connectionError = 'Could not connect to device. Check USB connection.';
      } else {
        connectionError = err.message;
      }
    }
  }

  async function disconnect() {
    if (transport) {
      await transport.disconnect();
      transport = null;
    }
    connectionStatus = 'disconnected';
    configLoaded = false;
    demoMode = false;
    connectionError = null;
  }

  async function sendCommand(param, value) {
    if (!transport) throw new Error('Not connected');
    // Format booleans as on/off, arrays as comma-separated
    let wireVal = value;
    if (typeof value === 'boolean') wireVal = value ? 'on' : 'off';
    else if (Array.isArray(value)) wireVal = value.length ? value.join(',') : 'none';
    const line = `set ${param} ${wireVal}`;
    addConsoleLine(`> ${line}`);
    await transport.send(line);
    return new Promise((resolve, reject) => {
      pendingAcks.set(param, { resolve, reject });
      setTimeout(() => {
        if (pendingAcks.has(param)) {
          pendingAcks.delete(param);
          reject(new Error(`Timeout waiting for ack: ${param}`));
        }
      }, 5000);
    });
  }

  async function sendRaw(text) {
    if (!transport) throw new Error('Not connected');
    addConsoleLine(`> ${text}`);
    await transport.send(text);
  }

  function markDirty(panel) {
    dirtyPanels = new Set([...dirtyPanels, panel]);
  }

  function clearDirty(panel) {
    const next = new Set(dirtyPanels);
    next.delete(panel);
    dirtyPanels = next;
    delete drafts[panel];
    drafts = { ...drafts };
  }

  function saveDraft(panel, localConfig) {
    drafts = { ...drafts, [panel]: { ...localConfig } };
  }

  function getDraft(panel) {
    return drafts[panel] || null;
  }

  function roleForMode(mode) {
    if (mode === 'prop_dongle') return 'dongle';
    if (mode === 'prop_headset') return 'headset';
    if (mode === 'usb') return 'usb';
    return '';
  }

  return {
    get transport() { return transport; },
    get connectionStatus() { return connectionStatus; },
    get configLoaded() { return configLoaded; },
    get demoMode() { return demoMode; },
    get connectionError() { return connectionError; },
    get transportType() { return transportType; },
    get status() { return status; },
    get config() { return config; },
    get rssiHistory() { return rssiHistory; },
    get packetLossHistory() { return packetLossHistory; },
    get timestamps() { return timestamps; },
    get consoleLog() { return consoleLog; },
    get toast() { return toast; },
    get dirtyPanels() { return dirtyPanels; },
    get drafts() { return drafts; },
    // Effective mode: reflects pending (unsaved) mode draft if one exists
    get effectiveOperatingMode() { return drafts.mode?.operatingMode ?? config.operatingMode; },
    get effectiveDeviceRole() { return roleForMode(drafts.mode?.operatingMode ?? config.operatingMode); },
    saveDraft,
    getDraft,
    connect,
    disconnect,
    sendCommand,
    sendRaw,
    handleLine,
    markDirty,
    clearDirty,
    validateConfig,
    showToast,
    set config(v) { config = v; },
  };
}

export const device = createDeviceStore();
