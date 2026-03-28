<script>
  import { device } from '../stores/device.svelte.js';

  const defaultDeviceConfig = () => ({
    audioIo: device.config.audioIo,
    deviceAddr: device.config.deviceAddr,
    peerAddr: device.config.peerAddr,
    autoSleep: device.config.autoSleep,
    lowBatteryThreshold: device.config.lowBatteryThreshold,
  });

  let localConfig = $state(device.getDraft('device') || defaultDeviceConfig());
  let dirty = $state(device.dirtyPanels.has('device'));
  let applying = $state(false);
  let importError = $state(null);

  $effect(() => {
    if (device.configLoaded && !device.dirtyPanels.has('device')) {
      localConfig = defaultDeviceConfig();
    }
  });

  function markDirty() {
    dirty = true;
    device.markDirty('device');
    device.saveDraft('device', localConfig);
  }

  async function apply() {
    applying = true;
    try {
      for (const [key, val] of Object.entries(localConfig)) {
        const param = key.replace(/([A-Z])/g, '_$1').toLowerCase();
        await device.sendCommand(param, val);
      }
      const count = Object.keys(localConfig).length;
      Object.assign(device.config, localConfig);
      dirty = false;
      device.clearDirty('device');
      device.showToast(`Applied ${count} device settings`);
    } catch (err) {
      device.showToast(`Failed to apply: ${err.message}`, 'error', 6000);
    }
    applying = false;
  }

  async function saveToDevice() {
    try {
      await device.sendRaw('save_config');
      device.showToast('Configuration saved to device');
    } catch (err) {
      device.showToast(`Save failed: ${err.message}`, 'error', 6000);
    }
  }

  async function loadFromDevice() {
    try {
      await device.sendRaw('load_config');
      await device.sendRaw('get all');
      device.showToast('Configuration loaded from device');
    } catch (err) {
      device.showToast(`Load failed: ${err.message}`, 'error', 6000);
    }
  }

  function exportConfig() {
    const blob = new Blob([JSON.stringify(device.config, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `flexaudiolink-config-${new Date().toISOString().slice(0,10)}.json`;
    a.click();
    URL.revokeObjectURL(url);
  }

  function importConfig() {
    importError = null;
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.json';
    input.onchange = async (e) => {
      const file = e.target.files[0];
      if (!file) return;
      const text = await file.text();
      let imported;
      try {
        imported = JSON.parse(text);
      } catch {
        importError = 'Invalid JSON file.';
        return;
      }
      if (typeof imported !== 'object' || imported === null || Array.isArray(imported)) {
        importError = 'Config file must be a JSON object.';
        return;
      }
      const errors = device.validateConfig(imported);
      if (errors.length > 0) {
        importError = errors.join('; ');
        return;
      }
      device.config = { ...device.config, ...imported };
      localConfig = defaultDeviceConfig();
      importError = null;
    };
    input.click();
  }

</script>

<div class="panel">
  {#if !device.configLoaded}
    <div class="banner info">Connect a device to configure settings.</div>
  {:else}
  <div class="card">
    <h3>Device Behavior</h3>
      <label class="field">
        <span>Auto-Sleep Timeout</span>
        <select bind:value={localConfig.autoSleep} onchange={markDirty}>
          <option value={0}>Off</option>
          <option value={5}>5 min</option>
          <option value={10}>10 min</option>
          <option value={15}>15 min</option>
          <option value={30}>30 min</option>
          <option value={60}>60 min</option>
        </select>
      </label>

      <label class="field">
        <span>Low Battery Warning</span>
        <select bind:value={localConfig.lowBatteryThreshold} onchange={markDirty}>
          <option value={5}>5%</option>
          <option value={10}>10%</option>
          <option value={15}>15%</option>
          <option value={20}>20%</option>
        </select>
      </label>
  </div>

  <div class="card">
    <h3>Audio I/O</h3>
    <p class="hint">Physical audio interface for this device. Set to CODEC on both devices for an analog-to-analog wireless bridge.</p>
    <label class="field">
      <span>Audio Interface</span>
      <select bind:value={localConfig.audioIo} onchange={markDirty}>
        <option value="usb">USB Audio</option>
        <option value="codec">CODEC (Analog/I2S)</option>
      </select>
    </label>
  </div>

  <div class="card">
    <h3>Radio Addressing</h3>
    <p class="hint">Proprietary 2.4 GHz address configuration. Both devices in a pair must have matching addresses. Default addresses work out of the box for a single pair.</p>
    <label class="field">
      <span>Device Address</span>
      <input type="text" class="addr-input" bind:value={localConfig.deviceAddr} oninput={markDirty}>
    </label>
    <label class="field">
      <span>Peer Address</span>
      <input type="text" class="addr-input" bind:value={localConfig.peerAddr} oninput={markDirty}>
    </label>
    <p class="hint">For multiple pairs in the same room, use unique address pairs to avoid crosstalk.</p>
  </div>

  <div class="card">
    <h3>Configuration Persistence</h3>
    <div class="btn-row">
      <button class="btn" onclick={saveToDevice}
              disabled={device.connectionStatus !== 'connected'}>
        Save to Device
      </button>
      <button class="btn" onclick={loadFromDevice}
              disabled={device.connectionStatus !== 'connected'}>
        Load from Device
      </button>
      <button class="btn" onclick={exportConfig}>Export to File</button>
      <button class="btn" onclick={importConfig}>Import from File</button>
    </div>
    {#if importError}
      <p class="import-error">{importError}</p>
    {/if}
  </div>

  <div class="apply-row">
    <button class="btn primary" onclick={apply}
            disabled={!dirty || applying || device.connectionStatus !== 'connected'}>
      {applying ? 'Applying...' : 'Apply'}
    </button>
    {#if dirty}
      <span class="unsaved-hint">Unsaved changes</span>
    {/if}
  </div>
  {/if}
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
  .field {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 5px 0;
    font-size: 0.88rem;
    gap: 12px;
  }
  .field select { flex: 1; max-width: 200px; }
  select {
    background: var(--surface-3);
    color: var(--text-primary);
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 4px 8px;
    font-size: 0.85rem;
  }
  .addr-input {
    background: var(--surface-3);
    color: var(--text-primary);
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 4px 8px;
    font-size: 0.85rem;
    font-family: 'JetBrains Mono', monospace;
    width: 130px;
  }
  .addr-input:disabled { opacity: 0.5; }
  .hint { font-size: 0.78rem; color: var(--text-secondary); margin: 4px 0; }
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
  .btn.primary { background: var(--color-accent); border-color: var(--color-accent); color: #fff; }
  .btn:disabled { opacity: 0.4; cursor: not-allowed; }
  .btn:hover:not(:disabled) { opacity: 0.85; }
  .apply-row { display: flex; align-items: center; gap: 12px; }
  .unsaved-hint { font-size: 0.8rem; color: var(--color-warning); }
  .import-error {
    margin: 8px 0 0;
    padding: 6px 10px;
    background: rgba(244, 67, 54, 0.12);
    border-radius: 4px;
    color: var(--color-danger);
    font-size: 0.82rem;
  }
  .banner.info {
    background: rgba(79, 195, 247, 0.12);
    color: var(--color-accent);
    padding: 8px 12px;
    border-radius: 6px;
    font-size: 0.82rem;
  }
</style>
