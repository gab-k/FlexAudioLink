<script>
  let { config, showWarningBanner = true } = $props();

  const CODEC_RATIOS = { pcm: 1, adpcm: 4, lc3: 8, opus: 6 };
  const PHY_BUDGETS = { 1: 800, 2: 1800, 4: 3800 }; // usable kbps per PHY rate

  const channelCount = (ch) => ch === 'stereo' ? 2 : 1;

  const spkBitrate = $derived(
    (config.sampleRateSpk * config.bitWidthSpk * channelCount(config.channelsSpk))
    / (CODEC_RATIOS[config.codecSpk] || 1)
    / 1000
  );

  const micBitrate = $derived(
    (config.sampleRateMic * config.bitWidthMic * channelCount(config.channelsMic))
    / (CODEC_RATIOS[config.codecMic] || 1)
    / 1000
  );

  const overhead = $derived(Math.round((spkBitrate + micBitrate) * 0.12));
  const totalBitrate = $derived(Math.round(spkBitrate + micBitrate + overhead));
  const phyBudget = $derived(PHY_BUDGETS[config.phyRate] || 3800);
  const headroom = $derived(phyBudget - totalBitrate);
  const usagePercent = $derived(Math.round((totalBitrate / phyBudget) * 100));

  const barColor = $derived(
    usagePercent > 90 ? 'var(--color-danger)' :
    usagePercent > 70 ? 'var(--color-warning)' :
    'var(--color-success)'
  );

  const overBudget = $derived(totalBitrate > phyBudget);
  const lowHeadroom = $derived(usagePercent > 80 && !overBudget);

  // --- Latency estimate ---

  // Codec frame accumulation — must collect a full frame before encoding
  // PCM/ADPCM: sample-level, no frame accumulation needed
  // LC3/Opus: frame-based, must buffer one full frame
  const CODEC_FRAME_MS = { pcm: 0, adpcm: 0, lc3: 10, opus: 10 };

  // TDMA frame period (ms) — scales with PHY rate
  const TDMA_FRAME_MS = { 1: 10, 2: 5, 4: 2.5 };
  const tdmaFrameMs = $derived(TDMA_FRAME_MS[config.phyRate] || 2.5);

  // TDMA slot allocation — divide frame between speaker (DL) and mic (UL)
  // proportional to their bitrates
  const totalAudioBitrate = $derived(spkBitrate + micBitrate);
  const spkSlotFraction = $derived(totalAudioBitrate > 0 ? spkBitrate / totalAudioBitrate : 0.5);
  const micSlotFraction = $derived(1 - spkSlotFraction);
  const spkSlotMs = $derived(tdmaFrameMs * spkSlotFraction);
  const micSlotMs = $derived(tdmaFrameMs * micSlotFraction);

  // Jitter buffer: capacity in ms from config, latency = half (target fill state = 50%)
  const jitterBufferCapacityMs = $derived(config.jitterBufferMs || 10);
  const jitterLatencyMs = $derived(jitterBufferCapacityMs / 2);

  // Speaker path: codec frame accumulation + jitter buffer latency on headset
  const codecFrameSpk = $derived(CODEC_FRAME_MS[config.codecSpk] || 0);
  const latencySpkTotal = $derived(codecFrameSpk + jitterLatencyMs);

  // Mic path: codec frame accumulation + jitter buffer latency on dongle
  const codecFrameMic = $derived(CODEC_FRAME_MS[config.codecMic] || 0);
  const latencyMicTotal = $derived(codecFrameMic + jitterLatencyMs);

  function latencyColor(ms) {
    if (ms < 8) return 'var(--color-success)';
    if (ms < 20) return 'var(--color-accent)';
    if (ms < 35) return 'var(--color-warning)';
    return 'var(--color-danger)';
  }
</script>

<div class="link-budget">
  <h4>Link Budget</h4>
  <div class="budget-breakdown">
    <div class="budget-row">
      <span>Speaker</span>
      <span>{config.sampleRateSpk} Hz x {config.bitWidthSpk}b x {channelCount(config.channelsSpk)}ch / {CODEC_RATIOS[config.codecSpk]}x</span>
      <span class="value">{Math.round(spkBitrate)} kbps</span>
    </div>
    <div class="budget-row">
      <span>Mic</span>
      <span>{config.sampleRateMic} Hz x {config.bitWidthMic}b x {channelCount(config.channelsMic)}ch / {CODEC_RATIOS[config.codecMic]}x</span>
      <span class="value">{Math.round(micBitrate)} kbps</span>
    </div>
    <div class="budget-row">
      <span>Overhead (~12%)</span>
      <span></span>
      <span class="value">~{overhead} kbps</span>
    </div>
    <hr>
    <div class="budget-row total">
      <span>Total</span>
      <span></span>
      <span class="value">{totalBitrate} kbps</span>
    </div>
  </div>

  <div class="budget-bar-container">
    <div class="budget-bar">
      <div class="budget-bar-fill" style="width: {Math.min(usagePercent, 100)}%; background: {barColor}"></div>
    </div>
    <div class="budget-bar-label">
      {totalBitrate} / {phyBudget} kbps ({usagePercent}%)
      {#if overBudget}
        <span class="badge danger">OVER BUDGET</span>
      {:else if usagePercent > 80}
        <span class="badge warning">LOW HEADROOM</span>
      {:else}
        <span class="badge success">OK</span>
      {/if}
    </div>
  </div>

  <!-- TDMA Slot Allocation -->
  <div class="latency-section">
    <h4>TDMA Frame ({tdmaFrameMs} ms @ {config.phyRate} Mbps)</h4>
    <div class="tdma-frame">
      <div class="tdma-slot dl" style="flex: {spkSlotFraction}" title="Downlink (Speaker): {spkSlotMs.toFixed(2)} ms">
        <span class="stage-label">DL (Speaker)</span>
        <span class="stage-value">{spkSlotMs.toFixed(1)} ms</span>
      </div>
      <div class="tdma-slot ul" style="flex: {micSlotFraction}" title="Uplink (Mic): {micSlotMs.toFixed(2)} ms">
        <span class="stage-label">UL (Mic)</span>
        <span class="stage-value">{micSlotMs.toFixed(1)} ms</span>
      </div>
    </div>
    <div class="tdma-legend">
      <span>Slot split by bitrate ratio: {Math.round(spkSlotFraction * 100)}% DL / {Math.round(micSlotFraction * 100)}% UL</span>
    </div>
  </div>

  <!-- Latency Estimate -->
  <div class="latency-section">
    <h4>Latency Estimate</h4>
    <div class="latency-paths">
      <!-- Speaker path -->
      <div class="latency-path">
        <div class="latency-path-header">
          <span class="latency-path-label">Speaker (PC → Ear)</span>
          <span class="latency-total" style="color: {latencyColor(latencySpkTotal)}">{latencySpkTotal.toFixed(1)} ms</span>
        </div>
        <div class="latency-pipeline">
          {#if codecFrameSpk > 0}
            <div class="latency-stage codec" style="flex: {codecFrameSpk}"
                 title="{config.codecSpk.toUpperCase()} frame accumulation: {codecFrameSpk} ms">
              <span class="stage-label">{config.codecSpk.toUpperCase()} Frame</span>
              <span class="stage-value">{codecFrameSpk} ms</span>
            </div>
          {/if}
          <div class="latency-stage jitter" style="flex: {jitterLatencyMs}"
               title="Receiver-side jitter buffer: {jitterBufferCapacityMs} ms capacity, ~{jitterLatencyMs} ms latency (50% fill)">
            <span class="stage-label">Jitter Buf (RX)</span>
            <span class="stage-value">{jitterLatencyMs} ms</span>
          </div>
        </div>
        <div class="latency-detail">
          {#if codecFrameSpk > 0}{config.codecSpk.toUpperCase()} frame: {codecFrameSpk} ms | {/if}Jitter buffer: {jitterBufferCapacityMs} ms capacity, {jitterLatencyMs} ms latency (50% target fill)
        </div>
      </div>

      <!-- Mic path -->
      <div class="latency-path">
        <div class="latency-path-header">
          <span class="latency-path-label">Mic (Headset → PC)</span>
          <span class="latency-total" style="color: {latencyColor(latencyMicTotal)}">{latencyMicTotal.toFixed(1)} ms</span>
        </div>
        <div class="latency-pipeline">
          {#if codecFrameMic > 0}
            <div class="latency-stage codec" style="flex: {codecFrameMic}"
                 title="{config.codecMic.toUpperCase()} frame accumulation: {codecFrameMic} ms">
              <span class="stage-label">{config.codecMic.toUpperCase()} Frame</span>
              <span class="stage-value">{codecFrameMic} ms</span>
            </div>
          {/if}
          <div class="latency-stage jitter" style="flex: {jitterLatencyMs}"
               title="Receiver-side jitter buffer: {jitterBufferCapacityMs} ms capacity, ~{jitterLatencyMs} ms latency (50% fill)">
            <span class="stage-label">Jitter Buf (RX)</span>
            <span class="stage-value">{jitterLatencyMs} ms</span>
          </div>
        </div>
        <div class="latency-detail">
          {#if codecFrameMic > 0}{config.codecMic.toUpperCase()} frame: {codecFrameMic} ms | {/if}Jitter buffer: {jitterBufferCapacityMs} ms capacity, {jitterLatencyMs} ms latency (50% target fill)
        </div>
      </div>
    </div>
    <p class="latency-note">Latency = half jitter buffer capacity (target fill state = 50%). Configure payload and buffer sizes in Radio Settings.</p>
  </div>

  {#if showWarningBanner && overBudget}
    <div class="banner danger">Total data rate exceeds PHY capacity. Reduce sample rate, channels, or switch to a higher compression codec.</div>
  {/if}
  {#if showWarningBanner && lowHeadroom}
    <div class="banner warning">Headroom below 20%. Link may be unstable under interference.</div>
  {/if}
</div>

<style>
  .link-budget {
    background: var(--surface-2);
    border-radius: 8px;
    padding: 12px 16px;
    margin: 12px 0;
  }
  .link-budget h4 {
    margin: 0 0 8px;
    font-size: 0.85rem;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--text-secondary);
  }
  .budget-breakdown {
    font-family: 'JetBrains Mono', 'Fira Code', monospace;
    font-size: 0.8rem;
  }
  .budget-row {
    display: grid;
    grid-template-columns: 80px 1fr 100px;
    gap: 8px;
    padding: 2px 0;
    color: var(--text-secondary);
  }
  .budget-row .value {
    text-align: right;
    color: var(--text-primary);
  }
  .budget-row.total {
    font-weight: 600;
    color: var(--text-primary);
  }
  hr {
    border: none;
    border-top: 1px solid var(--border);
    margin: 4px 0;
  }
  .budget-bar-container { margin-top: 10px; }
  .budget-bar {
    height: 14px;
    background: var(--surface-3);
    border-radius: 7px;
    overflow: hidden;
  }
  .budget-bar-fill {
    height: 100%;
    border-radius: 7px;
    transition: width 0.3s, background 0.3s;
  }
  .budget-bar-label {
    font-size: 0.78rem;
    margin-top: 4px;
    display: flex;
    align-items: center;
    gap: 8px;
    color: var(--text-secondary);
  }
  .badge {
    font-size: 0.7rem;
    padding: 1px 6px;
    border-radius: 4px;
    font-weight: 600;
    text-transform: uppercase;
  }
  .badge.success { background: var(--color-success); color: #fff; }
  .badge.warning { background: var(--color-warning); color: #000; }
  .badge.danger { background: var(--color-danger); color: #fff; }
  .banner {
    margin-top: 8px;
    padding: 8px 12px;
    border-radius: 6px;
    font-size: 0.82rem;
  }
  .banner.warning { background: rgba(255, 193, 7, 0.15); color: var(--color-warning); }
  .banner.danger { background: rgba(244, 67, 54, 0.15); color: var(--color-danger); }

  /* TDMA + Latency section */
  .latency-section {
    margin-top: 14px;
    padding-top: 12px;
    border-top: 1px solid var(--border);
  }
  .latency-section h4 {
    margin: 0 0 10px;
    font-size: 0.85rem;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--text-secondary);
  }
  .tdma-frame {
    display: flex;
    gap: 2px;
    height: 32px;
    border-radius: 4px;
    overflow: hidden;
  }
  .tdma-slot {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    border-radius: 3px;
    min-width: 40px;
    cursor: default;
    transition: background 0.15s;
  }
  .tdma-slot.dl {
    background: rgba(79, 195, 247, 0.2);
  }
  .tdma-slot.dl:hover {
    background: rgba(79, 195, 247, 0.35);
  }
  .tdma-slot.ul {
    background: rgba(255, 167, 38, 0.2);
  }
  .tdma-slot.ul:hover {
    background: rgba(255, 167, 38, 0.35);
  }
  .tdma-legend {
    font-size: 0.72rem;
    color: var(--text-secondary);
    margin-top: 4px;
  }
  .latency-paths {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .latency-path-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 4px;
  }
  .latency-path-label {
    font-size: 0.8rem;
    color: var(--text-secondary);
  }
  .latency-total {
    font-family: 'JetBrains Mono', 'Fira Code', monospace;
    font-size: 0.9rem;
    font-weight: 600;
  }
  .latency-pipeline {
    display: flex;
    gap: 2px;
    height: 28px;
    border-radius: 4px;
    overflow: hidden;
  }
  .latency-stage {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    border-radius: 3px;
    min-width: 40px;
    cursor: default;
    transition: background 0.15s;
  }
  .latency-stage.codec {
    background: rgba(171, 71, 188, 0.2);
  }
  .latency-stage.codec:hover {
    background: rgba(171, 71, 188, 0.35);
  }
  .latency-stage.jitter {
    background: rgba(79, 195, 247, 0.18);
  }
  .latency-stage.jitter:hover {
    background: rgba(79, 195, 247, 0.32);
  }
  .stage-label {
    font-size: 0.58rem;
    text-transform: uppercase;
    letter-spacing: 0.03em;
    color: var(--text-secondary);
    line-height: 1;
  }
  .stage-value {
    font-size: 0.65rem;
    font-family: 'JetBrains Mono', monospace;
    color: var(--text-primary);
    line-height: 1;
  }
  .latency-detail {
    font-size: 0.7rem;
    color: var(--text-secondary);
    margin-top: 3px;
  }
  .latency-note {
    font-size: 0.7rem;
    color: var(--text-secondary);
    opacity: 0.7;
    margin-top: 6px;
  }
</style>
