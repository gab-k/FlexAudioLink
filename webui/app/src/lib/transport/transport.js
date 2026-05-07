// Transport abstraction — unifies WebSerial and WebSocket backends

export class TransportEvent extends EventTarget {
  _onMessage = null;

  set onMessage(cb) {
    if (this._onMessage) this.removeEventListener('message', this._onMessage);
    this._onMessage = (e) => cb(e.detail);
    this.addEventListener('message', this._onMessage);
  }

  emit(data) {
    this.dispatchEvent(new CustomEvent('message', { detail: data }));
  }

  emitStatus(status) {
    this.dispatchEvent(new CustomEvent('status', { detail: status }));
  }
}

export class WebSerialTransport extends TransportEvent {
  port = null;
  reader = null;
  writer = null;
  readLoopActive = false;
  buffer = '';

  // USB VID/PID for the dongle — update to match actual hardware
  static FILTERS = [{ usbVendorId: 0x1FC9, usbProductId: 0x00A2 }];

  async connect() {
    try {
      this.port = await navigator.serial.requestPort({ filters: WebSerialTransport.FILTERS });
      await this.port.open({ baudRate: 115200 });
      this.writer = this.port.writable.getWriter();
      this.emitStatus('connected');
      this._readLoop();
    } catch (err) {
      this.emitStatus('error');
      throw err;
    }
  }

  async _readLoop() {
    this.readLoopActive = true;
    const decoder = new TextDecoder();
    while (this.port?.readable && this.readLoopActive) {
      this.reader = this.port.readable.getReader();
      try {
        while (true) {
          const { value, done } = await this.reader.read();
          if (done) break;
          this.buffer += decoder.decode(value, { stream: true });
          this._processBuffer();
        }
      } catch (err) {
        if (this.readLoopActive) {
          console.error('Serial read error:', err);
          this.emitStatus('error');
        }
      } finally {
        this.reader.releaseLock();
        this.reader = null;
      }
    }
  }

  _processBuffer() {
    let nlIdx;
    while ((nlIdx = this.buffer.indexOf('\n')) !== -1) {
      const line = this.buffer.slice(0, nlIdx).trim();
      this.buffer = this.buffer.slice(nlIdx + 1);
      if (line.length === 0) continue;
      this.emit(line);
    }
  }

  async send(text) {
    if (!this.writer) throw new Error('Not connected');
    const encoder = new TextEncoder();
    await this.writer.write(encoder.encode(text + '\n'));
  }

  async disconnect() {
    this.readLoopActive = false;
    if (this.reader) {
      try { await this.reader.cancel(); } catch {}
    }
    if (this.writer) {
      try { this.writer.releaseLock(); } catch {}
      this.writer = null;
    }
    if (this.port) {
      try { await this.port.close(); } catch {}
      this.port = null;
    }
    this.buffer = '';
    this.emitStatus('disconnected');
  }
}

export class WebSocketTransport extends TransportEvent {
  ws = null;
  url = 'ws://localhost:8765';

  async connect() {
    return new Promise((resolve, reject) => {
      let connected = false;
      let failed = false;
      this.ws = new WebSocket(this.url);
      this.ws.onopen = () => {
        connected = true;
        this.emitStatus('connected');
        resolve();
      };
      this.ws.onerror = (e) => {
        if (!connected && !failed) {
          failed = true;
          this.emitStatus('error');
          reject(new Error('WebSocket connection failed'));
        }
      };
      this.ws.onclose = () => {
        if (!connected && !failed) {
          failed = true;
          this.emitStatus('error');
          reject(new Error('WebSocket connection closed'));
        } else if (connected) {
          this.emitStatus('disconnected');
        }
        // If failed, don't emit 'disconnected' — keep 'error' status
      };
      this.ws.onmessage = (event) => {
        // Each WS message is one text line (bridge forwards serial lines)
        const line = (typeof event.data === 'string' ? event.data : '').trim();
        if (line.length) this.emit(line);
      };
    });
  }

  async send(text) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      throw new Error('Not connected');
    }
    this.ws.send(text + '\n');
  }

  async disconnect() {
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
    this.emitStatus('disconnected');
  }
}

export class DemoTransport extends TransportEvent {
  static MODE_AUDIO_IO = {
    usb: 'wired',
    prop_dongle: 'usb',
    prop_headset: 'codec',
  };

  static DEFAULT_STATE = {
    sample_rate_spk: '48000', bit_width_spk: '16', channels_spk: 'stereo',
    codec_spk: 'pcm', volume: '80', sidetone: '0',
    sample_rate_mic: '48000', bit_width_mic: '16', channels_mic: 'mono',
    codec_mic: 'pcm', mic_gain: '12', mic_mute: 'off',
    phy_rate: '4', tx_power: '0', fhss_exclusion: 'none',
    payload_ms_dl: '1', payload_ms_ul: '1', jitter_buffer_ms: '10',
    mode: 'prop_dongle',
    audio_io: 'usb', device_addr: 'D0D0D0D0', peer_addr: 'A1A1A1A1',
    auto_sleep: '10', low_battery_threshold: '10',
    eq0: '100,0', eq1: '400,0', eq2: '1000,0', eq3: '4000,0', eq4: '10000,0',
  };

  static GROUPS = [
    ['[audio]', ['sample_rate_spk', 'bit_width_spk', 'channels_spk', 'codec_spk',
                 'volume', 'sidetone', 'sample_rate_mic', 'bit_width_mic',
                 'channels_mic', 'codec_mic', 'mic_gain', 'mic_mute']],
    ['[radio]', ['phy_rate', 'tx_power', 'fhss_exclusion',
                 'payload_ms_dl', 'payload_ms_ul', 'jitter_buffer_ms']],
    ['[mode]', ['mode']],
    ['[device]', ['audio_io', 'device_addr', 'peer_addr', 'auto_sleep',
                  'low_battery_threshold']],
    ['[eq]', ['eq0', 'eq1', 'eq2', 'eq3', 'eq4']],
  ];

  constructor() {
    super();
    this.state = { ...DemoTransport.DEFAULT_STATE };
    this._statusInterval = null;
    this._tx = 0;
    this._rx = 0;
    this._lost = 0;
  }

  async connect() {
    this.emitStatus('connected');
  }

  async send(text) {
    const line = text.trim();
    if (!line) return;

    // Process asynchronously so callers see responses via onMessage
    setTimeout(() => this._handle(line), 0);
  }

  _handle(line) {
    const parts = line.split(/\s+/);
    const cmd = parts[0];

    if (cmd === 'echo') {
      this.emit('OK echo');
      return;
    }

    if (cmd === 'status') {
      if (parts[1] === 'on' && parts[2]) {
        const ms = parseInt(parts[2]) || 500;
        if (this._statusInterval) clearInterval(this._statusInterval);
        this._statusInterval = setInterval(() => this._emitStatus(), ms);
        this.emit('OK status');
      } else if (parts[1] === 'off') {
        if (this._statusInterval) { clearInterval(this._statusInterval); this._statusInterval = null; }
        this.emit('OK status');
      }
      return;
    }

    if (cmd === 'get' && parts[1] === 'all') {
      for (const [header, keys] of DemoTransport.GROUPS) {
        this.emit(header);
        for (const key of keys) {
          this.emit(`${key}=${this.state[key]}`);
        }
      }
      return;
    }

    if (cmd === 'set') {
      const param = parts[1];
      const value = parts.slice(2).join(' ');
      // Handle EQ: 'set eq <band> <freq> <gain>'
      if (param === 'eq' && parts.length >= 5) {
        const eqKey = `eq${parts[2]}`;
        if (eqKey in this.state) {
          this.state[eqKey] = `${parts[3]},${parts[4]}`;
          this.emit(`OK eq=${this.state[eqKey]}`);
          return;
        }
      }
      if (param === 'save_config' || param === 'load_config') {
        this.emit(`OK ${param}`);
      } else if (param === 'mode') {
        if (!(value in DemoTransport.MODE_AUDIO_IO)) {
          this.emit('ERR mode invalid_value');
          return;
        }
        this.state.mode = value;
        this.state.audio_io = DemoTransport.MODE_AUDIO_IO[value];
        this.emit(`OK mode=${value}`);
      } else if (param in this.state) {
        this.state[param] = value;
        this.emit(`OK ${param}=${value}`);
      } else {
        this.emit(`ERR ${param} unknown_param`);
      }
      return;
    }

    if (cmd === 'reset' || cmd === 'save_config' || cmd === 'load_config') {
      this.emit(`OK ${cmd}`);
      return;
    }

    this.emit(`ERR ${cmd} unknown_command`);
  }

  _emitStatus() {
    this._tx += 8 + Math.floor(Math.random() * 5);
    this._rx += 8 + Math.floor(Math.random() * 5);
    if (Math.random() < 0.05) this._lost++;
    const rssi = -45 + Math.floor(Math.random() * 11) - 5;
    const bat = Math.max(0, Math.min(100, 85 + Math.floor(Math.random() * 3) - 1));
    const loss = (Math.random() * 0.5).toFixed(1);
    this.emit(`#S rssi=${rssi} bat=${bat} loss=${loss} conn=yes tx=${this._tx} rx_ok=${this._rx} lost=${this._lost} urun=0 orun=0 cerr=0 fw=0.1.0-demo`);
  }

  async disconnect() {
    if (this._statusInterval) { clearInterval(this._statusInterval); this._statusInterval = null; }
    this.emitStatus('disconnected');
  }
}

export function supportsWebSerial() {
  return 'serial' in navigator;
}

export function createTransport({ demo = false } = {}) {
  if (demo) return new DemoTransport();
  return supportsWebSerial() ? new WebSerialTransport() : new WebSocketTransport();
}
