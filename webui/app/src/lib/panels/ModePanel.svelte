<script>
  import { device } from '../stores/device.svelte.js';

  let localConfig = $state(device.getDraft('mode') || {
    operatingMode: device.config.operatingMode,
  });

  let dirty = $state(device.dirtyPanels.has('mode'));

  $effect(() => {
    if (device.configLoaded && !device.dirtyPanels.has('mode')) {
      localConfig = {
        operatingMode: device.config.operatingMode,
      };
    }
  });

  function markDirty() {
    dirty = true;
    device.markDirty('mode');
    device.saveDraft('mode', localConfig);
  }

  function selectMode(mode) {
    localConfig.operatingMode = mode;
    markDirty();
  }

  async function apply() {
    try {
      await device.sendCommand('mode', localConfig.operatingMode);
      Object.assign(device.config, localConfig);
      dirty = false;
      device.clearDirty('mode');
      device.showToast('Applied mode settings');
    } catch (err) {
      device.showToast(`Failed to apply: ${err.message}`, 'error', 6000);
    }
  }

  const modeOptions = [
    {
      value: 'usb',
      label: 'USB Audio',
      desc: 'Direct wired USB Audio Class device. The radio path is not started.',
    },
    {
      value: 'pfsk_dongle',
      label: 'PFSK Dongle',
      desc: 'USB audio bridge for the PC side of the proprietary 2.4 GHz link.',
    },
    {
      value: 'pfsk_headset',
      label: 'PFSK Headset',
      desc: 'Codec/I2S audio bridge for the headset side of the proprietary 2.4 GHz link.',
    },
  ];
</script>

<div class="panel">
  {#if !device.configLoaded}
    <div class="banner info">Connect a device to configure settings.</div>
  {:else}
  <div class="card">
    <h3>Boot Mode</h3>
    <p class="hint">Select the persisted firmware mode that starts on boot.</p>
    <div class="mode-grid">
      {#each modeOptions as option}
        <button class="mode-btn" class:active={localConfig.operatingMode === option.value}
                onclick={() => selectMode(option.value)}>
          <span class="mode-label">{option.label}</span>
          <span class="mode-value">{option.value}</span>
          <span class="mode-desc">{option.desc}</span>
        </button>
      {/each}
    </div>
  </div>

  {#if localConfig.operatingMode === 'usb'}
    <div class="banner info">
      USB mode disables the wireless radio. Audio and Radio settings panels are not applicable.
    </div>
  {/if}
  {#if localConfig.operatingMode === 'pfsk_dongle' || localConfig.operatingMode === 'pfsk_headset'}
    <div class="banner info">
      PFSK modes use the proprietary 2.4 GHz path and the role is part of the selected boot mode.
    </div>
  {/if}

  <div class="actions">
    <button class="btn primary" onclick={apply}
            disabled={!dirty || device.connectionStatus !== 'connected'}>
      Apply
    </button>
    <div class="banner warning">Changing boot mode restarts the device.</div>
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
  .mode-grid {
    display: grid;
    gap: 10px;
  }
  .mode-grid { grid-template-columns: repeat(3, minmax(0, 1fr)); }
  .mode-btn {
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
  .mode-btn:hover {
    border-color: var(--text-secondary);
  }
  .mode-btn.active {
    border-color: var(--color-accent);
    background: rgba(79, 195, 247, 0.08);
  }
  .mode-label {
    font-size: 0.95rem;
    font-weight: 600;
  }
  .mode-value {
    font-family: 'JetBrains Mono', monospace;
    font-size: 0.72rem;
    color: var(--color-accent);
  }
  .mode-desc {
    font-size: 0.78rem;
    color: var(--text-secondary);
    line-height: 1.4;
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
