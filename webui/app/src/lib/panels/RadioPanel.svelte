<script>
  import { device } from '../stores/device.svelte.js';
  import LinkBudget from '../components/LinkBudget.svelte';

  const defaultRadioConfig = () => ({
    phyRate: device.config.phyRate,
    txPower: device.config.txPower,
    payloadMsDl: device.config.payloadMsDl,
    payloadMsUl: device.config.payloadMsUl,
    jitterBufferMs: device.config.jitterBufferMs,
  });

  let localConfig = $state(device.getDraft('radio') || defaultRadioConfig());

  $effect(() => {
    if (device.configLoaded && !device.dirtyPanels.has('radio')) {
      localConfig = defaultRadioConfig();
    }
  });

  // Hardware payload limit: 252 bytes (8-bit LENGTH field max 255 - 2B seq - 1B len)
  const MAX_PAYLOAD_BYTES = 252;

  const CODEC_RATIOS = { pcm: 1, adpcm: 4, lc3: 8, opus: 6 };
  const CODEC_FRAME_MS = { pcm: 0, adpcm: 0, lc3: 10, opus: 10 };
  const chCount = (ch) => ch === 'stereo' ? 2 : 1;

  // Jitter buffer minimum: must hold at least one codec frame from either direction
  const minJitterBufferMs = $derived(
    Math.max(CODEC_FRAME_MS[device.config.codecSpk] || 0, CODEC_FRAME_MS[device.config.codecMic] || 0) || 1
  );

  // DL (speaker) bytes per ms
  const dlBytesPerMs = $derived(
    (device.config.sampleRateSpk * (device.config.bitWidthSpk / 8) * chCount(device.config.channelsSpk))
    / (CODEC_RATIOS[device.config.codecSpk] || 1)
    / 1000
  );
  const dlPayloadBytes = $derived(Math.round(localConfig.payloadMsDl * dlBytesPerMs));
  const dlExceedsHw = $derived(dlPayloadBytes > MAX_PAYLOAD_BYTES);
  const maxDlMs = $derived(dlBytesPerMs > 0 ? MAX_PAYLOAD_BYTES / dlBytesPerMs : 50);

  // UL (mic) bytes per ms
  const ulBytesPerMs = $derived(
    (device.config.sampleRateMic * (device.config.bitWidthMic / 8) * chCount(device.config.channelsMic))
    / (CODEC_RATIOS[device.config.codecMic] || 1)
    / 1000
  );
  const ulPayloadBytes = $derived(Math.round(localConfig.payloadMsUl * ulBytesPerMs));
  const ulExceedsHw = $derived(ulPayloadBytes > MAX_PAYLOAD_BYTES);
  const maxUlMs = $derived(ulBytesPerMs > 0 ? MAX_PAYLOAD_BYTES / ulBytesPerMs : 50);
  let dirty = $state(device.dirtyPanels.has('radio'));
  let applying = $state(false);

  const isProp = $derived(
    device.effectiveOperatingMode === 'prop_dongle' ||
    device.effectiveOperatingMode === 'prop_headset'
  );
  const txPowerOptions = [-20, -16, -12, -8, -4, 0, 4, 8];

  function markDirty() {
    dirty = true;
    device.markDirty('radio');
    device.saveDraft('radio', localConfig);
  }

  function onJitterBufferChange() {
    // Enforce minimum (codec frame size)
    if (localConfig.jitterBufferMs < minJitterBufferMs) {
      localConfig.jitterBufferMs = minJitterBufferMs;
    }
    // Clamp payload sizes to not exceed the new jitter buffer
    if (localConfig.payloadMsDl > localConfig.jitterBufferMs) {
      localConfig.payloadMsDl = localConfig.jitterBufferMs;
    }
    if (localConfig.payloadMsUl > localConfig.jitterBufferMs) {
      localConfig.payloadMsUl = localConfig.jitterBufferMs;
    }
    markDirty();
  }

  // Merged config for LinkBudget: audio settings from device + local radio overrides
  const mergedConfig = $derived({ ...device.config, ...localConfig });

  async function apply() {
    applying = true;
    try {
      if (isProp) {
        await device.sendCommand('phy_rate', localConfig.phyRate);
        await device.sendCommand('tx_power', localConfig.txPower);
        await device.sendCommand('payload_ms_dl', localConfig.payloadMsDl);
        await device.sendCommand('payload_ms_ul', localConfig.payloadMsUl);
        await device.sendCommand('jitter_buffer_ms', localConfig.jitterBufferMs);
      }
      Object.assign(device.config, localConfig);
      dirty = false;
      device.clearDirty('radio');
      device.showToast('Applied radio settings');
    } catch (err) {
      device.showToast(`Failed to apply: ${err.message}`, 'error', 6000);
    }
    applying = false;
  }
</script>

<div class="panel">
  {#if !device.configLoaded}
    <div class="banner info">Connect a device to configure settings.</div>
  {:else if device.effectiveOperatingMode === 'usb'}
    <div class="banner info">USB Audio mode is active. Radio settings do not apply. Change the operating mode in the Mode tab.</div>
  {:else if isProp}
    <div class="card">
      <h3>PHY & Power</h3>
        <label class="field">
          <span>PHY Data Rate</span>
          <select bind:value={localConfig.phyRate} onchange={markDirty}>
            <option value={1}>1 Mbps</option>
            <option value={2}>2 Mbps</option>
            <option value={4}>4 Mbps</option>
          </select>
        </label>
        <p class="hint">Changing PHY rate requires reconnection.</p>

        <label class="field">
          <span>TX Power</span>
          <select bind:value={localConfig.txPower} onchange={markDirty}>
            {#each txPowerOptions as pwr}
              <option value={pwr}>{pwr > 0 ? '+' : ''}{pwr} dBm</option>
            {/each}
          </select>
        </label>

        <label class="field">
          <span>
            Jitter Buffer ({localConfig.jitterBufferMs} ms)
            {#if minJitterBufferMs > 1}
              <span class="info-icon" title="Minimum {minJitterBufferMs} ms — the jitter buffer must hold at least one full codec frame to be decodable.">i</span>
            {/if}
          </span>
          <input type="range" min={minJitterBufferMs} max="50" step="0.5"
                 bind:value={localConfig.jitterBufferMs} oninput={onJitterBufferChange}>
        </label>
        <p class="hint">Buffer capacity (min {minJitterBufferMs} ms). Estimated latency = {(localConfig.jitterBufferMs / 2).toFixed(1)} ms (target fill: 50%).</p>

        <label class="field">
          <span>
            DL Payload ({localConfig.payloadMsDl} ms)
            <span class="info-icon" title="Payload size must not exceed the jitter buffer. A payload larger than the buffer cannot be absorbed without overflow, causing guaranteed packet loss.">i</span>
          </span>
          <input type="range" min="0.5" max={Math.min(maxDlMs, localConfig.jitterBufferMs)} step="0.5"
                 bind:value={localConfig.payloadMsDl} oninput={markDirty}>
        </label>
        <p class="hint">
          Speaker downlink: {dlPayloadBytes} bytes/pkt (hw max {MAX_PAYLOAD_BYTES} B)
          {#if dlExceedsHw}<span class="hint-warn"> — exceeds hardware limit!</span>{/if}
        </p>

        <label class="field">
          <span>
            UL Payload ({localConfig.payloadMsUl} ms)
            <span class="info-icon" title="Payload size must not exceed the jitter buffer. A payload larger than the buffer cannot be absorbed without overflow, causing guaranteed packet loss.">i</span>
          </span>
          <input type="range" min="0.5" max={Math.min(maxUlMs, localConfig.jitterBufferMs)} step="0.5"
                 bind:value={localConfig.payloadMsUl} oninput={markDirty}>
        </label>
        <p class="hint">
          Mic uplink: {ulPayloadBytes} bytes/pkt (hw max {MAX_PAYLOAD_BYTES} B)
          {#if ulExceedsHw}<span class="hint-warn"> — exceeds hardware limit!</span>{/if}
        </p>
    </div>
  {/if}

  {#if device.configLoaded && isProp}
    <LinkBudget config={mergedConfig} />
  {/if}

  {#if device.configLoaded && isProp}
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
  .field select, .field input[type="range"] { flex: 1; max-width: 200px; }
  select {
    background: var(--surface-3);
    color: var(--text-primary);
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 4px 8px;
    font-size: 0.85rem;
  }
  .hint {
    font-size: 0.78rem;
    color: var(--text-secondary);
    margin: 4px 0;
  }
  .hint-warn {
    color: var(--color-danger);
    font-weight: 600;
  }
  .info-icon {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 15px;
    height: 15px;
    border-radius: 50%;
    background: var(--color-accent);
    color: #fff;
    font-size: 0.62rem;
    font-weight: 700;
    font-style: italic;
    cursor: help;
    margin-left: 4px;
    vertical-align: middle;
  }
  .banner.info {
    background: rgba(79, 195, 247, 0.12);
    color: var(--color-accent);
    padding: 8px 12px;
    border-radius: 6px;
    font-size: 0.82rem;
  }
  .apply-row {
    display: flex;
    align-items: center;
    gap: 12px;
  }
  .btn {
    padding: 8px 24px;
    border-radius: 6px;
    border: 1px solid var(--border);
    background: var(--surface-2);
    color: var(--text-primary);
    font-size: 0.88rem;
    cursor: pointer;
  }
  .btn.primary { background: var(--color-accent); border-color: var(--color-accent); color: #fff; }
  .btn:disabled { opacity: 0.4; cursor: not-allowed; }
  .unsaved-hint { font-size: 0.8rem; color: var(--color-warning); }
</style>
