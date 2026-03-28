<script>
  import { device } from '../stores/device.svelte.js';
  import LinkBudget from '../components/LinkBudget.svelte';

  let localConfig = $state(device.getDraft('audio') || { ...device.config });
  let applying = $state(false);
  let dirty = $state(device.dirtyPanels.has('audio'));

  $effect(() => {
    if (device.configLoaded && !device.dirtyPanels.has('audio')) {
      localConfig = { ...device.config };
    }
  });

  function markDirty() {
    dirty = true;
    device.markDirty('audio');
    device.saveDraft('audio', localConfig);
  }

  // Codec constraints
  const isBLE = $derived(device.effectiveOperatingMode === 'ble');
  const spkCodecOptions = $derived(isBLE ? ['lc3'] : ['pcm', 'adpcm', 'lc3', 'opus']);
  const micCodecOptions = $derived(isBLE ? ['lc3'] : ['pcm', 'adpcm', 'lc3', 'opus']);

  const spkBitWidthOptions = $derived(
    localConfig.codecSpk === 'adpcm' ? [16] : [8, 16, 24, 32]
  );
  const micBitWidthOptions = $derived(
    localConfig.codecMic === 'adpcm' ? [16] : [8, 16, 24]
  );

  const lc3SampleRates = [8000, 16000, 24000, 32000, 48000];
  const adpcmSampleRates = [8000, 16000, 24000, 48000];
  const allSampleRates = [8000, 16000, 24000, 48000, 96000];
  const spkSampleRates = $derived(
    localConfig.codecSpk === 'lc3' ? lc3SampleRates :
    localConfig.codecSpk === 'adpcm' ? adpcmSampleRates :
    allSampleRates
  );
  const micSampleRates = $derived(
    localConfig.codecMic === 'lc3' ? lc3SampleRates :
    localConfig.codecMic === 'adpcm' ? adpcmSampleRates :
    allSampleRates
  );

  // Enforce constraints when codec changes
  function onSpkCodecChange() {
    if (localConfig.codecSpk === 'adpcm') {
      localConfig.bitWidthSpk = 16;
      if (localConfig.sampleRateSpk > 48000) localConfig.sampleRateSpk = 48000;
    }
    if (localConfig.codecSpk === 'lc3' && !lc3SampleRates.includes(localConfig.sampleRateSpk)) {
      localConfig.sampleRateSpk = 48000;
    }
    markDirty();
  }
  function onMicCodecChange() {
    if (localConfig.codecMic === 'adpcm') {
      localConfig.bitWidthMic = 16;
      if (localConfig.sampleRateMic > 48000) localConfig.sampleRateMic = 48000;
    }
    if (localConfig.codecMic === 'lc3' && !lc3SampleRates.includes(localConfig.sampleRateMic)) {
      localConfig.sampleRateMic = 48000;
    }
    markDirty();
  }

  // Check if over budget
  const codecRatios = { pcm: 1, adpcm: 4, lc3: 8, opus: 6 };
  const phyBudgets = { 1: 800, 2: 1800, 4: 3800 };
  const chCount = (ch) => ch === 'stereo' ? 2 : 1;
  const totalBitrate = $derived.by(() => {
    const spk = (localConfig.sampleRateSpk * localConfig.bitWidthSpk * chCount(localConfig.channelsSpk)) / (codecRatios[localConfig.codecSpk] || 1) / 1000;
    const mic = (localConfig.sampleRateMic * localConfig.bitWidthMic * chCount(localConfig.channelsMic)) / (codecRatios[localConfig.codecMic] || 1) / 1000;
    return Math.round((spk + mic) * 1.12);
  });
  const overBudget = $derived(totalBitrate > (phyBudgets[localConfig.phyRate] || 3800));

  async function apply() {
    applying = true;
    try {
      const params = [
        ['sample_rate_spk', localConfig.sampleRateSpk],
        ['bit_width_spk', localConfig.bitWidthSpk],
        ['channels_spk', localConfig.channelsSpk],
        ['codec_spk', localConfig.codecSpk],
        ['volume', localConfig.volume],
        ['sidetone', localConfig.sidetone],
        ['sample_rate_mic', localConfig.sampleRateMic],
        ['bit_width_mic', localConfig.bitWidthMic],
        ['channels_mic', localConfig.channelsMic],
        ['codec_mic', localConfig.codecMic],
        ['mic_gain', localConfig.micGain],
        ['mic_mute', localConfig.micMute],
      ];
      for (const [param, value] of params) {
        await device.sendCommand(param, value);
      }
      // Send EQ bands individually — use sendRaw to avoid ack key collision
      // (all bands share the 'eq' param name which would overwrite pending acks)
      for (let i = 0; i < localConfig.eq.length; i++) {
        await device.sendRaw(`set eq ${i} ${localConfig.eq[i].freq} ${localConfig.eq[i].gain}`);
      }
      device.config = { ...device.config, ...localConfig };
      dirty = false;
      device.clearDirty('audio');
      device.showToast(`Applied ${params.length + localConfig.eq.length} audio settings`);
    } catch (err) {
      device.showToast(`Failed to apply: ${err.message}`, 'error', 6000);
    }
    applying = false;
  }
</script>

<div class="panel">
  {#if !device.configLoaded}
    <div class="banner info">Connect a device to configure settings.</div>
  {:else}
  <div class="grid-2">
    <!-- Speaker Settings -->
    <div class="card">
      <h3>Speaker (Headset Output)</h3>

      <label class="field">
        <span>Sample Rate</span>
        <select bind:value={localConfig.sampleRateSpk} onchange={markDirty}>
          {#each spkSampleRates as rate}
            <option value={rate}>{(rate/1000).toFixed(rate % 1000 ? 1 : 0)} kHz</option>
          {/each}
        </select>
      </label>

      <label class="field">
        <span>
          Bit Width
          {#if localConfig.codecSpk === 'adpcm'}
            <span class="info-icon" title="ADPCM encodes 16-bit samples only and works best up to 48 kHz. Higher sample rates and other bit depths are not supported.">i</span>
          {/if}
        </span>
        <select bind:value={localConfig.bitWidthSpk} onchange={markDirty}
                disabled={spkBitWidthOptions.length === 1}>
          {#each spkBitWidthOptions as bw}
            <option value={bw}>{bw}-bit</option>
          {/each}
        </select>
      </label>

      <label class="field">
        <span>Channels</span>
        <select bind:value={localConfig.channelsSpk} onchange={markDirty}>
          <option value="mono">Mono</option>
          <option value="stereo">Stereo</option>
        </select>
      </label>

      <label class="field">
        <span>
          Compression
          {#if isBLE}
            <span class="info-icon" title="In BLE mode, compression is fixed to LC3 as required by the Bluetooth LE Audio standard.">i</span>
          {/if}
        </span>
        <select bind:value={localConfig.codecSpk} onchange={onSpkCodecChange}
                disabled={isBLE}>
          {#each spkCodecOptions as codec}
            <option value={codec}>{codec.toUpperCase()}</option>
          {/each}
        </select>
      </label>

      <label class="field">
        <span>Volume ({localConfig.volume}%)</span>
        <input type="range" min="0" max="100" bind:value={localConfig.volume} oninput={markDirty}>
      </label>

      <label class="field">
        <span>Sidetone ({localConfig.sidetone}%)</span>
        <input type="range" min="0" max="100" bind:value={localConfig.sidetone} oninput={markDirty}>
      </label>
    </div>

    <!-- Mic Settings -->
    <div class="card">
      <h3>Microphone (Headset Input)</h3>

      <label class="field">
        <span>Sample Rate</span>
        <select bind:value={localConfig.sampleRateMic} onchange={markDirty}>
          {#each micSampleRates as rate}
            <option value={rate}>{(rate/1000).toFixed(rate % 1000 ? 1 : 0)} kHz</option>
          {/each}
        </select>
      </label>

      <label class="field">
        <span>
          Bit Width
          {#if localConfig.codecMic === 'adpcm'}
            <span class="info-icon" title="ADPCM encodes 16-bit samples only and works best up to 48 kHz. Higher sample rates and other bit depths are not supported.">i</span>
          {/if}
        </span>
        <select bind:value={localConfig.bitWidthMic} onchange={markDirty}
                disabled={micBitWidthOptions.length === 1}>
          {#each micBitWidthOptions as bw}
            <option value={bw}>{bw}-bit</option>
          {/each}
        </select>
      </label>

      <label class="field">
        <span>Channels</span>
        <select bind:value={localConfig.channelsMic} onchange={markDirty}>
          <option value="mono">Mono</option>
          <option value="stereo">Stereo</option>
        </select>
      </label>

      <label class="field">
        <span>
          Compression
          {#if isBLE}
            <span class="info-icon" title="In BLE mode, compression is fixed to LC3 as required by the Bluetooth LE Audio standard.">i</span>
          {/if}
        </span>
        <select bind:value={localConfig.codecMic} onchange={onMicCodecChange}
                disabled={isBLE}>
          {#each micCodecOptions as codec}
            <option value={codec}>{codec.toUpperCase()}</option>
          {/each}
        </select>
      </label>

      <label class="field">
        <span>Mic Gain ({localConfig.micGain} dB)</span>
        <input type="range" min="0" max="24" step="6" bind:value={localConfig.micGain} oninput={markDirty}>
      </label>

      <label class="field inline">
        <input type="checkbox" bind:checked={localConfig.micMute} onchange={markDirty}>
        <span>Mute Microphone</span>
      </label>
    </div>
  </div>

  <!-- EQ -->
  <div class="card">
    <h3>Equalizer (5-Band)</h3>
    <div class="eq-container">
      {#each localConfig.eq as band, i}
        <div class="eq-band">
          <input type="range" min="-12" max="12" step="1"
            bind:value={localConfig.eq[i].gain} oninput={markDirty}
            orient="vertical" class="eq-slider">
          <span class="eq-val">{band.gain > 0 ? '+' : ''}{band.gain}</span>
          <span class="eq-freq">{band.freq >= 1000 ? `${band.freq/1000}k` : band.freq}</span>
        </div>
      {/each}
    </div>
  </div>

  <!-- Link Budget -->
  <LinkBudget config={localConfig} />

  <!-- Apply -->
  <div class="apply-row">
    <button class="btn primary" onclick={apply}
            disabled={!dirty || applying || overBudget || device.connectionStatus !== 'connected'}>
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
  .grid-2 { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
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
  .field span { flex-shrink: 0; }
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
  .field select, .field input[type="range"] { flex: 1; max-width: 200px; }
  select {
    background: var(--surface-3);
    color: var(--text-primary);
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 4px 8px;
    font-size: 0.85rem;
  }
  select:disabled { opacity: 0.4; }
  input[type="range"] { accent-color: var(--color-accent); }
  .field.inline {
    justify-content: flex-start;
    gap: 8px;
  }
  .eq-container {
    display: flex;
    justify-content: space-around;
    align-items: flex-end;
    gap: 12px;
    padding: 8px 0;
  }
  .eq-band {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 4px;
  }
  .eq-slider {
    writing-mode: vertical-lr;
    direction: rtl;
    height: 100px;
    width: 28px;
  }
  .eq-val {
    font-size: 0.75rem;
    font-family: 'JetBrains Mono', monospace;
    color: var(--text-secondary);
  }
  .eq-freq {
    font-size: 0.72rem;
    color: var(--text-secondary);
  }
  .apply-row {
    display: flex;
    align-items: center;
    gap: 12px;
    padding-top: 4px;
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
  .btn.primary {
    background: var(--color-accent);
    border-color: var(--color-accent);
    color: #fff;
  }
  .btn:disabled { opacity: 0.4; cursor: not-allowed; }
  .unsaved-hint {
    font-size: 0.8rem;
    color: var(--color-warning);
  }
  .banner.info {
    background: rgba(79, 195, 247, 0.12);
    color: var(--color-accent);
    padding: 8px 12px;
    border-radius: 6px;
    font-size: 0.82rem;
  }
</style>
