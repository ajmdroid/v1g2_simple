<script>
	import { onMount, onDestroy } from 'svelte';
	
	let acknowledged = $state(false);
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
		lockoutMaxDistanceM: 600,
		// Camera alert settings
		cameraAlertsEnabled: true,
		cameraAlertDistanceM: 500,
		cameraAlertRedLight: true,
		cameraAlertSpeed: true,
		cameraAlertALPR: true,
		cameraAudioEnabled: true
	});
	
	// V1 connection state
	let v1Connected = $state(false);
	
	// GPS status polling
	let gpsStatus = $state(null);
	let resettingGps = $state(false);
	
	// Auto-lockout learning data
	let lockoutClusters = $state([]);
	let lockoutDataLoading = $state(false);
	let sessionStats = $state(null);
	
	// Camera database status
	let cameraStatus = $state(null);
	
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
		await fetchGpsStatus();
		await fetchCameraStatus();
		// Poll statuses every 2 seconds
		pollInterval = setInterval(async () => {
			await fetchObdStatus();
			await fetchV1Status();
			await fetchGpsStatus();
		}, 2000);
	});
	
	onDestroy(() => {
		if (pollInterval) clearInterval(pollInterval);
	});
	
	async function fetchCameraStatus() {
		try {
			const res = await fetch('/api/cameras/status');
			if (res.ok) {
				cameraStatus = await res.json();
			}
		} catch (e) {
			// Silently fail - cameras might not be loaded
		}
	}
	
	async function reloadCameraDatabase() {
		try {
			const res = await fetch('/api/cameras/reload', { method: 'POST' });
			if (res.ok) {
				await fetchCameraStatus();
				message = { type: 'success', text: 'Camera database reloaded' };
			} else {
				message = { type: 'error', text: 'Failed to reload camera database' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	// Test camera alert - triggers display and voice
	let cameraTestRunning = $state(false);
	async function testCameraAlert(type) {
		if (cameraTestRunning) return;
		cameraTestRunning = true;
		
		try {
			const res = await fetch(`/api/cameras/test?type=${type}`, { method: 'POST' });
			if (!res.ok) {
				const err = await res.json();
				message = { type: 'error', text: err.message || 'Test failed' };
			} else {
				// Success - the device will show the alert
				const typeNames = ['Red Light', 'Speed', 'ALPR', 'Red Light+Speed'];
				message = { type: 'success', text: `Testing ${typeNames[type]} camera alert` };
			}
		} catch (e) {
			message = { type: 'error', text: `Test failed: ${e.message}` };
		} finally {
			setTimeout(() => { cameraTestRunning = false; }, 2000);  // 2s debounce
		}
	}
	
	async function fetchGpsStatus() {
		try {
			const res = await fetch('/api/gps/status');
			if (res.ok) {
				gpsStatus = await res.json();
			}
		} catch (e) {
			// Silently fail - GPS might not be available
		}
	}
	
	async function resetGpsModule() {
		if (resettingGps) return;
		
		resettingGps = true;
		try {
			const res = await fetch('/api/gps/reset', { method: 'POST' });
			if (res.ok) {
				message = { type: 'success', text: 'GPS cold start initiated - will take 30-90 seconds to get fix' };
				// Refresh status after a short delay
				setTimeout(fetchGpsStatus, 1000);
			} else {
				message = { type: 'error', text: 'Failed to reset GPS module' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		} finally {
			resettingGps = false;
		}
	}
	
	async function fetchLockoutData() {
		if (lockoutDataLoading) return;
		
		lockoutDataLoading = true;
		try {
			const res = await fetch('/api/gps/auto-lockouts');
			if (res.ok) {
				const data = await res.json();
				lockoutClusters = data.clusters || [];
				sessionStats = data.session || null;
			}
		} catch (e) {
			console.error('Failed to fetch lockout data:', e);
		} finally {
			lockoutDataLoading = false;
		}
	}
	
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
		if (!acknowledged) {
			message = { type: 'error', text: 'Please acknowledge the warning before saving' };
			return;
		}
		
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
			params.append('cameraAlertsEnabled', settings.cameraAlertsEnabled);
			params.append('cameraAudioEnabled', settings.cameraAudioEnabled);
			params.append('cameraAlertDistanceM', settings.cameraAlertDistanceM);
			
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
	<h1 class="text-2xl font-bold">🔌 Hardware Integrations</h1>
	
	<!-- Alpha Warning Banner -->
	<div class="alert alert-warning shadow-lg">
		<svg xmlns="http://www.w3.org/2000/svg" class="stroke-current shrink-0 h-6 w-6" fill="none" viewBox="0 0 24 24">
			<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z" />
		</svg>
		<div class="flex-1">
			<h3 class="font-bold">🧪 Alpha Features - Experimental</h3>
			<div class="text-sm">
				GPS and lockout features are minimally tested and may not work correctly. OBD-II speed is more stable but still experimental.
			</div>
			<div class="form-control mt-2">
				<label class="label cursor-pointer justify-start gap-2">
					<input 
						type="checkbox" 
						class="checkbox checkbox-warning" 
						bind:checked={acknowledged}
					/>
					<span class="label-text font-semibold">I understand these features are experimental</span>
				</label>
			</div>
		</div>
	</div>
	
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
		<div class="card bg-base-200" class:opacity-50={!acknowledged}>
			<div class="card-body space-y-4">
				<h2 class="card-title">📍 GPS Module</h2>
				<p class="text-sm text-base-content/60">Serial GPS module for speed display and lockouts.</p>
				
				<label class="label cursor-pointer">
					<div class="flex flex-col">
						<span class="label-text">Enable GPS Module</span>
						<span class="label-text-alt text-base-content/50">Required for lockouts and GPS speed display</span>
					</div>
					<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.gpsEnabled} disabled={!acknowledged} />
				</label>
				
				<div class="text-xs text-base-content/40">
					💡 Auto-disables after 60 seconds if no GPS module detected.
				</div>
				
				<!-- GPS Status Display -->
				{#if gpsStatus && settings.gpsEnabled}
					<div class="divider my-2"></div>
					
					<div class="flex items-center gap-2 mb-2">
						<span class="font-semibold text-sm">Status:</span>
						{#if gpsStatus.hasValidFix}
							<span class="badge badge-success">Fix Acquired</span>
						{:else if gpsStatus.moduleDetected}
							<span class="badge badge-warning">Searching...</span>
						{:else if !gpsStatus.detectionComplete}
							<span class="badge badge-info">Detecting Module...</span>
						{:else}
							<span class="badge badge-error">No Module</span>
						{/if}
					</div>
					
					{#if gpsStatus.moduleDetected}
						<div class="grid grid-cols-2 gap-2 text-sm">
							<!-- Satellites -->
							<div class="stat bg-base-300 rounded-lg p-2">
								<div class="stat-title text-xs">Satellites</div>
								<div class="stat-value text-xl">{gpsStatus.satellites ?? 0}</div>
							</div>
							
							<!-- HDOP (accuracy) -->
							<div class="stat bg-base-300 rounded-lg p-2">
								<div class="stat-title text-xs">Accuracy (HDOP)</div>
								<div class="stat-value text-xl" class:text-success={gpsStatus.hdop && gpsStatus.hdop < 2} class:text-warning={gpsStatus.hdop && gpsStatus.hdop >= 2 && gpsStatus.hdop < 5} class:text-error={gpsStatus.hdop && gpsStatus.hdop >= 5}>
									{gpsStatus.hdop ? gpsStatus.hdop.toFixed(1) : '—'}
								</div>
							</div>
							
							<!-- Speed -->
							<div class="stat bg-base-300 rounded-lg p-2">
								<div class="stat-title text-xs">Speed</div>
								<div class="stat-value text-xl">
									{gpsStatus.hasValidFix ? Math.round(gpsStatus.speed_mph) : '—'}
									<span class="text-xs font-normal">mph</span>
								</div>
							</div>
							
							<!-- Heading -->
							<div class="stat bg-base-300 rounded-lg p-2">
								<div class="stat-title text-xs">Heading</div>
								<div class="stat-value text-xl">
									{gpsStatus.hasValidFix ? Math.round(gpsStatus.heading) + '°' : '—'}
								</div>
							</div>
						</div>
						
						{#if gpsStatus.hasValidFix}
							<div class="text-xs text-base-content/60 mt-2 space-y-1">
								<div>📍 {gpsStatus.latitude?.toFixed(6)}, {gpsStatus.longitude?.toFixed(6)}</div>
								{#if gpsStatus.gpsTimeStr}
									<div>🕐 {gpsStatus.gpsTimeStr}</div>
								{/if}
								{#if gpsStatus.fixStale}
									<div class="text-warning">⚠️ Fix data is stale (>30s old)</div>
								{/if}
							</div>
						{/if}
					{:else if !gpsStatus.detectionComplete}
						<div class="flex items-center gap-2 text-sm text-base-content/60">
							<span class="loading loading-spinner loading-sm"></span>
							<span>Detecting GPS module... (up to 60 seconds)</span>
						</div>
					{:else}
						<div class="alert alert-error text-sm py-2">
							<span>No GPS module detected. Check wiring and ensure module is powered.</span>
						</div>
					{/if}
					
					<!-- Reset GPS Button -->
					<button 
						class="btn btn-outline btn-warning btn-sm gap-2 mt-2" 
						onclick={resetGpsModule}
						disabled={resettingGps}
					>
						{#if resettingGps}
							<span class="loading loading-spinner loading-xs"></span>
							Resetting...
						{:else}
							🔄 Cold Start GPS
						{/if}
					</button>
					<div class="text-xs text-base-content/50">
						Clears satellite data and forces fresh acquisition. Takes 30-90 seconds to get fix.
					</div>
				{/if}
			</div>
		</div>

		<!-- OBD-II Module -->
		<div class="card bg-base-200" class:opacity-50={!acknowledged}>
			<div class="card-body space-y-4">
				<h2 class="card-title">🚗 OBD-II Module</h2>
				<p class="text-sm text-base-content/60">Bluetooth ELM327 adapter for vehicle speed.</p>
				
				<label class="label cursor-pointer">
					<div class="flex flex-col">
						<span class="label-text">Enable OBD-II</span>
						<span class="label-text-alt text-base-content/50">Connect to ELM327 BLE adapter</span>
					</div>
					<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.obdEnabled} disabled={!acknowledged} />
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
		<div class="card bg-base-200" class:opacity-50={!acknowledged}>
			<div class="card-body space-y-4">
				<h2 class="card-title">🔒 Auto-Lockout System</h2>
				<p class="text-sm text-base-content/60">Automatically learn and mute false alerts at specific locations.</p>
				
				<label class="label cursor-pointer">
					<div class="flex flex-col">
						<span class="label-text font-semibold">Enable Auto-Lockouts</span>
						<span class="label-text-alt text-base-content/50">Learn false alerts and auto-mute in the future</span>
					</div>
					<input type="checkbox" class="toggle toggle-success" bind:checked={settings.lockoutEnabled} disabled={!acknowledged} />
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
					
					<div class="form-control">
						<label class="label" for="unlearn-interval">
							<span class="label-text">Unlearn Interval (hours)</span>
							<span class="label-text-alt">Time between counted misses</span>
						</label>
						<input 
							id="unlearn-interval"
							type="number" 
							class="input input-bordered w-24"
							bind:value={settings.lockoutUnlearnIntervalHours}
							min="0"
							max="24"
						/>
						<div class="label">
							<span class="label-text-alt text-base-content/50">Default: 4 hours (prevents multiple misses per trip)</span>
						</div>
					</div>
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
		
		<!-- Help Section -->
		<div class="collapse collapse-arrow bg-base-200">
			<input type="checkbox" />
			<div class="collapse-title font-medium">
				❓ How Auto-Lockouts Work
			</div>
			<div class="collapse-content text-sm space-y-2">
				<p><strong>Learning:</strong> When you pass a false alert multiple times (at the same location, same frequency), it gets promoted to a lockout. The V1 will be muted automatically at that location in the future.</p>
				<p><strong>Unlearning:</strong> If you pass a lockout location and the signal is no longer there, it counts as a "miss". After enough misses, the lockout is automatically removed.</p>
				<p><strong>Intervals:</strong> The learn/unlearn intervals prevent the same signal from being counted multiple times in one trip. With a 4-hour interval, you need to pass on separate occasions.</p>
				<p><strong>Directional:</strong> Lockouts are learned in a specific direction. If you approach from the opposite direction, misses won't count against unlearning.</p>
				<p><strong>Ka Protection:</strong> Ka band is almost exclusively used by police. False Ka sources are extremely rare, so auto-learning Ka is disabled by default.</p>
			</div>
		</div>
		
		<!-- Learning Data View -->
		{#if settings.lockoutEnabled}
			<div class="card bg-base-200">
				<div class="card-body space-y-4">
					<div class="flex items-center justify-between">
						<div>
							<h2 class="card-title">📊 Learning Progress</h2>
							<p class="text-sm text-base-content/60">View auto-lockout clusters stored on SD card</p>
						</div>
						<button 
							class="btn btn-sm btn-outline" 
							onclick={fetchLockoutData}
							disabled={lockoutDataLoading}
						>
							{#if lockoutDataLoading}
								<span class="loading loading-spinner loading-xs"></span>
							{:else}
								🔄 Refresh
							{/if}
						</button>
					</div>
					
					{#if lockoutClusters.length === 0}
						{#if lockoutDataLoading}
							<div class="flex justify-center py-4">
								<span class="loading loading-spinner loading-md"></span>
							</div>
						{:else}
							<div class="alert alert-info">
								<svg xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" class="stroke-current shrink-0 w-6 h-6"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"></path></svg>
								<span>No clusters learned yet. Drive past false alerts to begin learning.</span>
							</div>
						{/if}
					{:else}
						<div class="overflow-x-auto">
							<table class="table table-xs table-zebra">
								<thead>
									<tr>
										<th>Name</th>
										<th>Band</th>
										<th>Freq (MHz)</th>
										<th>Hits</th>
										<th>Status</th>
										<th>Location</th>
									</tr>
								</thead>
								<tbody>
									{#each lockoutClusters as cluster}
										<tr>
											<td class="font-mono text-xs">{cluster.name || 'Unknown'}</td>
											<td>
												<span class="badge badge-sm">
													{cluster.band === 1 ? 'X' : cluster.band === 2 ? 'K' : cluster.band === 4 ? 'Ka' : cluster.band === 8 ? 'Laser' : 'Unknown'}
												</span>
											</td>
											<td class="font-mono text-xs">
												{cluster.frequency_khz ? (cluster.frequency_khz / 1000).toFixed(3) : 'N/A'}
											</td>
											<td>
												<div class="flex flex-col text-xs">
													<span>🚗 {cluster.movingHitCount || 0}</span>
													<span>🛑 {cluster.stoppedHitCount || 0}</span>
												</div>
											</td>
											<td>
												{#if cluster.isPromoted}
													<span class="badge badge-success badge-sm">✓ Locked</span>
												{:else}
													<span class="badge badge-warning badge-sm">Learning</span>
												{/if}
											</td>
											<td class="font-mono text-xs">
												{cluster.centerLat?.toFixed(5)}, {cluster.centerLon?.toFixed(5)}
											</td>
										</tr>
									{/each}
								</tbody>
							</table>
						</div>
						
						<div class="text-sm text-base-content/60 mt-2">
							<strong>Total:</strong> {lockoutClusters.length} cluster{lockoutClusters.length !== 1 ? 's' : ''}
							<span class="mx-2">•</span>
							<strong>Promoted:</strong> {lockoutClusters.filter(c => c.isPromoted).length}
							<span class="mx-2">•</span>
							<strong>Learning:</strong> {lockoutClusters.filter(c => !c.isPromoted).length}
						</div>
					{/if}
					
					<!-- Session Statistics -->
					{#if sessionStats && sessionStats.alertsProcessed > 0}
						<div class="divider">Session Stats (since boot)</div>
						<div class="stats stats-vertical sm:stats-horizontal shadow bg-base-300 w-full">
							<div class="stat">
								<div class="stat-title">Alerts</div>
								<div class="stat-value text-lg">{sessionStats.alertsProcessed.toLocaleString()}</div>
								<div class="stat-desc">Processed</div>
							</div>
							<div class="stat">
								<div class="stat-title">Filtered</div>
								<div class="stat-value text-lg">{(sessionStats.alertsSkippedWeak + sessionStats.alertsSkippedKa).toLocaleString()}</div>
								<div class="stat-desc">
									{sessionStats.alertsSkippedWeak} weak, {sessionStats.alertsSkippedKa} Ka
								</div>
							</div>
							<div class="stat">
								<div class="stat-title">Clusters</div>
								<div class="stat-value text-lg text-success">+{sessionStats.clustersCreated}</div>
								<div class="stat-desc">{sessionStats.clusterHits.toLocaleString()} hits</div>
							</div>
							{#if sessionStats.clustersPromoted > 0}
								<div class="stat">
									<div class="stat-title">Promoted</div>
									<div class="stat-value text-lg text-accent">{sessionStats.clustersPromoted}</div>
									<div class="stat-desc">→ Lockouts</div>
								</div>
							{/if}
						</div>
						<div class="text-xs text-base-content/50 mt-2">
							Uptime: {Math.floor((sessionStats.uptimeMs || 0) / 60000)} min
						</div>
					{/if}
				</div>
			</div>
		{/if}
		
		<!-- Camera Alerts Section -->
		<div class="card bg-base-200 mt-6" class:opacity-50={!acknowledged}>
			<div class="card-body space-y-4">
				<h2 class="card-title">📸 Camera Alerts</h2>
				<p class="text-sm text-base-content/60">
					Red light cameras, speed cameras, and ALPR databases. Requires GPS and camera database on SD card.
				</p>
				
				<label class="label cursor-pointer">
					<div class="flex flex-col">
						<span class="label-text">Enable Camera Alerts</span>
						<span class="label-text-alt text-base-content/50">Alert when approaching known camera locations</span>
					</div>
					<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.cameraAlertsEnabled} disabled={!acknowledged || !settings.gpsEnabled} />
				</label>
				
				{#if !settings.gpsEnabled}
					<div class="alert alert-warning">
						<svg xmlns="http://www.w3.org/2000/svg" class="h-5 w-5" fill="none" viewBox="0 0 24 24" stroke="currentColor">
							<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01M12 3l9 16H3L12 3z" />
						</svg>
						<span class="text-sm">GPS must be enabled for camera alerts to work.</span>
					</div>
				{/if}
				
				<!-- Camera Database Status -->
				{#if cameraStatus}
					<div class="divider my-2"></div>
					<div class="flex items-center gap-2 mb-2">
						<span class="font-semibold text-sm">Database:</span>
						{#if cameraStatus.loaded}
							<span class="badge badge-success">{cameraStatus.count?.toLocaleString()} cameras</span>
						{:else}
							<span class="badge badge-warning">Not Loaded</span>
						{/if}
					</div>
					
					{#if cameraStatus.loaded}
						<div class="text-sm space-y-1">
							{#if cameraStatus.name}
								<p><span class="text-base-content/60">Name:</span> {cameraStatus.name}</p>
							{/if}
							{#if cameraStatus.date}
								<p><span class="text-base-content/60">Date:</span> {cameraStatus.date}</p>
							{/if}
							<div class="flex gap-4 text-xs text-base-content/60 mt-2">
								<span>🚦 Red Light: {cameraStatus.redLightCount?.toLocaleString() ?? 0}</span>
								<span>📷 Speed: {cameraStatus.speedCount?.toLocaleString() ?? 0}</span>
								<span>🔍 ALPR: {cameraStatus.alprCount?.toLocaleString() ?? 0}</span>
							</div>
							
							<!-- Regional Cache Info -->
							{#if cameraStatus.cache}
								<div class="mt-3 p-2 bg-base-300 rounded-lg">
									<p class="text-xs font-semibold text-success">
										📍 Regional Cache Active
									</p>
									<p class="text-xs text-base-content/60 mt-1">
										{cameraStatus.cache.count?.toLocaleString()} cameras within {cameraStatus.cache.radiusMiles} mi
									</p>
									<p class="text-xs text-base-content/40">
										Center: {cameraStatus.cache.centerLat?.toFixed(2)}°, {cameraStatus.cache.centerLon?.toFixed(2)}°
									</p>
								</div>
							{:else}
								<p class="text-xs text-base-content/40 mt-2">
									💡 Regional cache will build when GPS gets a fix
								</p>
							{/if}
						</div>
					{:else}
					<div class="text-sm text-base-content/60">
							<p class="font-semibold">📂 Copy camera database files to SD card:</p>
							<ul class="list-disc list-inside mt-1 text-xs">
								<li><code>/alpr.bin</code> - ALPR cameras (~70k)</li>
								<li><code>/redlight_cam.bin</code> - Red light cameras</li>
								<li><code>/speed_cam.bin</code> - Speed cameras</li>
							</ul>
							<div class="mt-3 p-2 bg-base-300 rounded-lg">
								<p class="text-xs font-semibold">How to get camera files:</p>
								<ol class="list-decimal list-inside text-xs mt-1 space-y-1">
									<li>Download pre-built <code>.bin</code> files from releases</li>
									<li>Or run <code>tools/download_cameras.py</code> on your computer</li>
									<li>Copy the <code>.bin</code> files to SD card root</li>
									<li>Reboot device or tap "Reload Database"</li>
								</ol>
							</div>
						</div>
					{/if}
					
					<button class="btn btn-sm btn-ghost" onclick={reloadCameraDatabase}>
						🔄 Reload Database
					</button>
					
					<!-- Test Camera Alerts -->
					<div class="divider my-2 text-xs text-base-content/50">Test Camera Alerts</div>
					
					<div class="bg-base-300 rounded-lg p-3 space-y-2">
						<p class="text-xs text-base-content/60">
							Test camera alert display and voice announcements. This simulates detecting each camera type.
						</p>
						
						<div class="flex flex-wrap gap-2">
							<button 
								class="btn btn-sm btn-outline btn-error"
								onclick={() => testCameraAlert(0)}
								disabled={cameraTestRunning}
							>
								🚦 Red Light
							</button>
							<button 
								class="btn btn-sm btn-outline btn-warning"
								onclick={() => testCameraAlert(1)}
								disabled={cameraTestRunning}
							>
								📷 Speed
							</button>
							<button 
								class="btn btn-sm btn-outline btn-info"
								onclick={() => testCameraAlert(2)}
								disabled={cameraTestRunning}
							>
								🔍 ALPR
							</button>
							<button 
								class="btn btn-sm btn-outline btn-secondary"
								onclick={() => testCameraAlert(3)}
								disabled={cameraTestRunning}
							>
								🚦📷 Both
							</button>
						</div>
					</div>
				{/if}
				
				{#if settings.cameraAlertsEnabled}
					<div class="divider my-2"></div>
					
					<!-- Camera Types -->
					<div class="space-y-2">
						<span class="label-text font-semibold">Alert Types</span>
						<div class="flex flex-wrap gap-4">
							<label class="label cursor-pointer gap-2">
								<input type="checkbox" class="checkbox checkbox-sm checkbox-error" bind:checked={settings.cameraAlertRedLight} disabled={!acknowledged} />
								<span class="label-text">🚦 Red Light</span>
							</label>
							<label class="label cursor-pointer gap-2">
								<input type="checkbox" class="checkbox checkbox-sm checkbox-warning" bind:checked={settings.cameraAlertSpeed} disabled={!acknowledged} />
								<span class="label-text">📷 Speed Camera</span>
							</label>
							<label class="label cursor-pointer gap-2">
								<input type="checkbox" class="checkbox checkbox-sm checkbox-info" bind:checked={settings.cameraAlertALPR} disabled={!acknowledged} />
								<span class="label-text">🔍 ALPR</span>
							</label>
						</div>
					</div>
					
					<!-- Alert Distance -->
					<div class="form-control">
						<label class="label" for="camera-distance">
							<span class="label-text">Alert Distance</span>
							<span class="label-text-alt">{settings.cameraAlertDistanceM}m ({Math.round(settings.cameraAlertDistanceM * 3.28084)}ft)</span>
						</label>
						<input 
							id="camera-distance"
							type="range" 
							class="range range-sm range-warning"
							bind:value={settings.cameraAlertDistanceM}
							min="100"
							max="2000"
							step="100"
							disabled={!acknowledged}
						/>
						<div class="w-full flex justify-between text-xs px-2">
							<span>100m</span>
							<span>500m</span>
							<span>1000m</span>
							<span>1500m</span>
							<span>2000m</span>
						</div>
					</div>
					
					<!-- Audio Toggle -->
					<label class="label cursor-pointer">
						<div class="flex flex-col">
							<span class="label-text">Audio Alerts</span>
							<span class="label-text-alt text-base-content/50">Play audio when approaching camera</span>
						</div>
						<input type="checkbox" class="toggle toggle-sm" bind:checked={settings.cameraAudioEnabled} disabled={!acknowledged} />
					</label>
				{/if}
			</div>
		</div>
		
		<!-- Camera Help Section -->
		<div class="collapse collapse-arrow bg-base-200">
			<input type="checkbox" />
			<div class="collapse-title font-medium">
				❓ About Camera Databases
			</div>
			<div class="collapse-content text-sm space-y-2">
				<p><strong>Red Light Cameras:</strong> Traffic enforcement cameras that photograph vehicles running red lights.</p>
				<p><strong>Speed Cameras:</strong> Fixed or mobile cameras that detect vehicles exceeding speed limits.</p>
				<p><strong>ALPR (Automatic License Plate Readers):</strong> Cameras that automatically scan and record license plates for various purposes including toll collection, parking enforcement, and law enforcement.</p>
				<p><strong>Database Updates:</strong> Download updated camera databases from <a href="https://www.rdforum.org/threads/88426/" target="_blank" class="link link-primary">RDForum</a> and place on SD card.</p>
				<p><strong>Coverage:</strong> Most databases cover USA and Canada. Coverage varies by region.</p>
			</div>
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