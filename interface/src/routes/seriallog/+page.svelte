<script>
  import { onMount } from 'svelte';
  
  let sdReady = $state(false);
  let logEnabled = $state(false);
  let logSize = $state('0 B');
  let loading = $state(true);
  let clearing = $state(false);
  let toggling = $state(false);
  let message = $state('');
  let messageType = $state('');
  let logContent = $state('');
  let loadingLog = $state(false);
  let showViewer = $state(false);
  let autoScroll = $state(true);

  async function loadStatus() {
    loading = true;
    try {
      const res = await fetch('/api/seriallog');
      if (!res.ok) throw new Error('Failed to load');
      const data = await res.json();
      
      sdReady = data.sdReady || false;
      logEnabled = data.logEnabled || false;
      logSize = data.logSize || '0 B';
    } catch (e) {
      message = 'Failed to load status';
      messageType = 'error';
    } finally {
      loading = false;
    }
  }

  async function toggleLogging() {
    toggling = true;
    message = '';
    try {
      const res = await fetch(`/api/seriallog/toggle?enable=${!logEnabled}`, { method: 'POST' });
      const data = await res.json();
      
      if (data.success) {
        logEnabled = data.enabled;
        message = logEnabled ? 'Logging enabled' : 'Logging disabled';
        messageType = 'success';
      } else {
        throw new Error('Toggle failed');
      }
    } catch (e) {
      message = 'Failed to toggle logging';
      messageType = 'error';
    } finally {
      toggling = false;
    }
  }

  async function loadLogContent() {
    loadingLog = true;
    try {
      const res = await fetch('/api/seriallog/content?tail=32768');
      if (!res.ok) throw new Error('Failed to load');
      logContent = await res.text();
      showViewer = true;
      
      // Auto-scroll to bottom after content loads
      setTimeout(() => {
        if (autoScroll) {
          const viewer = document.getElementById('log-viewer');
          if (viewer) viewer.scrollTop = viewer.scrollHeight;
        }
      }, 50);
    } catch (e) {
      message = 'Failed to load log content';
      messageType = 'error';
    } finally {
      loadingLog = false;
    }
  }

  async function clearLog() {
    if (!confirm('Clear the system log? This cannot be undone.')) return;
    
    clearing = true;
    message = '';
    try {
      const res = await fetch('/api/serial_log/clear', { method: 'POST' });
      const data = await res.json();
      
      if (data.success) {
        message = 'Log cleared!';
        messageType = 'success';
        logContent = '';
        await loadStatus();
      } else {
        throw new Error('Failed to clear');
      }
    } catch (e) {
      message = 'Failed to clear log';
      messageType = 'error';
    } finally {
      clearing = false;
    }
  }

  function downloadLog() {
    window.location.href = '/serial_log.txt';
  }

  onMount(() => {
    loadStatus();
  });
</script>

<div class="space-y-6">
  <h1 class="text-2xl font-bold text-cyan-400">System Logs</h1>
  <p class="text-base-content/60">Debug output saved to SD card</p>

  {#if message}
    <div class="alert {messageType === 'success' ? 'alert-success' : 'alert-error'}">
      {message}
    </div>
  {/if}

  {#if loading}
    <div class="flex justify-center py-12">
      <span class="loading loading-spinner loading-lg text-cyan-400"></span>
    </div>
  {:else}
    <!-- Status Card -->
    <div class="card bg-base-200">
      <div class="card-body">
        <h2 class="card-title text-lg">Status</h2>
        
        <div class="divide-y divide-base-300">
          <div class="flex justify-between items-center py-3">
            <span class="text-base-content/60">SD Card</span>
            <span class="badge {sdReady ? 'badge-success' : 'badge-error'}">
              {sdReady ? 'Mounted' : 'Not Available'}
            </span>
          </div>
          
          <div class="flex justify-between items-center py-3">
            <span class="text-base-content/60">Logging</span>
            <label class="cursor-pointer flex items-center gap-2">
              <input 
                type="checkbox" 
                class="toggle toggle-success toggle-sm" 
                checked={logEnabled}
                disabled={!sdReady || toggling}
                onchange={toggleLogging}
              />
              <span class="badge {logEnabled ? 'badge-success' : 'badge-warning'}">
                {logEnabled ? 'Enabled' : 'Disabled'}
              </span>
            </label>
          </div>
          
          <div class="flex justify-between items-center py-3">
            <span class="text-base-content/60">Log Size</span>
            <span class="font-mono">{logSize}</span>
          </div>
          
          <div class="flex justify-between items-center py-3">
            <span class="text-base-content/60">Max Size</span>
            <span class="font-mono">2 GB</span>
          </div>
        </div>
      </div>
    </div>

    <!-- Actions -->
    <div class="flex flex-wrap gap-3 justify-center">
      <button class="btn btn-secondary" onclick={loadLogContent} disabled={!sdReady || loadingLog}>
        {#if loadingLog}
          <span class="loading loading-spinner loading-sm"></span>
        {:else}
          <svg xmlns="http://www.w3.org/2000/svg" class="h-5 w-5" fill="none" viewBox="0 0 24 24" stroke="currentColor">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 12a3 3 0 11-6 0 3 3 0 016 0z" />
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M2.458 12C3.732 7.943 7.523 5 12 5c4.478 0 8.268 2.943 9.542 7-1.274 4.057-5.064 7-9.542 7-4.477 0-8.268-2.943-9.542-7z" />
          </svg>
        {/if}
        View Log
      </button>
      
      <button class="btn btn-primary" onclick={downloadLog} disabled={!sdReady}>
        <svg xmlns="http://www.w3.org/2000/svg" class="h-5 w-5" fill="none" viewBox="0 0 24 24" stroke="currentColor">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 16v1a3 3 0 003 3h10a3 3 0 003-3v-1m-4-4l-4 4m0 0l-4-4m4 4V4" />
        </svg>
        Download
      </button>
      
      <button class="btn btn-error" onclick={clearLog} disabled={!sdReady || clearing}>
        {#if clearing}
          <span class="loading loading-spinner loading-sm"></span>
        {:else}
          <svg xmlns="http://www.w3.org/2000/svg" class="h-5 w-5" fill="none" viewBox="0 0 24 24" stroke="currentColor">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16" />
          </svg>
        {/if}
        Clear
      </button>
    </div>

    <!-- Log Viewer -->
    {#if showViewer}
      <div class="card bg-base-200">
        <div class="card-body p-4">
          <div class="flex justify-between items-center mb-2">
            <h2 class="card-title text-lg">Log Viewer (last 32KB)</h2>
            <div class="flex items-center gap-4">
              <label class="cursor-pointer flex items-center gap-2 text-sm">
                <input type="checkbox" class="checkbox checkbox-sm" bind:checked={autoScroll} />
                Auto-scroll
              </label>
              <button class="btn btn-ghost btn-sm" onclick={loadLogContent}>
                <svg xmlns="http://www.w3.org/2000/svg" class="h-4 w-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                  <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 4v5h.582m15.356 2A8.001 8.001 0 004.582 9m0 0H9m11 11v-5h-.581m0 0a8.003 8.003 0 01-15.357-2m15.357 2H15" />
                </svg>
                Refresh
              </button>
              <button class="btn btn-ghost btn-sm" onclick={() => showViewer = false}>
                <svg xmlns="http://www.w3.org/2000/svg" class="h-4 w-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                  <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12" />
                </svg>
              </button>
            </div>
          </div>
          <pre 
            id="log-viewer"
            class="bg-base-300 rounded-lg p-4 text-xs font-mono overflow-auto max-h-96 whitespace-pre-wrap break-all"
          >{logContent || 'Log is empty'}</pre>
        </div>
      </div>
    {/if}

    <!-- Note -->
    <div class="alert bg-base-300">
      <svg xmlns="http://www.w3.org/2000/svg" class="h-5 w-5 text-info" fill="none" viewBox="0 0 24 24" stroke="currentColor">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z" />
      </svg>
      <div>
        <span class="font-semibold">Note:</span> Serial log captures all debug output (what you'd see in <code class="text-cyan-400">pio device monitor</code>). 
        Useful for debugging issues in the field. Log rotates automatically at 2GB.
      </div>
    </div>
  {/if}
</div>
