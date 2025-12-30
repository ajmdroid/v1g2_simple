<script>
  let currentTime = $state('Loading...');
  let enableTimesync = $state(false);
  let wifiNetworks = $state([
    { ssid: '', password: '' },
    { ssid: '', password: '' },
    { ssid: '', password: '' }
  ]);
  let loading = $state(true);
  let saving = $state(false);
  let message = $state('');
  let messageType = $state('');

  async function loadSettings() {
    loading = true;
    try {
      const res = await fetch('/api/timesettings');
      if (!res.ok) throw new Error('Failed to load');
      const data = await res.json();
      
      currentTime = data.currentTime || 'Not Set';
      enableTimesync = data.enableTimesync || false;
      wifiNetworks = data.wifiNetworks || [
        { ssid: '', password: '' },
        { ssid: '', password: '' },
        { ssid: '', password: '' }
      ];
    } catch (e) {
      message = 'Failed to load settings';
      messageType = 'error';
    } finally {
      loading = false;
    }
  }

  async function saveSettings() {
    saving = true;
    message = '';
    try {
      const formData = new FormData();
      if (enableTimesync) {
        formData.append('enableTimesync', 'on');
      }
      wifiNetworks.forEach((net, i) => {
        formData.append(`wifi${i}ssid`, net.ssid);
        formData.append(`wifi${i}pwd`, net.password);
      });
      
      const res = await fetch('/time', {
        method: 'POST',
        body: formData
      });
      
      if (res.ok) {
        message = 'Settings saved!';
        messageType = 'success';
        // Reload to get fresh data
        await loadSettings();
      } else {
        throw new Error('Save failed');
      }
    } catch (e) {
      message = 'Failed to save settings';
      messageType = 'error';
    } finally {
      saving = false;
    }
  }

  async function setTimeFromDevice() {
    saving = true;
    message = '';
    try {
      const now = Math.floor(Date.now() / 1000);
      const formData = new FormData();
      formData.append('timestamp', now.toString());
      
      const res = await fetch('/time', {
        method: 'POST',
        body: formData
      });
      
      if (res.ok) {
        message = 'Time set from this device!';
        messageType = 'success';
        await loadSettings();
      } else {
        throw new Error('Failed to set time');
      }
    } catch (e) {
      message = 'Failed to set time';
      messageType = 'error';
    } finally {
      saving = false;
    }
  }

  $effect(() => {
    loadSettings();
  });
</script>

<div class="space-y-6">
  <h1 class="text-2xl font-bold text-cyan-400">Time Settings</h1>

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
    <!-- Current Time Card -->
    <div class="card bg-base-200">
      <div class="card-body">
        <h2 class="card-title text-lg">Current Time (UTC)</h2>
        <div class="flex justify-between items-center py-2">
          <span class="text-base-content/60">System Time</span>
          <span class="font-mono text-lg">{currentTime}</span>
        </div>
        <p class="text-sm text-base-content/50">
          All times displayed and stored in UTC. Configure NTP sync or set manually for accurate timestamps.
        </p>
      </div>
    </div>

    <!-- WiFi Networks Card -->
    <div class="card bg-base-200">
      <div class="card-body">
        <h2 class="card-title text-lg">WiFi Networks for Internet/NTP</h2>
        <p class="text-sm text-base-content/50 mb-4">
          Add up to 3 WiFi networks. The device will automatically connect to whichever is available with the strongest signal.
        </p>

        <div class="form-control mb-4">
          <label class="label cursor-pointer justify-start gap-4">
            <input type="checkbox" class="toggle toggle-primary" bind:checked={enableTimesync} />
            <span class="label-text">Enable WiFi STA mode (for NTP sync & NAT routing)</span>
          </label>
        </div>

        {#each wifiNetworks as network, i}
          <div class="divider text-sm text-base-content/50">Network {i + 1}</div>
          <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
            <div class="form-control">
              <label class="label" for="wifi{i}ssid">
                <span class="label-text">SSID</span>
              </label>
              <input 
                type="text" 
                id="wifi{i}ssid"
                class="input input-bordered" 
                bind:value={network.ssid}
                placeholder={i === 0 ? 'e.g., Home WiFi' : i === 1 ? 'e.g., Car WiFi Hotspot' : 'e.g., Phone Hotspot'}
              />
            </div>
            <div class="form-control">
              <label class="label" for="wifi{i}pwd">
                <span class="label-text">Password</span>
              </label>
              <input 
                type="password" 
                id="wifi{i}pwd"
                class="input input-bordered" 
                bind:value={network.password}
                placeholder="Password"
              />
            </div>
          </div>
        {/each}

        <div class="card-actions justify-end mt-4">
          <button class="btn btn-primary" onclick={saveSettings} disabled={saving}>
            {#if saving}
              <span class="loading loading-spinner loading-sm"></span>
            {/if}
            Save WiFi Settings
          </button>
        </div>
      </div>
    </div>

    <!-- Manual Time Card -->
    <div class="card bg-base-200">
      <div class="card-body">
        <h2 class="card-title text-lg">Manual Time Setting</h2>
        <p class="text-sm text-base-content/50 mb-4">
          Set time manually if you don't want to connect to WiFi. Your device will use this time.
        </p>
        <button class="btn btn-secondary" onclick={setTimeFromDevice} disabled={saving}>
          {#if saving}
            <span class="loading loading-spinner loading-sm"></span>
          {/if}
          Set Time from This Device
        </button>
      </div>
    </div>
  {/if}
</div>
