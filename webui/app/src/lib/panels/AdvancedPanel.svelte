<script>
  import { device } from '../stores/device.svelte.js';
  import { tick } from 'svelte';

  let rawCmd = $state('');
  let consoleEl = $state(null);
  let showAdvanced = $state(false);

  async function sendRaw() {
    if (!rawCmd.trim()) return;
    try {
      await device.sendRaw(rawCmd.trim());
    } catch (err) {
      console.error('Send failed:', err);
    }
    rawCmd = '';
  }

  function handleKeydown(e) {
    if (e.key === 'Enter') sendRaw();
  }

  $effect(() => {
    // Auto-scroll console
    if (consoleEl && device.consoleLog.length) {
      tick().then(() => {
        consoleEl.scrollTop = consoleEl.scrollHeight;
      });
    }
  });

  function exportLog() {
    const text = device.consoleLog.map(l => `[${l.time}] ${l.text}`).join('\n');
    const blob = new Blob([text], { type: 'text/plain' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `headset-log-${new Date().toISOString().slice(0,19)}.txt`;
    a.click();
    URL.revokeObjectURL(url);
  }

  async function rebootDevice() {
    if (!confirm('Reboot device? Connection will be lost.')) return;
    await device.sendRaw('reset');
  }

  async function enterDFU() {
    if (!confirm('Enter DFU mode on dongle?')) return;
    await device.sendRaw('reset dfu');
  }

  async function readDeviceInfo() {
    await device.sendRaw('get device');
  }
</script>

<div class="panel">
  <div class="toggle-row">
    <label class="field inline">
      <input type="checkbox" bind:checked={showAdvanced}>
      <span>Show Advanced Controls</span>
    </label>
  </div>

  {#if showAdvanced}
    <!-- Live Statistics -->
    <div class="card">
      <h3>Live Statistics</h3>
      <div class="stats-grid">
        <div class="stat">
          <span class="stat-label">Packets TX</span>
          <span class="stat-value">{device.status.packetsTx}</span>
        </div>
        <div class="stat">
          <span class="stat-label">Packets RX</span>
          <span class="stat-value">{device.status.packetsRx}</span>
        </div>
        <div class="stat">
          <span class="stat-label">Packets Lost</span>
          <span class="stat-value">{device.status.packetsLost}</span>
        </div>
        <div class="stat">
          <span class="stat-label">Buffer Underruns</span>
          <span class="stat-value">{device.status.bufferUnderruns}</span>
        </div>
        <div class="stat">
          <span class="stat-label">Buffer Overruns</span>
          <span class="stat-value">{device.status.bufferOverruns}</span>
        </div>
        <div class="stat">
          <span class="stat-label">Codec Errors</span>
          <span class="stat-value">{device.status.codecErrors}</span>
        </div>
      </div>
    </div>

    <!-- Developer Shortcuts -->
    <div class="card">
      <h3>Developer Shortcuts</h3>
      <div class="btn-row">
        <button class="btn" onclick={rebootDevice}
                disabled={device.connectionStatus !== 'connected'}>Reboot Device</button>
        <button class="btn" onclick={enterDFU}
                disabled={device.connectionStatus !== 'connected'}>Enter DFU Mode</button>
        <button class="btn" onclick={readDeviceInfo}
                disabled={device.connectionStatus !== 'connected'}>Read Device Info</button>
      </div>
    </div>
  {/if}

  <!-- Serial Console (always visible) -->
  <div class="card console-card">
    <div class="console-header">
      <h3>Serial Console</h3>
      <button class="btn-small" onclick={exportLog}>Export Log</button>
    </div>
    <div class="console" bind:this={consoleEl}>
      {#each device.consoleLog as line}
        <div class="console-line">
          <span class="console-time">{line.time.slice(11, 23)}</span>
          <span class="console-text">{line.text}</span>
        </div>
      {/each}
      {#if device.consoleLog.length === 0}
        <div class="console-placeholder">No log output yet.</div>
      {/if}
    </div>
    <div class="console-input">
      <input type="text" bind:value={rawCmd} onkeydown={handleKeydown}
             placeholder="Send raw command..."
             disabled={device.connectionStatus !== 'connected'}>
      <button class="btn-small" onclick={sendRaw}
              disabled={device.connectionStatus !== 'connected' || !rawCmd.trim()}>
        Send
      </button>
    </div>
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
  .toggle-row { padding: 0 4px; }
  .field.inline { display: flex; align-items: center; gap: 8px; font-size: 0.88rem; }
  .stats-grid {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 10px;
  }
  .stat {
    background: var(--surface-3);
    border-radius: 6px;
    padding: 10px 14px;
    text-align: center;
  }
  .stat-label { display: block; font-size: 0.72rem; color: var(--text-secondary); text-transform: uppercase; margin-bottom: 4px; }
  .stat-value { display: block; font-size: 1.2rem; font-weight: 600; font-family: 'JetBrains Mono', monospace; }
  .btn-row { display: flex; gap: 8px; flex-wrap: wrap; }
  .btn {
    padding: 8px 18px;
    border-radius: 6px;
    border: 1px solid var(--border);
    background: var(--surface-3);
    color: var(--text-primary);
    font-size: 0.85rem;
    cursor: pointer;
  }
  .btn:disabled { opacity: 0.4; cursor: not-allowed; }
  .btn:hover:not(:disabled) { opacity: 0.85; }
  .console-card { display: flex; flex-direction: column; }
  .console-header { display: flex; justify-content: space-between; align-items: center; }
  .console {
    background: #0a0a0a;
    border-radius: 6px;
    padding: 8px 12px;
    height: 300px;
    overflow-y: auto;
    font-family: 'JetBrains Mono', 'Fira Code', monospace;
    font-size: 0.75rem;
    line-height: 1.5;
  }
  .console-line { display: flex; gap: 8px; }
  .console-time { color: #666; flex-shrink: 0; }
  .console-text { color: #ccc; word-break: break-all; }
  .console-placeholder { color: #444; }
  .console-input {
    display: flex;
    gap: 8px;
    margin-top: 8px;
  }
  .console-input input {
    flex: 1;
    background: var(--surface-3);
    color: var(--text-primary);
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 6px 10px;
    font-family: 'JetBrains Mono', monospace;
    font-size: 0.82rem;
  }
  .btn-small {
    padding: 4px 12px;
    font-size: 0.75rem;
    border-radius: 4px;
    border: 1px solid var(--border);
    background: var(--surface-3);
    color: var(--text-primary);
    cursor: pointer;
  }
  .btn-small:disabled { opacity: 0.4; cursor: not-allowed; }
</style>
