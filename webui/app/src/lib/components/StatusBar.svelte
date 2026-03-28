<script>
  import { device } from '../stores/device.svelte.js';
</script>

<div class="status-bar">
  <div class="status-left">
    {#if device.connectionStatus === 'connected'}
      <span class="dot green"></span> Connected ({device.demoMode ? 'Demo' : device.transportType})
    {:else if device.connectionStatus === 'connecting'}
      <span class="dot yellow"></span> Connecting...
    {:else if device.connectionStatus === 'error'}
      <span class="dot red"></span> Connection Error
    {:else}
      <span class="dot grey"></span> Disconnected
    {/if}
  </div>
  <div class="status-center">
    {#if device.toast}
      <span class="toast" class:success={device.toast.type === 'success'} class:error={device.toast.type === 'error'}>
        {device.toast.text}
      </span>
    {/if}
  </div>
  <div class="status-right">
    {#if device.connectionStatus === 'connected'}
      <button class="btn-small" onclick={() => device.disconnect()}>{device.demoMode ? 'Exit Demo' : 'Disconnect'}</button>
    {:else}
      <button class="btn-small primary" onclick={() => device.connect()}>Connect</button>
      <button class="btn-small demo" onclick={() => device.connect({ demo: true })}>Demo Mode</button>
    {/if}
  </div>
</div>

<style>
  .status-bar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 6px 16px;
    background: var(--surface-1);
    border-top: 1px solid var(--border);
    font-size: 0.78rem;
    min-height: 32px;
  }
  .status-left, .status-right { flex-shrink: 0; }
  .status-center { flex: 1; text-align: center; }
  .dot {
    display: inline-block;
    width: 8px;
    height: 8px;
    border-radius: 50%;
    margin-right: 6px;
  }
  .dot.green { background: var(--color-success); }
  .dot.yellow { background: var(--color-warning); }
  .dot.red { background: var(--color-danger); }
  .dot.grey { background: #666; }
  .toast {
    opacity: 0.9;
    font-weight: 500;
  }
  .toast.success { color: var(--color-success); }
  .toast.error { color: var(--color-danger); }
  .btn-small {
    padding: 3px 10px;
    font-size: 0.75rem;
    border-radius: 4px;
    border: 1px solid var(--border);
    background: var(--surface-2);
    color: var(--text-primary);
    cursor: pointer;
  }
  .btn-small.primary {
    background: var(--color-accent);
    border-color: var(--color-accent);
    color: #fff;
  }
  .btn-small:hover { opacity: 0.85; }
  .btn-small.demo {
    background: rgba(255, 193, 7, 0.15);
    border-color: var(--color-warning);
    color: var(--color-warning);
  }
</style>
