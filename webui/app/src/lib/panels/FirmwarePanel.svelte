<script>
  import { device } from '../stores/device.svelte.js';

  let dfuSent = $state(false);

  async function enterDfuMode() {
    if (!confirm('Enter DFU bootloader mode? The device will disconnect and wait for nrfutil.')) return;
    try {
      await device.sendRaw('reset dfu');
      dfuSent = true;
    } catch (err) {
      console.error('Failed to enter DFU mode:', err);
    }
  }

  function copyCommand(text) {
    navigator.clipboard.writeText(text);
  }

  const nrfutilCmd = 'nrfutil dfu usb-serial -pkg firmware.zip -p /dev/ttyACM0';
  const nrfutilInstall = 'pip install nrfutil';
</script>

<div class="panel">
  <!-- Version + DFU Instructions -->
  <div class="card">
    <h3>Firmware Update</h3>
    <div class="stat-row">
      <span>Current Version</span>
      <span class="mono">{device.status.fwVersion ?? '—'}</span>
    </div>
    <p class="desc">Firmware is flashed using <strong>nrfutil</strong> over USB serial. The browser cannot perform the update directly — use the steps below.</p>

    <div class="steps">
      <div class="step">
        <span class="step-num">1</span>
        <div class="step-content">
          <p>Install nrfutil (if not already installed):</p>
          <div class="cmd-row">
            <code>{nrfutilInstall}</code>
            <button class="btn-copy" onclick={() => copyCommand(nrfutilInstall)} title="Copy">
              <svg width="14" height="14" viewBox="0 0 16 16" fill="currentColor"><path d="M0 6.75C0 5.784.784 5 1.75 5h1.5a.75.75 0 010 1.5h-1.5a.25.25 0 00-.25.25v7.5c0 .138.112.25.25.25h7.5a.25.25 0 00.25-.25v-1.5a.75.75 0 011.5 0v1.5A1.75 1.75 0 019.25 16h-7.5A1.75 1.75 0 010 14.25v-7.5z"/><path d="M5 1.75C5 .784 5.784 0 6.75 0h7.5C15.216 0 16 .784 16 1.75v7.5A1.75 1.75 0 0114.25 11h-7.5A1.75 1.75 0 015 9.25v-7.5zm1.75-.25a.25.25 0 00-.25.25v7.5c0 .138.112.25.25.25h7.5a.25.25 0 00.25-.25v-7.5a.25.25 0 00-.25-.25h-7.5z"/></svg>
            </button>
          </div>
        </div>
      </div>

      <div class="step">
        <span class="step-num">2</span>
        <div class="step-content">
          <p>Put the device into DFU bootloader mode:</p>
          <button class="btn primary" onclick={enterDfuMode}
                  disabled={device.connectionStatus !== 'connected' || dfuSent}>
            {dfuSent ? 'DFU Mode Entered' : 'Enter DFU Mode'}
          </button>
          {#if dfuSent}
            <p class="hint">Device is now in DFU bootloader mode. The serial connection has been dropped — this is expected.</p>
          {/if}
        </div>
      </div>

      <div class="step">
        <span class="step-num">3</span>
        <div class="step-content">
          <p>Run nrfutil in your terminal (adjust port and package path):</p>
          <div class="cmd-row">
            <code>{nrfutilCmd}</code>
            <button class="btn-copy" onclick={() => copyCommand(nrfutilCmd)} title="Copy">
              <svg width="14" height="14" viewBox="0 0 16 16" fill="currentColor"><path d="M0 6.75C0 5.784.784 5 1.75 5h1.5a.75.75 0 010 1.5h-1.5a.25.25 0 00-.25.25v7.5c0 .138.112.25.25.25h7.5a.25.25 0 00.25-.25v-1.5a.75.75 0 011.5 0v1.5A1.75 1.75 0 019.25 16h-7.5A1.75 1.75 0 010 14.25v-7.5z"/><path d="M5 1.75C5 .784 5.784 0 6.75 0h7.5C15.216 0 16 .784 16 1.75v7.5A1.75 1.75 0 0114.25 11h-7.5A1.75 1.75 0 015 9.25v-7.5zm1.75-.25a.25.25 0 00-.25.25v7.5c0 .138.112.25.25.25h7.5a.25.25 0 00.25-.25v-7.5a.25.25 0 00-.25-.25h-7.5z"/></svg>
            </button>
          </div>
          <p class="hint">On Windows use the COM port (e.g. COM3). On macOS use /dev/tty.usbmodem*.</p>
        </div>
      </div>

      <div class="step">
        <span class="step-num">4</span>
        <div class="step-content">
          <p>After flashing completes, the device reboots automatically. Reconnect from the GUI.</p>
        </div>
      </div>
    </div>
  </div>

  <div class="card">
    <p class="desc">To update another device, connect it via USB-C and repeat the steps above.</p>
  </div>
</div>

<style>
  .panel { display: flex; flex-direction: column; gap: 12px; }
  .card {
    background: var(--surface-2);
    border-radius: 8px;
    padding: 14px 18px;
  }
  .card h3 {
    margin: 0 0 10px;
    font-size: 0.85rem;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--text-secondary);
  }
  .stat-row {
    display: flex;
    justify-content: space-between;
    padding: 4px 0;
    font-size: 0.88rem;
  }
  .mono { font-family: 'JetBrains Mono', monospace; }
  .desc {
    font-size: 0.85rem;
    color: var(--text-secondary);
    line-height: 1.5;
    margin: 0 0 14px;
  }
  .steps { display: flex; flex-direction: column; gap: 16px; }
  .step {
    display: flex;
    gap: 12px;
    align-items: flex-start;
  }
  .step-num {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 26px;
    height: 26px;
    border-radius: 50%;
    background: var(--color-accent);
    color: #fff;
    font-size: 0.8rem;
    font-weight: 700;
    flex-shrink: 0;
  }
  .step-content {
    flex: 1;
    font-size: 0.85rem;
  }
  .step-content p { margin: 0 0 6px; }
  .cmd-row {
    display: flex;
    align-items: center;
    gap: 8px;
    background: #0a0a0a;
    border-radius: 6px;
    padding: 8px 12px;
    margin: 6px 0;
  }
  .cmd-row code {
    flex: 1;
    font-family: 'JetBrains Mono', monospace;
    font-size: 0.82rem;
    color: #ccc;
    user-select: all;
  }
  .btn-copy {
    background: none;
    border: none;
    color: var(--text-secondary);
    cursor: pointer;
    padding: 2px;
    flex-shrink: 0;
  }
  .btn-copy:hover { color: var(--text-primary); }
  .hint { font-size: 0.78rem; color: var(--text-secondary); margin: 4px 0; }
  .btn {
    padding: 8px 18px;
    border-radius: 6px;
    border: 1px solid var(--border);
    background: var(--surface-3);
    color: var(--text-primary);
    font-size: 0.85rem;
    cursor: pointer;
  }
  .btn.primary { background: var(--color-accent); border-color: var(--color-accent); color: #fff; }
  .btn:disabled { opacity: 0.4; cursor: not-allowed; }
</style>
