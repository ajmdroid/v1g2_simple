<script>
	import { onMount, onDestroy } from 'svelte';
	
	let settings = $state({
		gpsEnabled: false,
		obdEnabled: false,
		obdPin: '1234',
		// Auto-lockout settings
		lockoutEnabled: true,
		lockoutKaProtection: true,
		lockoutDirectionalUnlearn: true,
		lockoutFreqToleranceMHz: 8,
		lockoutLearnCount: 3,
		lockoutUnlearnCount: 5,
		lockoutManualDeleteCount: 25,
		lockoutLearnIntervalHours: 4,
		lockoutUnlearnIntervalHours: 4,
		lockoutMaxSignalStrength: 0,
		lockoutMaxDistanceM: 600
	});
	
	// V1 connection state
	let v1Connected = $state(false);
	
	// OBD state
	let obdStatus = $state({
		state: 'DISABLED',
		connected: false,
		scanning: false,
		deviceName: '',
		savedDeviceAddress: '',
		savedDeviceName: '',
		speedMph: 0
	});
	let obdDevices = $state([]);
	let obdScanning = $state(false);
	let obdPin = $state('1234');
	
	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);
	let pollInterval = null;
	
	onMount(async () => {
		await fetchSettings();
		await fetchObdStatus();
		await fetchV1Status();
		// Poll OBD status every 2 seconds
		pollInterval = setInterval(async () => {
			await fetchObdStatus();
			await fetchV1Status();
		}, 2000);
	});
	
	onDestroy(() => {
		if (pollInterval) clearInterval(pollInterval);
	});
	
	async function fetchV1Status() {
		try {
			const res = await fetch('/api/v1/current');
			if (res.ok) {
				const data = await res.json();
				v1Connected = data.connected || false;
			}
		} catch (e) {
			v1Connected = false;
		}
	}
	
	async function fetchSettings() {
		loading = true;
		try {
			const res = await fetch('/api/settings');
			if (res.ok) {
				const data = await res.json();
				settings = { ...settings, ...data };
				obdPin = data.obdPin || '1234';
			}
		} catch (e) {
			message = { type: 'error', text: 'Failed to load settings' };
		} finally {
			loading = false;
		}
	}
	
	async function fetchObdStatus() {
		try {
			const res = await fetch('/api/obd/status');
			if (res.ok) {
				const data = await res.json();
				obdStatus = data;
				obdScanning = data.scanning;
			}
		} catch (e) {
			// Ignore polling errors
		}
	}
	
	async function fetchObdDevices() {
		try {
			const res = await fetch('/api/obd/devices');
			if (res.ok) {
				const data = await res.json();
				obdDevices = data.devices || [];
				obdScanning = data.scanning;
			}
		} catch (e) {
			console.error('Failed to fetch OBD devices:', e);
		}
	}
	
	// Modal state for device selection
	let showDeviceModal = $state(false);
	
	async function startObdScan() {
		// Warn user if V1 isn't connected yet
		if (!v1Connected) {
			const proceed = confirm(
				'V1 is not connected yet. Scanning for OBD devices will delay V1 connection by ~30 seconds.\n\n' +
				'For best results, wait for V1 to connect first.\n\n' +
				'Continue anyway?'
			);
			if (!proceed) {
				return;
			}
		}
		
		obdScanning = true;
		showDeviceModal = true;  // Open modal when scanning starts
		message = null;
		
		try {
			const res = await fetch('/api/obd/scan', { method: 'POST' });
			if (res.ok) {
				message = { type: 'info', text: 'Scanning for BLE devices... (~30 seconds)' };
				// Poll for devices during scan
				const pollDevices = setInterval(async () => {
					await fetchObdDevices();
					if (!obdScanning) {
						clearInterval(pollDevices);
					}
				}, 2000);
				// Stop polling after 35 seconds max
				setTimeout(() => {
					clearInterval(pollDevices);
					obdScanning = false;
					fetchObdDevices();
				}, 35000);
			}
		} catch (e) {
			message = { type: 'error', text: 'Failed to start scan' };
			obdScanning = false;
		}
	}
	
	async function stopObdScan() {
		try {
			const res = await fetch('/api/obd/scan/stop', { method: 'POST' });
			if (res.ok) {
				obdScanning = false;
				message = { type: 'info', text: 'Scan stopped' };
				await fetchObdDevices();
			}
		} catch (e) {
			message = { type: 'error', text: 'Failed to stop scan' };
		}
	}
	
	async function connectToDevice(device) {
		message = null;
		showDeviceModal = false;  // Close modal when connecting
		
		try {
			const params = new URLSearchParams();
			params.append('address', device.address);
			params.append('name', device.name);
			params.append('pin', obdPin);
			
			const res = await fetch('/api/obd/connect', {
				method: 'POST',
				headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
				body: params
			});
			
			if (res.ok) {
				message = { type: 'success', text: `Connecting to ${device.name}...` };
				// Refresh status
				setTimeout(fetchObdStatus, 1000);
			} else {
				message = { type: 'error', text: 'Failed to connect' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	async function forgetSavedDevice() {
		if (!confirm('Forget the saved OBD device? You will need to scan and connect again.')) return;
		
		try {
			const res = await fetch('/api/obd/forget', { method: 'POST' });
			if (res.ok) {
				message = { type: 'success', text: 'Saved device forgotten' };
				await fetchObdStatus();
			} else {
				message = { type: 'error', text: 'Failed to forget device' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	async function clearScanResults() {
		try {
			await fetch('/api/obd/devices/clear', { method: 'POST' });
			obdDevices = [];
		} catch (e) {
			console.error('Failed to clear devices:', e);
		}
	}
	
	async function saveSettings() {
		saving = true;
		message = null;
		
		try {
			const params = new URLSearchParams();
			params.append('gpsEnabled', settings.gpsEnabled);
			params.append('obdEnabled', settings.obdEnabled);
			params.append('obdPin', obdPin);
			params.append('lockoutEnabled', settings.lockoutEnabled);
			params.append('lockoutKaProtection', settings.lockoutKaProtection);
			params.append('lockoutDirectionalUnlearn', settings.lockoutDirectionalUnlearn);
			params.append('lockoutFreqToleranceMHz', settings.lockoutFreqToleranceMHz);
			params.append('lockoutLearnCount', settings.lockoutLearnCount);
			params.append('lockoutUnlearnCount', settings.lockoutUnlearnCount);
			params.append('lockoutManualDeleteCount', settings.lockoutManualDeleteCount);
			params.append('lockoutLearnIntervalHours', settings.lockoutLearnIntervalHours);
			params.append('lockoutUnlearnIntervalHours', settings.lockoutUnlearnIntervalHours);
			params.append('lockoutMaxSignalStrength', settings.lockoutMaxSignalStrength);
			params.append('lockoutMaxDistanceM', settings.lockoutMaxDistanceM);
			
			const res = await fetch('/settings', {
				method: 'POST',
				headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
				body: params
			});
			
			if (res.ok) {
				message = { type: 'success', text: 'Settings saved!' };
			} else {
				message = { type: 'error', text: 'Failed to save settings' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		} finally {
			saving = false;
		}
	}
	
	function resetToDefaults() {
		if (!confirm('Reset all lockout settings to defaults?')) return;
		
		settings.lockoutEnabled = true;
		settings.lockoutKaProtection = true;
		settings.lockoutDirectionalUnlearn = true;
		settings.lockoutFreqToleranceMHz = 8;
		settings.lockoutLearnCount = 3;
		settings.lockoutUnlearnCount = 5;
		settings.lockoutManualDeleteCount = 25;
		settings.lockoutLearnIntervalHours = 4;
		settings.lockoutUnlearnIntervalHours = 4;
		settings.lockoutMaxSignalStrength = 0;
		settings.lockoutMaxDistanceM = 600;
	}
	
	function getObdStateLabel(state) {
		const labels = {
			'OBD_DISABLED': 'Disabled',
			'IDLE': 'Idle',
			'SCANNING': 'Scanning...',
			'CONNECTING': 'Connecting...',
			'INITIALIZING': 'Initializing...',
			'READY': 'Ready',
			'POLLING': 'Connected',
			'DISCONNECTED': 'Disconnected',
			'FAILED': 'Failed'
		};
		return labels[state] || state;
	}
	
	function getObdStateBadge(state) {
		if (state === 'POLLING' || state === 'READY') return 'badge-success';
		if (state === 'CONNECTING' || state === 'INITIALIZING' || state === 'SCANNING') return 'badge-warning';
		if (state === 'FAILED' || state === 'DISCONNECTED') return 'badge-error';
		return 'badge-ghost';
	}
</script>

<div class="space-y-6">
	<h1 class="text-2xl font-bold">🔌 Integrations</h1>
	
	{#if message}
		<div class="alert alert-{message.type === 'error' ? 'error' : message.type === 'success' ? 'success' : 'info'}" role="status">
			<span>{message.text}</span>
		</div>
	{/if}
	
	{#if loading}
		<div class="flex justify-center p-8">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<!-- GPS Module -->
		<div class="card bg-base-200">
			<div class="card-body space-y-4">
				<h2 class="card-title">📍 GPS Module</h2>
				<p class="text-sm text-base-content/60">Serial GPS module for speed display and lockouts.</p>
				
				<label class="label cursor-pointer">
					<div class="flex flex-col">
						<span class="label-text">Enable GPS Module</span>
						<span class="label-text-alt text-base-content/50">Required for lockouts and GPS speed display</span>
					</div>
					<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.gpsEnabled} />
				</label>
				
				<div class="text-xs text-base-content/40">
					💡 Auto-disables after 60 seconds if no GPS module detected.
				</div>
			</div>
		</div>

		<!-- OBD-II Module -->
		<div class="card bg-base-200">
			<div class="card-body space-y-4">
				<h2 class="card-title">🚗 OBD-II Module</h2>
				<p class="text-sm text-base-content/60">Bluetooth ELM327 adapter for vehicle speed.</p>
				
				<label class="label cursor-pointer">
					<div class="flex flex-col">
						<span class="label-text">Enable OBD-II</span>
						<span class="label-text-alt text-base-content/50">Connect to ELM327 BLE adapter</span>
					</div>
					<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.obdEnabled} />
				</label>
				
				{#if settings.obdEnabled}
					<!-- OBD Status -->
					<div class="divider text-sm">Connection Status</div>
					
					<div class="flex items-center justify-between">
						<div>
							<span class="badge {getObdStateBadge(obdStatus.state)}">{getObdStateLabel(obdStatus.state)}</span>
							{#if obdStatus.deviceName}
								<span class="ml-2 text-sm">{obdStatus.deviceName}</span>
							{/if}
						</div>
						{#if obdStatus.connected && obdStatus.speedMph !== undefined}
							<span class="text-lg font-mono">{Math.round(obdStatus.speedMph)} mph</span>
						{/if}
					</div>
					
					{#if obdStatus.savedDeviceName}
						<div class="flex items-center justify-between">
							<div class="text-sm text-base-content/60">
								Saved device: <span class="font-mono">{obdStatus.savedDeviceName}</span>
							</div>
							<button class="btn btn-xs btn-ghost text-error" onclick={forgetSavedDevice}>
								🗑️ Forget
							</button>
						</div>
					{/if}
					
					<!-- Device Scan -->
					<div class="divider text-sm">Device Selection</div>
					
					<div class="form-control">
						<label class="label" for="obd-pin">
							<span class="label-text">PIN Code</span>
							<span class="label-text-alt">Usually 1234 or 0000</span>
						</label>
						<input 
							id="obd-pin"
							type="text" 
							class="input input-bordered w-32"
							bind:value={obdPin}
							maxlength="6"
							placeholder="1234"
						/>
					</div>
					
					<div class="flex gap-2">
						{#if obdScanning}
							<button 
								class="btn btn-warning"
								onclick={stopObdScan}
							>
								<span class="loading loading-spinner loading-sm"></span>
								Stop Scan
							</button>
						{:else}
							<button 
								class="btn btn-primary"
								onclick={startObdScan}
							>
								🔍 Scan for Devices
							</button>
						{/if}
						{#if obdDevices.length > 0}
							<button class="btn btn-ghost btn-sm" onclick={() => showDeviceModal = true}>
								📋 View {obdDevices.length} device{obdDevices.length > 1 ? 's' : ''}
							</button>
						{/if}
					</div>
					
					<p class="text-xs text-base-content/50">
						{#if obdDevices.length > 0}
							Found {obdDevices.length} device{obdDevices.length > 1 ? 's' : ''}. Click "View" to select one.
						{:else}
							No devices found yet. Click "Scan for Devices" to search.
						{/if}
					</p>
				{/if}
			</div>
		</div>

		<!-- Auto-Lockout Master -->
		<div class="card bg-base-200">
			<div class="card-body space-y-4">
				<h2 class="card-title">🔒 Auto-Lockout System</h2>
				<p class="text-sm text-base-content/60">Automatically learn and mute false alerts at specific locations.</p>
				
				<label class="label cursor-pointer">
					<div class="flex flex-col">
						<span class="label-text font-semibold">Enable Auto-Lockouts</span>
						<span class="label-text-alt text-base-content/50">Learn false alerts and auto-mute in the future</span>
					</div>
					<input type="checkbox" class="toggle toggle-success" bind:checked={settings.lockoutEnabled} />
				</label>
				
				{#if !settings.gpsEnabled && settings.lockoutEnabled}
					<div class="alert alert-warning text-sm">
						<svg xmlns="http://www.w3.org/2000/svg" class="stroke-current shrink-0 h-5 w-5" fill="none" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z" /></svg>
						<span>GPS module must be enabled for lockouts to work!</span>
					</div>
				{/if}
			</div>
		</div>

		{#if settings.lockoutEnabled}
			<!-- Learning Settings -->
			<div class="card bg-base-200">
				<div class="card-body space-y-4">
					<h2 class="card-title">📚 Learning Settings</h2>
					<p class="text-sm text-base-content/60">How alerts are learned and promoted to lockouts.</p>
					
					<div class="grid grid-cols-2 gap-4">
						<div class="form-control">
							<label class="label" for="learn-count">
								<span class="label-text">Learn Count</span>
							</label>
							<input 
								id="learn-count"
								type="number" 
								class="input input-bordered w-full"
								bind:value={settings.lockoutLearnCount}
								min="1"
								max="10"
							/>
<div class="label">
								<span class="label-text-alt text-base-content/50">Default: 3</span>
							</div>
						</div>
						
						<div class="form-control">
							<label class="label" for="learn-interval">
								<span class="label-text">Learn Interval (hrs)</span>
							</label>
							<input 
								id="learn-interval"
								type="number" 
								class="input input-bordered w-full"
								bind:value={settings.lockoutLearnIntervalHours}
								min="0"
								max="24"
							/>
<div class="label">
								<span class="label-text-alt text-base-content/50">Default: 4</span>
							</div>
						</div>
					</div>
					
					<label class="label cursor-pointer">
						<div class="flex flex-col">
							<span class="label-text">Ka Band Protection</span>
							<span class="label-text-alt text-base-content/50">Never auto-learn Ka (real threats)</span>
						</div>
						<input type="checkbox" class="toggle toggle-warning" bind:checked={settings.lockoutKaProtection} />
					</label>
				</div>
			</div>

			<!-- Unlearning Settings -->
			<div class="card bg-base-200">
				<div class="card-body space-y-4">
					<h2 class="card-title">🗑️ Unlearning Settings</h2>
					<p class="text-sm text-base-content/60">How lockouts are removed when alerts stop appearing.</p>
					
					<div class="grid grid-cols-2 gap-4">
						<div class="form-control">
							<label class="label" for="unlearn-count">
								<span class="label-text">Auto Unlearn Count</span>
							</label>
							<input 
								id="unlearn-count"
								type="number" 
								class="input input-bordered w-full"
								bind:value={settings.lockoutUnlearnCount}
								min="1"
								max="50"
							/>
<div class="label">
								<span class="label-text-alt text-base-content/50">Default: 5</span>
							</div>
						</div>
						
						<div class="form-control">
							<label class="label" for="manual-delete-count">
								<span class="label-text">Manual Delete Count</span>
							</label>
							<input 
								id="manual-delete-count"
								type="number" 
								class="input input-bordered w-full"
								bind:value={settings.lockoutManualDeleteCount}
								min="1"
								max="100"
							/>
<div class="label">
								<span class="label-text-alt text-base-content/50">Default: 25</span>
							</div>
						</div>
					</div>
					
					<label class="label cursor-pointer">
						<div class="flex flex-col">
							<span class="label-text">Directional Unlearn</span>
							<span class="label-text-alt text-base-content/50">Only unlearn when traveling same direction</span>
						</div>
						<input type="checkbox" class="toggle" bind:checked={settings.lockoutDirectionalUnlearn} />
					</label>
				</div>
			</div>

			<!-- Advanced Settings -->
			<div class="card bg-base-200">
				<div class="card-body space-y-4">
					<h2 class="card-title">⚙️ Advanced Settings</h2>
					
					<div class="grid grid-cols-2 gap-4">
						<div class="form-control">
							<label class="label" for="freq-tolerance">
								<span class="label-text">Freq Tolerance (MHz)</span>
							</label>
							<input 
								id="freq-tolerance"
								type="number" 
								class="input input-bordered w-full"
								bind:value={settings.lockoutFreqToleranceMHz}
								min="1"
								max="50"
							/>
<div class="label">
								<span class="label-text-alt text-base-content/50">Default: 8</span>
							</div>
						</div>
						
						<div class="form-control">
							<label class="label" for="max-distance">
								<span class="label-text">Max Distance (m)</span>
							</label>
							<input 
								id="max-distance"
								type="number" 
								class="input input-bordered w-full"
								bind:value={settings.lockoutMaxDistanceM}
								min="50"
								max="2000"
							/>
<div class="label">
								<span class="label-text-alt text-base-content/50">Default: 600</span>
							</div>
						</div>
					</div>
					
					<div class="form-control">
						<label class="label" for="max-signal">
							<span class="label-text">Max Signal Strength</span>
							<span class="label-text-alt">0 = No limit</span>
						</label>
						<input 
							id="max-signal"
							type="range" 
							class="range range-sm"
							bind:value={settings.lockoutMaxSignalStrength}
							min="0"
							max="8"
						/>
						<div class="w-full flex justify-between text-xs px-2">
							<span>Off</span>
							<span>1</span>
							<span>2</span>
							<span>3</span>
							<span>4</span>
							<span>5</span>
							<span>6</span>
							<span>7</span>
							<span>8</span>
						</div>
					</div>
				</div>
			</div>
		{/if}

		<!-- Save Button -->
		<div class="flex gap-2">
			<button 
				class="btn btn-primary"
				onclick={saveSettings}
				disabled={saving}
			>
				{#if saving}
					<span class="loading loading-spinner loading-sm"></span>
				{/if}
				Save Settings
			</button>
			
			{#if settings.lockoutEnabled}
				<button class="btn btn-ghost" onclick={resetToDefaults}>
					Reset Lockouts to Defaults
				</button>
			{/if}
		</div>
	{/if}
</div>

<!-- OBD Device Selection Modal -->
{#if showDeviceModal}
<div class="modal modal-open">
	<div class="modal-box max-w-lg">
		<h3 class="font-bold text-lg mb-4">🔍 Found OBD Devices</h3>
		
		{#if obdScanning}
			<div class="flex items-center gap-3 mb-4">
				<span class="loading loading-spinner loading-md"></span>
				<span>Scanning for devices...</span>
			</div>
		{/if}
		
		{#if obdDevices.length > 0}
			<div class="overflow-x-auto max-h-64">
				<table class="table table-sm">
					<thead>
						<tr>
							<th>Name</th>
							<th>Signal</th>
							<th></th>
						</tr>
					</thead>
					<tbody>
						{#each obdDevices as device}
							<tr class="hover">
								<td>
									<div class="font-mono text-sm">{device.name || 'Unknown'}</div>
									<div class="text-xs text-base-content/50">{device.address}</div>
								</td>
								<td>
									<span class="badge badge-sm {device.rssi > -60 ? 'badge-success' : device.rssi > -80 ? 'badge-warning' : 'badge-error'}">
										{device.rssi} dBm
									</span>
								</td>
								<td>
									<button 
										class="btn btn-sm btn-primary"
										onclick={() => connectToDevice(device)}
									>
										Connect
									</button>
								</td>
							</tr>
						{/each}
					</tbody>
				</table>
			</div>
		{:else if !obdScanning}
			<p class="text-base-content/60 py-4">No devices found. Make sure your OBD adapter is powered on and in range.</p>
		{/if}
		
		<div class="modal-action">
			{#if obdDevices.length > 0}
				<button class="btn btn-ghost btn-sm" onclick={clearScanResults}>
					Clear List
				</button>
			{/if}
			<button class="btn" onclick={() => showDeviceModal = false}>Close</button>
		</div>
	</div>
	<button class="modal-backdrop" aria-label="Close modal" onclick={() => showDeviceModal = false}></button>
</div>
{/if}