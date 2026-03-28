<script>
  import { device } from '../stores/device.svelte.js';
  import { onMount, onDestroy } from 'svelte';
  import uPlot from 'uplot';
  import 'uplot/dist/uPlot.min.css';

  let chartEl = $state(null);
  let plot = null;

  onMount(() => {
    if (!chartEl) return;
    const opts = {
      width: chartEl.clientWidth,
      height: 200,
      cursor: { show: false },
      legend: { show: true },
      scales: {
        x: { time: true },
        y: { auto: true },
        y2: { auto: true, range: [0, 10] },
      },
      axes: [
        {},
        { label: 'RSSI (dBm)', stroke: '#4fc3f7', grid: { stroke: 'rgba(255,255,255,0.06)' } },
        { label: 'Loss (%)', side: 1, stroke: '#ff7043', scale: 'y2', grid: { show: false } },
      ],
      series: [
        {},
        { label: 'RSSI', stroke: '#4fc3f7', width: 2, scale: 'y' },
        { label: 'Pkt Loss %', stroke: '#ff7043', width: 2, scale: 'y2' },
      ],
    };
    const data = [[], [], []];
    plot = new uPlot(opts, data, chartEl);
  });

  $effect(() => {
    if (plot && device.timestamps.length > 0) {
      plot.setData([
        device.timestamps,
        device.rssiHistory,
        device.packetLossHistory,
      ]);
    }
  });

  onDestroy(() => {
    if (plot) { plot.destroy(); plot = null; }
  });
</script>

<div class="panel">
  <div class="grid-3">
    <!-- Device Info -->
    <div class="card">
      <h3>Device</h3>
      <div class="stat-row">
        <span>Firmware</span>
        <span class="mono">{device.status.fwVersion ?? '—'}</span>
      </div>
      <div class="stat-row">
        <span>Peer</span>
        <span class="badge" class:success={device.status.peerConnected} class:muted={!device.status.peerConnected}>
          {device.status.peerConnected ? 'Connected' : 'Not Connected'}
        </span>
      </div>
    </div>

    <!-- Battery -->
    <div class="card">
      <h3>Battery</h3>
      {#if device.status.battery != null}
        <div class="battery-display">
          <div class="battery-bar-outer">
            <div class="battery-bar-inner"
              style="width: {device.status.battery}%; background: {device.status.battery < 15 ? 'var(--color-danger)' : device.status.battery < 30 ? 'var(--color-warning)' : 'var(--color-success)'}">
            </div>
          </div>
          <span class="battery-pct">{device.status.battery}%</span>
        </div>
        {#if device.status.battery <= (device.config.lowBatteryThreshold || 10)}
          <div class="banner danger">Low battery!</div>
        {/if}
      {:else}
        <span class="muted-text">No data</span>
      {/if}
    </div>

    <!-- Link Quality -->
    <div class="card">
      <h3>Link Quality</h3>
      <div class="stat-row">
        <span>RSSI</span>
        <span class="mono">{device.status.rssi != null ? `${device.status.rssi} dBm` : '—'}</span>
      </div>
      <div class="stat-row">
        <span>Packet Loss</span>
        <span class="mono">{device.status.packetLoss != null ? `${device.status.packetLoss.toFixed(1)}%` : '—'}</span>
      </div>
      <div class="stat-row">
        <span>TX Power</span>
        <span class="mono">{device.status.txPower != null ? `${device.status.txPower} dBm` : '—'}</span>
      </div>
    </div>
  </div>

  <!-- Chart -->
  <div class="card">
    <h3>Link Quality History</h3>
    <div class="chart-container" bind:this={chartEl}></div>
    {#if device.timestamps.length === 0}
      <div class="chart-placeholder">Waiting for data...</div>
    {/if}
  </div>
</div>

<style>
  .panel { display: flex; flex-direction: column; gap: 12px; }
  .grid-3 {
    display: grid;
    grid-template-columns: 1fr 1fr 1fr;
    gap: 12px;
  }
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
    padding: 3px 0;
    font-size: 0.88rem;
  }
  .mono { font-family: 'JetBrains Mono', 'Fira Code', monospace; }
  .badge {
    font-size: 0.78rem;
    padding: 1px 8px;
    border-radius: 4px;
    font-weight: 500;
  }
  .badge.success { background: rgba(76, 175, 80, 0.2); color: var(--color-success); }
  .badge.muted { background: rgba(255,255,255,0.06); color: var(--text-secondary); }
  .muted-text { color: var(--text-secondary); font-size: 0.85rem; }
  .battery-display { display: flex; align-items: center; gap: 10px; margin-bottom: 8px; }
  .battery-bar-outer {
    flex: 1;
    height: 18px;
    background: var(--surface-3);
    border-radius: 9px;
    overflow: hidden;
  }
  .battery-bar-inner {
    height: 100%;
    border-radius: 9px;
    transition: width 0.5s;
  }
  .battery-pct { font-weight: 600; font-size: 1.1rem; min-width: 40px; }
  .banner {
    margin-top: 8px;
    padding: 6px 10px;
    border-radius: 6px;
    font-size: 0.8rem;
  }
  .banner.danger { background: rgba(244, 67, 54, 0.15); color: var(--color-danger); }
  .chart-container { min-height: 200px; }
  .chart-placeholder {
    color: var(--text-secondary);
    font-size: 0.85rem;
    padding: 40px;
    text-align: center;
  }
</style>
