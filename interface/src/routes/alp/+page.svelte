<script>
	import { onMount, onDestroy } from 'svelte';
	
	let settings = $state({
		alpEnabled: false,
		alpPairingCode: '',
		alpLogToSerial: true,
		alpLogToSD: true
	});
	
	let status = $state({
		state: 'ALP_DISABLED',
		deviceName: '',
		deviceAddress: '',
		servicesDiscovered: 0,
		notificationsSubscribed: 0,
		packetsLogged: 0
	});
	
	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);
	let statusInterval = null;
	
	onMount(async () => {
		await fetchSettings();
		await fetchStatus();
		// Poll status every 2 seconds when enabled
		statusInterval = setInterval(fetchStatus, 2000);
	});
	
	onDestroy(() => {
		if (statusInterval) clearInterval(statusInterval);
	});
	
	async function fetchSettings() {
		try {
			const res = await fetch('/api/alp/settings');
			if (res.ok) {
				const data = await res.json();
				settings = { ...settings, ...data };
			}
		} catch (e) {
			message = { type: 'error', text: 'Failed to load ALP settings' };
		} finally {
			loading = false;
		}
	}
	
	async function fetchStatus() {
		try {
			const res = await fetch('/api/alp/status');
			if (res.ok) {
				const data = await res.json();
				status = { ...status, ...data };
			}
		} catch (e) {
			// Silently ignore status fetch errors during polling
		}
	}
	
	async function saveSettings() {
		saving = true;
		message = null;
		
		try {
			const res = await fetch('/api/alp/settings', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify(settings)
			});
			
			if (res.ok) {
				message = { type: 'success', text: 'ALP settings saved!' };
				await fetchStatus();
			} else {
				const data = await res.json();
				message = { type: 'error', text: data.error || 'Failed to save settings' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		} finally {
			saving = false;
		}
	}
	
	async function startScan() {
		message = null;
		try {
			const res = await fetch('/api/alp/scan', { method: 'POST' });
			if (res.ok) {
				message = { type: 'info', text: 'Scanning for ALP device...' };
				await fetchStatus();
			} else {
				message = { type: 'error', text: 'Failed to start scan' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	async function disconnect() {
		message = null;
		try {
			const res = await fetch('/api/alp/disconnect', { method: 'POST' });
			if (res.ok) {
				message = { type: 'info', text: 'Disconnected from ALP' };
				await fetchStatus();
			} else {
				message = { type: 'error', text: 'Failed to disconnect' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	function getStateDisplay(state) {
		const states = {
			'ALP_DISABLED': { text: 'Disabled', class: 'badge-ghost' },
			'ALP_SCANNING': { text: 'Scanning...', class: 'badge-info' },
			'ALP_FOUND': { text: 'Found', class: 'badge-warning' },
			'ALP_CONNECTING': { text: 'Connecting...', class: 'badge-info' },
			'ALP_CONNECTED': { text: 'Connected', class: 'badge-success' },
			'ALP_DISCONNECTED': { text: 'Disconnected', class: 'badge-warning' },
			'ALP_ERROR': { text: 'Error', class: 'badge-error' }
		};
		return states[state] || { text: state, class: 'badge-ghost' };
	}
</script>

<div class="space-y-6">
	<h1 class="text-2xl font-bold">🛡️ ALP Integration</h1>
	<p class="text-sm text-base-content/60">
		Connect to AntiLaserPriority (ALP) system for protocol discovery and logging. 
		This feature logs all BLE data from the ALP device for analysis.
	</p>
	
	{#if message}
		<div class="alert alert-{message.type === 'error' ? 'error' : message.type === 'success' ? 'success' : 'info'}" role="status" aria-live="polite">
			<span>{message.text}</span>
		</div>
	{/if}
	
	{#if loading}
		<div class="flex justify-center p-8">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<!-- Connection Status -->
		<div class="card bg-base-200">
			<div class="card-body">
				<h2 class="card-title">📡 Connection Status</h2>
				
				<div class="stats stats-vertical lg:stats-horizontal shadow">
					<div class="stat">
						<div class="stat-title">State</div>
						<div class="stat-value text-lg">
							<span class="badge {getStateDisplay(status.state).class}">
								{getStateDisplay(status.state).text}
							</span>
						</div>
					</div>
					
					{#if status.deviceName}
					<div class="stat">
						<div class="stat-title">Device</div>
						<div class="stat-value text-lg">{status.deviceName}</div>
						<div class="stat-desc">{status.deviceAddress}</div>
					</div>
					{/if}
					
					{#if status.state === 'ALP_CONNECTED'}
					<div class="stat">
						<div class="stat-title">Services</div>
						<div class="stat-value text-lg">{status.servicesDiscovered}</div>
					</div>
					
					<div class="stat">
						<div class="stat-title">Notifications</div>
						<div class="stat-value text-lg">{status.notificationsSubscribed}</div>
					</div>
					
					<div class="stat">
						<div class="stat-title">Packets Logged</div>
						<div class="stat-value text-lg">{status.packetsLogged}</div>
					</div>
					{/if}
				</div>
				
				<div class="card-actions justify-end mt-4">
					{#if status.state === 'ALP_CONNECTED'}
						<button class="btn btn-warning btn-sm" onclick={disconnect}>
							Disconnect
						</button>
					{:else if settings.alpEnabled && status.state !== 'ALP_SCANNING' && status.state !== 'ALP_CONNECTING'}
						<button class="btn btn-primary btn-sm" onclick={startScan}>
							Start Scan
						</button>
					{/if}
				</div>
			</div>
		</div>
		
		<!-- ALP Settings -->
		<div class="card bg-base-200">
			<div class="card-body">
				<h2 class="card-title">⚙️ Settings</h2>
				
				<div class="form-control">
					<label class="label cursor-pointer">
						<span class="label-text">Enable ALP Integration</span>
						<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.alpEnabled} />
					</label>
				</div>
				
				<div class="form-control">
					<label class="label" for="pairing-code">
						<span class="label-text">Pairing Code (6 digits)</span>
					</label>
					<input 
						id="pairing-code"
						type="text" 
						class="input input-bordered w-32" 
						bind:value={settings.alpPairingCode}
						placeholder="000000"
						maxlength="6"
						pattern="[0-9]*"
						disabled={!settings.alpEnabled}
					/>
					<label class="label">
						<span class="label-text-alt text-base-content/60">
							Optional: Some ALP devices require a pairing code
						</span>
					</label>
				</div>
			</div>
		</div>
		
		<!-- Logging Options -->
		<div class="card bg-base-200">
			<div class="card-body">
				<h2 class="card-title">📝 Logging</h2>
				<p class="text-sm text-base-content/60">
					Log all BLE packets for protocol analysis. Data is logged in raw hex format.
				</p>
				
				<div class="form-control">
					<label class="label cursor-pointer">
						<span class="label-text">Log to Serial (USB)</span>
						<input 
							type="checkbox" 
							class="toggle" 
							bind:checked={settings.alpLogToSerial}
							disabled={!settings.alpEnabled}
						/>
					</label>
				</div>
				
				<div class="form-control">
					<label class="label cursor-pointer">
						<span class="label-text">Log to SD Card</span>
						<input 
							type="checkbox" 
							class="toggle" 
							bind:checked={settings.alpLogToSD}
							disabled={!settings.alpEnabled}
						/>
					</label>
					<label class="label">
						<span class="label-text-alt text-base-content/60">
							Logs saved to /alp_log.txt on SD card
						</span>
					</label>
				</div>
			</div>
		</div>
		
		<!-- Save Button -->
		<button 
			class="btn btn-primary btn-block" 
			onclick={saveSettings}
			disabled={saving}
		>
			{#if saving}
				<span class="loading loading-spinner loading-sm"></span>
			{/if}
			Save ALP Settings
		</button>
		
		<!-- Protocol Discovery Info -->
		<div class="collapse collapse-arrow bg-base-200">
			<input type="checkbox" /> 
			<div class="collapse-title font-medium">
				ℹ️ About Protocol Discovery
			</div>
			<div class="collapse-content text-sm text-base-content/70">
				<p class="mb-2">
					This feature is designed for reverse-engineering the ALP BLE protocol.
					When connected, the device will:
				</p>
				<ul class="list-disc list-inside space-y-1">
					<li>Scan for devices with "ALP", "AntiLaser", or "AL Priority" in their name</li>
					<li>Enumerate all BLE services and characteristics</li>
					<li>Subscribe to all notification/indication characteristics</li>
					<li>Log all received data in hex format with timestamps</li>
				</ul>
				<p class="mt-2">
					Check the serial output or SD card for logged data.
				</p>
			</div>
		</div>
	{/if}
</div>
