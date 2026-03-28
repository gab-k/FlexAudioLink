<script>
  import { device } from '../stores/device.svelte.js';

  let localConfig = $state(device.getDraft('mode') || {
    deviceRole: device.config.deviceRole,
    operatingMode: device.config.operatingMode,
  });

  let dirty = $state(device.dirtyPanels.has('mode'));

  $effect(() => {
    if (device.configLoaded && !device.dirtyPanels.has('mode')) {
      localConfig = {
        deviceRole: device.config.deviceRole,
        operatingMode: device.config.operatingMode,
      };
    }
  });

  function markDirty() {
    dirty = true;
    device.markDirty('mode');
    device.saveDraft('mode', localConfig);
  }

  function onRoleChange() {
    // USB Audio mode is only valid for headset role
    if (localConfig.deviceRole === 'dongle' && localConfig.operatingMode === 'usb') {
      localConfig.operatingMode = 'proprietary';
    }
    markDirty();
  }

  function onModeChange() {
    markDirty();
  }

  async function apply() {
    try {
      await device.sendCommand('role', localConfig.deviceRole);
      await device.sendCommand('mode', localConfig.operatingMode);
      Object.assign(device.config, localConfig);
      dirty = false;
      device.clearDirty('mode');
      device.showToast('Applied mode settings');
    } catch (err) {
      device.showToast(`Failed to apply: ${err.message}`, 'error', 6000);
    }
  }

  const modeDescriptions = {
    proprietary: 'Low-latency wireless audio over proprietary 2.4 GHz FHSS/TDMA link. Full configurability of PHY rate, payload sizes, codecs, and jitter buffer.',
    ble: 'Bluetooth Low Energy Audio. Codec is locked to LC3 with BLE-specific constraints on sample rates and channels.',
    usb: 'USB sound card mode. Device acts as a wired USB Audio Class device with no wireless link. Radio settings do not apply.',
  };

  const roleDescriptions = {
    dongle: 'Connects to a PC via USB-C. Streams audio to and from the peer device over the wireless link.',
    headset: 'Worn by the user. Receives audio from and sends audio to the peer device. In USB mode, acts as a wired USB audio device.',
  };
</script>

<div class="panel">
  {#if !device.configLoaded}
    <div class="banner info">Connect a device to configure settings.</div>
  {:else}
  <div class="card">
    <h3>Device Role</h3>
    <p class="hint">Determines this device's role in the wireless link. Both devices use the same hardware and firmware.</p>
    <div class="role-grid">
      <button class="role-btn" class:active={localConfig.deviceRole === 'dongle'}
              onclick={() => { localConfig.deviceRole = 'dongle'; onRoleChange(); }}>
        <span class="role-label">Dongle</span>
        <span class="role-desc">{roleDescriptions.dongle}</span>
      </button>
      <button class="role-btn" class:active={localConfig.deviceRole === 'headset'}
              onclick={() => { localConfig.deviceRole = 'headset'; onRoleChange(); }}>
        <span class="role-label">Headset</span>
        <span class="role-desc">{roleDescriptions.headset}</span>
      </button>
    </div>
  </div>

  <div class="card">
    <h3>Operating Mode</h3>
    <p class="hint">Select the audio transport mode. This determines which settings are available.</p>
    <div class="mode-grid">
      <button class="mode-btn" class:active={localConfig.operatingMode === 'proprietary'}
              onclick={() => { localConfig.operatingMode = 'proprietary'; onModeChange(); }}>
        <span class="mode-label">Proprietary 2.4 GHz</span>
        <span class="mode-desc">{modeDescriptions.proprietary}</span>
      </button>
      <button class="mode-btn" class:active={localConfig.operatingMode === 'ble'}
              onclick={() => { localConfig.operatingMode = 'ble'; onModeChange(); }}>
        <span class="mode-label">BLE Audio</span>
        <span class="mode-desc">{modeDescriptions.ble}</span>
      </button>
      <button class="mode-btn" class:active={localConfig.operatingMode === 'usb'}
              onclick={() => { localConfig.operatingMode = 'usb'; onModeChange(); }}
              disabled={localConfig.deviceRole === 'dongle'}>
        <span class="mode-label">USB Audio</span>
        <span class="mode-desc">{modeDescriptions.usb}</span>
        {#if localConfig.deviceRole === 'dongle'}
          <span class="mode-constraint">Headset only</span>
        {/if}
      </button>
    </div>
  </div>

  {#if localConfig.operatingMode === 'usb'}
    <div class="banner info">
      USB mode disables the wireless radio. Audio and Radio settings panels are not applicable.
    </div>
  {/if}
  {#if localConfig.operatingMode === 'ble'}
    <div class="banner info">
      BLE Audio mode locks the codec to LC3 and restricts sample rate and channel options.
    </div>
  {/if}

  <div class="actions">
    <button class="btn primary" onclick={apply}
            disabled={!dirty || device.connectionStatus !== 'connected'}>
      Apply
    </button>
    <div class="banner warning">Changing device role or operating mode will restart the device.</div>
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
    margin: 0 0 6px;
    font-size: 0.85rem;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--text-secondary);
  }
  .hint { font-size: 0.82rem; color: var(--text-secondary); margin: 0 0 12px; }
  .role-grid, .mode-grid {
    display: grid;
    gap: 10px;
  }
  .role-grid { grid-template-columns: 1fr 1fr; }
  .mode-grid { grid-template-columns: 1fr 1fr 1fr; }
  .role-btn, .mode-btn {
    display: flex;
    flex-direction: column;
    gap: 6px;
    padding: 14px 16px;
    border: 2px solid var(--border);
    border-radius: 8px;
    background: var(--surface-1);
    color: var(--text-primary);
    cursor: pointer;
    text-align: left;
    transition: border-color 0.15s, background 0.15s;
  }
  .role-btn:hover, .mode-btn:hover {
    border-color: var(--text-secondary);
  }
  .role-btn.active, .mode-btn.active {
    border-color: var(--color-accent);
    background: rgba(79, 195, 247, 0.08);
  }
  .role-label, .mode-label {
    font-size: 0.95rem;
    font-weight: 600;
  }
  .role-desc, .mode-desc {
    font-size: 0.78rem;
    color: var(--text-secondary);
    line-height: 1.4;
  }
  .mode-constraint {
    font-size: 0.72rem;
    color: var(--color-warning);
    font-style: italic;
  }
  .mode-btn:disabled {
    opacity: 0.4;
    cursor: not-allowed;
  }
  .banner {
    padding: 8px 12px;
    border-radius: 6px;
    font-size: 0.82rem;
  }
  .banner.info {
    background: rgba(79, 195, 247, 0.1);
    color: var(--color-accent);
  }
  .banner.warning {
    background: rgba(255, 193, 7, 0.15);
    color: var(--color-warning);
  }
  .actions {
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
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
