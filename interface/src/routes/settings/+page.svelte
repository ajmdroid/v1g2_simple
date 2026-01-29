<script>
	import { onMount } from 'svelte';
	
    let settings = $state({
        ap_ssid: '',
        ap_password: '',
        proxy_ble: true,
        proxy_name: 'V1C-LE-S3',
        autoPowerOffMinutes: 0,
        apTimeoutMinutes: 0
    });
	
	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);
	let restoreFile = $state(null);
	let restoring = $state(false);
	
	// WiFi client (STA) state
	let wifiStatus = $state({
		enabled: false,
		savedSSID: '',
		state: 'disabled',
		connectedSSID: '',
		ip: '',
		rssi: 0,
		scanRunning: false
	});
	let wifiNetworks = $state([]);
	let wifiScanning = $state(false);
	let showWifiModal = $state(false);
	let selectedNetwork = $state(null);
	let wifiPassword = $state('');
	let wifiConnecting = $state(false);
	let wifiPollInterval = $state(null);
	
	onMount(async () => {
		await fetchSettings();
		await fetchWifiStatus();
		
		// Poll WiFi status every 3 seconds when modal is open
		return () => {
			if (wifiPollInterval) clearInterval(wifiPollInterval);
		};
	});
	
	async function fetchSettings() {
		try {
			const res = await fetch('/api/settings');
			if (res.ok) {
				const data = await res.json();
				settings = { ...settings, ...data };
			}
		} catch (e) {
			message = { type: 'error', text: 'Failed to load settings' };
		} finally {
			loading = false;
		}
	}
	
	async function fetchWifiStatus() {
		try {
			const res = await fetch('/api/wifi/status');
			if (res.ok) {
				const data = await res.json();
				wifiStatus = { ...wifiStatus, ...data };
			}
		} catch (e) {
			console.error('Failed to fetch WiFi status:', e);
		}
	}
	
	async function startWifiScan() {
		wifiScanning = true;
		wifiNetworks = [];
		showWifiModal = true;
		
		// Start polling for scan results
		if (wifiPollInterval) clearInterval(wifiPollInterval);
		wifiPollInterval = setInterval(pollWifiScan, 1000);
		
		try {
			await fetch('/api/wifi/scan', { method: 'POST' });
		} catch (e) {
			message = { type: 'error', text: 'Failed to start WiFi scan' };
			wifiScanning = false;
		}
	}
	
	async function pollWifiScan() {
		try {
			const res = await fetch('/api/wifi/scan', { method: 'POST' });
			if (res.ok) {
				const data = await res.json();
				if (!data.scanning && data.networks.length > 0) {
					wifiNetworks = data.networks;
					wifiScanning = false;
					clearInterval(wifiPollInterval);
					wifiPollInterval = null;
				} else if (!data.scanning) {
					// Scan complete but no networks
					wifiScanning = false;
					clearInterval(wifiPollInterval);
					wifiPollInterval = null;
				}
			}
		} catch (e) {
			console.error('Error polling WiFi scan:', e);
		}
		
		// Also update status
		await fetchWifiStatus();
	}
	
	function selectNetwork(network) {
		selectedNetwork = network;
		wifiPassword = '';
	}
	
	async function connectToNetwork() {
		if (!selectedNetwork) return;
		
		wifiConnecting = true;
		try {
			const res = await fetch('/api/wifi/connect', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({
					ssid: selectedNetwork.ssid,
					password: wifiPassword
				})
			});
			
			if (res.ok) {
				message = { type: 'success', text: `Connecting to ${selectedNetwork.ssid}...` };
				// Start polling for connection status
				if (wifiPollInterval) clearInterval(wifiPollInterval);
				wifiPollInterval = setInterval(async () => {
					await fetchWifiStatus();
					if (wifiStatus.state === 'connected') {
						clearInterval(wifiPollInterval);
						wifiPollInterval = null;
						showWifiModal = false;
						message = { type: 'success', text: `Connected to ${wifiStatus.connectedSSID}!` };
					} else if (wifiStatus.state === 'failed') {
						clearInterval(wifiPollInterval);
						wifiPollInterval = null;
						message = { type: 'error', text: 'Connection failed. Check password.' };
					}
				}, 1000);
			} else {
				message = { type: 'error', text: 'Failed to initiate connection' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		} finally {
			wifiConnecting = false;
		}
	}
	
	async function disconnectWifi() {
		try {
			await fetch('/api/wifi/disconnect', { method: 'POST' });
			await fetchWifiStatus();
			message = { type: 'success', text: 'Disconnected from WiFi' };
		} catch (e) {
			message = { type: 'error', text: 'Failed to disconnect' };
		}
	}
	
	async function forgetWifi() {
		if (!confirm('Forget saved WiFi network? You will need to reconnect manually.')) return;
		
		try {
			await fetch('/api/wifi/forget', { method: 'POST' });
			await fetchWifiStatus();
			message = { type: 'success', text: 'WiFi credentials forgotten' };
		} catch (e) {
			message = { type: 'error', text: 'Failed to forget network' };
		}
	}
	
	async function toggleWifiClient(enabled) {
		try {
			const res = await fetch('/api/wifi/enable', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ enabled })
			});
			if (res.ok) {
				await fetchWifiStatus();
				message = { type: 'success', text: enabled ? 'WiFi client enabled' : 'WiFi client disabled' };
			} else {
				message = { type: 'error', text: 'Failed to change WiFi setting' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	function closeWifiModal() {
		showWifiModal = false;
		selectedNetwork = null;
		wifiPassword = '';
		if (wifiPollInterval) {
			clearInterval(wifiPollInterval);
			wifiPollInterval = null;
		}
	}
	
	async function saveSettings() {
		saving = true;
		message = null;
		
		try {
			const formData = new FormData();
			formData.append('ap_ssid', settings.ap_ssid);
			formData.append('ap_password', settings.ap_password);
			formData.append('proxy_ble', settings.proxy_ble);
			formData.append('proxy_name', settings.proxy_name);
			formData.append('autoPowerOffMinutes', settings.autoPowerOffMinutes);
			formData.append('apTimeoutMinutes', settings.apTimeoutMinutes);
			
			const res = await fetch('/settings', {
				method: 'POST',
				body: formData
			});
			
			if (res.ok) {
				message = { type: 'success', text: 'Settings saved! WiFi will restart.' };
			} else {
				message = { type: 'error', text: 'Failed to save settings' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		} finally {
			saving = false;
		}
	}
	
	async function downloadBackup() {
		try {
			const res = await fetch('/api/settings/backup');
			if (res.ok) {
				const blob = await res.blob();
				const url = window.URL.createObjectURL(blob);
				const a = document.createElement('a');
				a.href = url;
				a.download = 'v1simple_backup.json';
				document.body.appendChild(a);
				a.click();
				document.body.removeChild(a);
				window.URL.revokeObjectURL(url);
				message = { type: 'success', text: 'Backup downloaded!' };
			} else {
				message = { type: 'error', text: 'Failed to download backup' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	function handleFileSelect(e) {
		const file = e.target.files[0];
		if (file) {
			restoreFile = file;
		}
	}
	
	async function restoreBackup() {
		if (!restoreFile) {
			message = { type: 'error', text: 'Please select a backup file first' };
			return;
		}
		
		// Confirm before overwriting
		if (!confirm('⚠️ This will overwrite all your current settings and profiles.\n\nAre you sure you want to restore from this backup?')) {
			return;
		}
		
		restoring = true;
		message = null;
		
		try {
			const text = await restoreFile.text();
			const res = await fetch('/api/settings/restore', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: text
			});
			
			const data = await res.json();
			if (res.ok && data.success) {
				message = { type: 'success', text: 'Settings restored! Refresh to see changes.' };
				restoreFile = null;
				// Refresh settings
				await fetchSettings();
			} else {
				message = { type: 'error', text: data.error || 'Failed to restore backup' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Failed to read backup file' };
		} finally {
			restoring = false;
		}
	}
</script>

<div class="space-y-6">
	<h1 class="text-2xl font-bold">Settings</h1>
	
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
		<!-- AP Settings -->
		<div class="card bg-base-200">
			<div class="card-body">
				<h2 class="card-title">📡 Access Point (AP)</h2>
				<p class="text-sm text-base-content/60">Device hosts its own hotspot for direct connection.</p>
				
				<div class="form-control">
					<label class="label" for="ap-ssid">
						<span class="label-text">AP Name</span>
					</label>
					<input 
						id="ap-ssid"
						type="text" 
						class="input input-bordered" 
						bind:value={settings.ap_ssid}
						placeholder="V1G2-Display"
					/>
				</div>

				<div class="form-control">
					<label class="label" for="ap-password">
						<span class="label-text">AP Password</span>
					</label>
					<input 
						id="ap-password"
						type="password" 
						class="input input-bordered" 
						bind:value={settings.ap_password}
						placeholder="At least 8 characters"
					/>
				</div>
				
				<div class="form-control mt-4">
					<label class="label cursor-pointer">
						<span class="label-text">AP Always On</span>
						<input 
							type="checkbox" 
							class="toggle" 
							checked={settings.apTimeoutMinutes === 0}
							onchange={(e) => settings.apTimeoutMinutes = e.target.checked ? 0 : 15}
						/>
					</label>
					{#if settings.apTimeoutMinutes > 0}
					<label class="label" for="ap-timeout">
						<span class="label-text">Auto-off after (minutes)</span>
					</label>
					<input 
						id="ap-timeout"
						type="range" 
						class="range range-sm" 
						min="5" 
						max="60" 
						step="5"
						bind:value={settings.apTimeoutMinutes}
					/>
					<div class="label">
						<span class="label-text-alt">
							AP will turn off after {settings.apTimeoutMinutes} minutes of inactivity
						</span>
					</div>
					{/if}
				</div>
			</div>
		</div>
		
		<!-- WiFi Client (Connect to Network) -->
		<div class="card bg-base-200">
			<div class="card-body space-y-4">
				<div class="flex justify-between items-center">
					<div>
						<h2 class="card-title">📶 WiFi Client</h2>
						<p class="text-sm text-base-content/60">Connect to an existing WiFi network.</p>
					</div>
					<input 
						type="checkbox" 
						class="toggle toggle-primary" 
						checked={wifiStatus.enabled}
						onchange={(e) => toggleWifiClient(e.target.checked)}
					/>
				</div>
				
				{#if wifiStatus.enabled}
				{#if wifiStatus.state === 'connected'}
					<div class="alert alert-success">
						<span>✓ Connected to <strong>{wifiStatus.connectedSSID}</strong></span>
						<span class="text-sm">IP: {wifiStatus.ip} • Signal: {wifiStatus.rssi} dBm</span>
					</div>
					<div class="flex gap-2">
						<button class="btn btn-outline btn-sm" onclick={disconnectWifi}>
							Disconnect
						</button>
						<button class="btn btn-ghost btn-sm text-error" onclick={forgetWifi}>
							Forget Network
						</button>
					</div>
				{:else if wifiStatus.state === 'connecting'}
					<div class="alert alert-info">
						<span class="loading loading-spinner loading-sm"></span>
						<span>Connecting to {wifiStatus.savedSSID}...</span>
					</div>
				{:else if wifiStatus.savedSSID}
					<div class="alert alert-warning">
						<span>⚠️ Not connected to <strong>{wifiStatus.savedSSID}</strong></span>
					</div>
					<div class="flex gap-2">
						<button class="btn btn-primary btn-sm" onclick={startWifiScan}>
							🔍 Scan for Networks
						</button>
						<button class="btn btn-ghost btn-sm text-error" onclick={forgetWifi}>
							Forget Network
						</button>
					</div>
				{:else}
					<button class="btn btn-primary btn-sm" onclick={startWifiScan}>
						🔍 Scan for Networks
					</button>
				{/if}
				{:else}
				<div class="text-sm text-base-content/50">
					WiFi client is disabled. Enable to connect to a network.
				</div>
				{/if}
			</div>
		</div>
		
		<!-- WiFi Scan Modal -->
		{#if showWifiModal}
		<div class="modal modal-open">
			<div class="modal-box max-w-md">
				<h3 class="font-bold text-lg">Select WiFi Network</h3>
				
				{#if wifiScanning}
					<div class="flex flex-col items-center py-8">
						<span class="loading loading-spinner loading-lg"></span>
						<p class="mt-4 text-sm text-base-content/60">Scanning for networks...</p>
					</div>
				{:else if selectedNetwork}
					<div class="py-4 space-y-4">
						<p>Connect to <strong>{selectedNetwork.ssid}</strong></p>
						
						{#if selectedNetwork.secure}
							<div class="form-control">
								<label class="label" for="wifi-password">
									<span class="label-text">Password</span>
								</label>
								<input 
									id="wifi-password"
									type="password" 
									class="input input-bordered" 
									bind:value={wifiPassword}
									placeholder="Enter WiFi password"
									onkeydown={(e) => e.key === 'Enter' && connectToNetwork()}
								/>
							</div>
						{:else}
							<p class="text-sm text-warning">⚠️ This is an open network</p>
						{/if}
						
						<div class="flex gap-2 justify-end">
							<button class="btn btn-ghost" onclick={() => selectedNetwork = null}>
								Back
							</button>
							<button 
								class="btn btn-primary" 
								onclick={connectToNetwork}
								disabled={wifiConnecting || (selectedNetwork.secure && !wifiPassword)}
							>
								{#if wifiConnecting}
									<span class="loading loading-spinner loading-sm"></span>
								{/if}
								Connect
							</button>
						</div>
					</div>
				{:else}
					<div class="py-4">
						{#if wifiNetworks.length === 0}
							<p class="text-center text-base-content/60 py-4">No networks found</p>
						{:else}
							<ul class="menu bg-base-100 rounded-box max-h-64 overflow-y-auto">
								{#each wifiNetworks as network}
									<li>
										<button onclick={() => selectNetwork(network)} class="flex justify-between">
											<span class="flex items-center gap-2">
												{#if network.secure}🔒{:else}🔓{/if}
												{network.ssid}
											</span>
											<span class="text-sm text-base-content/60">
												{network.rssi} dBm
											</span>
										</button>
									</li>
								{/each}
							</ul>
						{/if}
						
						<div class="flex gap-2 justify-end mt-4">
							<button class="btn btn-ghost btn-sm" onclick={startWifiScan}>
								🔄 Rescan
							</button>
						</div>
					</div>
				{/if}
				
				<div class="modal-action">
					<button class="btn" onclick={closeWifiModal}>Close</button>
				</div>
			</div>
			<!-- svelte-ignore a11y_click_events_have_key_events a11y_no_static_element_interactions -->
			<div class="modal-backdrop bg-black/50" role="presentation" onclick={closeWifiModal}></div>
		</div>
		{/if}
		
		<!-- BLE Proxy -->
		<div class="card bg-base-200">
			<div class="card-body space-y-4">
				<h2 class="card-title">🟦 Bluetooth Proxy</h2>
				<p class="text-sm text-base-content/60">Relay V1 data to phone apps.</p>
				<label class="label cursor-pointer">
					<span class="label-text">Enable Proxy</span>
					<input type="checkbox" class="toggle" bind:checked={settings.proxy_ble} />
				</label>
				<div class="form-control">
					<label class="label" for="proxy-name">
						<span class="label-text">Proxy Name</span>
					</label>
					<input
						id="proxy-name"
						type="text"
						class="input input-bordered"
						bind:value={settings.proxy_name}
						placeholder="V1C-LE-S3"
						disabled={!settings.proxy_ble}
					/>
				</div>
			</div>
		</div>
		
		<!-- Auto Power Off -->
		<div class="card bg-base-200">
			<div class="card-body space-y-4">
				<h2 class="card-title">🔌 Auto Power Off</h2>
				<p class="text-sm text-base-content/60">Automatically power off when V1 disconnects (e.g., when you turn off your car).</p>
				<div class="form-control">
					<label class="label" for="auto-power-off">
						<span class="label-text">Minutes after disconnect (0 = disabled)</span>
					</label>
					<input
						id="auto-power-off"
						type="number"
						class="input input-bordered w-24"
						bind:value={settings.autoPowerOffMinutes}
						min="0"
						max="60"
						placeholder="0"
					/>
					<div class="label">
						<span class="label-text-alt">
							{#if settings.autoPowerOffMinutes > 0}
								Device will power off {settings.autoPowerOffMinutes} minute{settings.autoPowerOffMinutes !== 1 ? 's' : ''} after V1 disconnects
							{:else}
								Auto power-off is disabled
							{/if}
						</span>
					</div>
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
			Save Settings
		</button>
		
		<!-- Backup & Restore -->
		<div class="card bg-base-200">
			<div class="card-body space-y-4">
				<h2 class="card-title">💾 Backup & Restore</h2>
				<p class="text-sm text-base-content/60">Download your settings or restore from a backup file.</p>
				
				<div class="flex flex-col gap-3">
					<button class="btn btn-outline btn-sm" onclick={downloadBackup}>
						⬇️ Download Backup
					</button>
					
					<div class="divider my-0">OR</div>
					
					<input 
						type="file" 
						accept=".json,application/json"
						class="file-input file-input-bordered file-input-sm w-full"
						onchange={handleFileSelect}
					/>
					
					<button 
						class="btn btn-warning btn-sm" 
						onclick={restoreBackup}
						disabled={!restoreFile || restoring}
					>
						{#if restoring}
							<span class="loading loading-spinner loading-sm"></span>
						{/if}
						⬆️ Restore from Backup
					</button>
				</div>
			</div>
		</div>
	{/if}
</div>
