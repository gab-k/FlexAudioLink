<script>
  import { device } from './lib/stores/device.svelte.js';
  import { supportsWebSerial } from './lib/transport/transport.js';
  import StatusBar from './lib/components/StatusBar.svelte';
  import ModePanel from './lib/panels/ModePanel.svelte';
  import StatusPanel from './lib/panels/StatusPanel.svelte';
  import AudioPanel from './lib/panels/AudioPanel.svelte';
  import RadioPanel from './lib/panels/RadioPanel.svelte';
  import DevicePanel from './lib/panels/DevicePanel.svelte';
  import FirmwarePanel from './lib/panels/FirmwarePanel.svelte';
  import AdvancedPanel from './lib/panels/AdvancedPanel.svelte';

  const tabs = [
    { id: 'mode', label: 'Mode' },
    { id: 'status', label: 'Status & Monitor' },
    { id: 'audio', label: 'Audio Settings' },
    { id: 'radio', label: 'Radio Settings' },
    { id: 'device', label: 'Device Settings' },
    { id: 'firmware', label: 'Firmware Update' },
    { id: 'advanced', label: 'Advanced' },
  ];

  let activeTab = $state('mode');
  let showBridgeInfo = $state(false);

  const hasWebSerial = supportsWebSerial();

  function tabLabel(tab) {
    const dirty = device.dirtyPanels.has(tab.id);
    return dirty ? `${tab.label} *` : tab.label;
  }
</script>

<div class="app">
  <header class="header">
    <div class="header-left">
      <h1>FlexAudioLink</h1>
      <span class="subtitle">Wireless Headset Configuration</span>
    </div>
    <div class="header-right">
      {#if !hasWebSerial}
        <button class="btn-small" onclick={() => showBridgeInfo = !showBridgeInfo}>
          WebSocket Mode
        </button>
      {/if}
    </div>
  </header>

  {#if device.demoMode}
    <div class="demo-banner">
      <span>DEMO MODE — No device connected. Values are simulated.</span>
      <button class="btn-small" onclick={() => device.disconnect()}>Exit Demo</button>
    </div>
  {/if}

  {#if showBridgeInfo}
    <div class="bridge-banner">
      <p>Your browser doesn't support WebSerial. Install the Python bridge:</p>
      <code>pip install pyserial websockets && python websocket_serial_bridge.py</code>
      <p>The app will connect via <code>ws://localhost:8765</code></p>
      <button class="btn-small" onclick={() => showBridgeInfo = false}>Dismiss</button>
    </div>
  {/if}

  {#if device.connectionStatus === 'error'}
    <div class="reconnect-banner">
      {#if device.connectionError}
        {device.connectionError}
      {:else}
        Connection lost.
      {/if}
      <button class="btn-link" onclick={() => device.connect()}>Retry</button>
    </div>
  {/if}

  <nav class="tabs">
    {#each tabs as tab}
      <button class="tab" class:active={activeTab === tab.id}
              onclick={() => activeTab = tab.id}>
        {tabLabel(tab)}
      </button>
    {/each}
  </nav>

  <main class="content">
    {#if activeTab === 'mode'}
      <ModePanel />
    {:else if activeTab === 'status'}
      <StatusPanel />
    {:else if activeTab === 'audio'}
      <AudioPanel />
    {:else if activeTab === 'radio'}
      <RadioPanel />
    {:else if activeTab === 'device'}
      <DevicePanel />
    {:else if activeTab === 'firmware'}
      <FirmwarePanel />
    {:else if activeTab === 'advanced'}
      <AdvancedPanel />
    {/if}
  </main>

  <StatusBar />
</div>

<style>
  .app {
    display: flex;
    flex-direction: column;
    height: 100vh;
    overflow: hidden;
  }
  .header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 12px 20px;
    background: var(--surface-1);
    border-bottom: 1px solid var(--border);
  }
  .header-left { display: flex; align-items: baseline; gap: 12px; }
  .header h1 {
    margin: 0;
    font-size: 1.2rem;
    font-weight: 700;
    color: var(--color-accent);
  }
  .subtitle {
    font-size: 0.82rem;
    color: var(--text-secondary);
  }
  .tabs {
    display: flex;
    gap: 0;
    background: var(--surface-1);
    border-bottom: 2px solid var(--border);
    padding: 0 16px;
    overflow-x: auto;
  }
  .tab {
    padding: 10px 18px;
    border: none;
    background: none;
    color: var(--text-secondary);
    font-size: 0.85rem;
    cursor: pointer;
    border-bottom: 2px solid transparent;
    margin-bottom: -2px;
    white-space: nowrap;
    transition: color 0.15s, border-color 0.15s;
  }
  .tab:hover { color: var(--text-primary); }
  .tab.active {
    color: var(--color-accent);
    border-bottom-color: var(--color-accent);
  }
  .content {
    flex: 1;
    overflow-y: auto;
    padding: 16px 20px;
  }
  .bridge-banner {
    background: rgba(79, 195, 247, 0.1);
    border-bottom: 1px solid var(--border);
    padding: 10px 20px;
    font-size: 0.82rem;
  }
  .bridge-banner code {
    display: block;
    background: var(--surface-3);
    padding: 6px 10px;
    border-radius: 4px;
    margin: 6px 0;
    font-size: 0.8rem;
  }
  .reconnect-banner {
    background: rgba(244, 67, 54, 0.12);
    color: var(--color-danger);
    padding: 8px 20px;
    font-size: 0.85rem;
    text-align: center;
  }
  .btn-link {
    background: none;
    border: none;
    color: var(--color-accent);
    cursor: pointer;
    text-decoration: underline;
    font-size: inherit;
  }
  .btn-small {
    padding: 4px 12px;
    font-size: 0.75rem;
    border-radius: 4px;
    border: 1px solid var(--border);
    background: var(--surface-2);
    color: var(--text-primary);
    cursor: pointer;
  }
  .demo-banner {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 16px;
    padding: 8px 20px;
    background: rgba(255, 193, 7, 0.18);
    border-bottom: 2px solid var(--color-warning);
    color: var(--color-warning);
    font-size: 0.85rem;
    font-weight: 600;
    letter-spacing: 0.02em;
  }
  .demo-banner .btn-small {
    background: transparent;
    border-color: var(--color-warning);
    color: var(--color-warning);
    font-weight: 600;
  }
</style>
