<script>
	import { onMount } from 'svelte';
	import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
	import { postSettingsForm } from '$lib/api/settings';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	
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
	let wifiPoll = $state(null);
	let timeTickInterval = $state(null);
	let clientNowMs = $state(Date.now());
	let wifiStatusFetchInFlight = false;
	let timeStatusFetchInFlight = false;
	const TIME_STATUS_POLL_INTERVAL_MS = 7000;
	const timeStatusPoll = createPoll(async () => {
		await fetchTimeStatus();
	}, TIME_STATUS_POLL_INTERVAL_MS);

	let timeStatus = $state({
		valid: false,
		source: 0,
		confidence: 0,
		epochMs: 0,
		tzOffsetMin: 0,
		ageMs: 0,
		sampleClientMs: 0,
		syncing: false
	});
	
	onMount(async () => {
		await fetchSettings();
		await fetchWifiStatus();
		await fetchTimeStatus();
		timeStatusPoll.start();
		timeTickInterval = setInterval(() => {
			clientNowMs = Date.now();
		}, 1000);
		
		// Poll WiFi status every 3 seconds when modal is open
		return () => {
			stopWifiPoll();
			timeStatusPoll.stop();
			if (timeTickInterval) clearInterval(timeTickInterval);
		};
	});

	function stopWifiPoll() {
		if (!wifiPoll) return;
		wifiPoll.stop();
		wifiPoll = null;
	}
	
	async function fetchSettings() {
		try {
			const res = await fetchWithTimeout('/api/settings');
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
		if (wifiStatusFetchInFlight) return;
		wifiStatusFetchInFlight = true;
		try {
			const res = await fetchWithTimeout('/api/wifi/status');
			if (res.ok) {
				const data = await res.json();
				wifiStatus = { ...wifiStatus, ...data };
			}
		} catch (e) {
			console.error('Failed to fetch WiFi status:', e);
		} finally {
			wifiStatusFetchInFlight = false;
		}
	}

	function getTimeSourceLabel(source) {
		switch (source) {
			case 1: return 'CLIENT_AP';
			case 2: return 'GPS';
			case 3: return 'SNTP';
			case 4: return 'RTC';
			default: return 'NONE';
		}
	}

	function getTimeConfidenceLabel(confidence) {
		switch (confidence) {
			case 2: return 'ACCURATE';
			case 1: return 'ESTIMATED';
			default: return 'NONE';
		}
	}

	function pad2(n) {
		return String(n).padStart(2, '0');
	}

	function formatOffset(mins) {
		const sign = mins >= 0 ? '+' : '-';
		const absMins = Math.abs(mins);
		const hh = Math.floor(absMins / 60);
		const mm = absMins % 60;
		return `${sign}${pad2(hh)}:${pad2(mm)}`;
	}

	function getProjectedEpochMs() {
		if (!timeStatus.valid || !timeStatus.epochMs || !timeStatus.sampleClientMs) return 0;
		const deltaMs = Math.max(0, clientNowMs - timeStatus.sampleClientMs);
		return timeStatus.epochMs + deltaMs;
	}

	function getProjectedAgeMs() {
		if (!timeStatus.valid || !timeStatus.sampleClientMs) return 0;
		const deltaMs = Math.max(0, clientNowMs - timeStatus.sampleClientMs);
		return (timeStatus.ageMs || 0) + deltaMs;
	}

	function formatDeviceDateTime() {
		const projectedEpochMs = getProjectedEpochMs();
		if (!projectedEpochMs) return '—';
		const tzOffsetMs = (timeStatus.tzOffsetMin || 0) * 60000;
		const d = new Date(projectedEpochMs + tzOffsetMs);
		return `${d.getUTCFullYear()}-${pad2(d.getUTCMonth() + 1)}-${pad2(d.getUTCDate())} ${pad2(d.getUTCHours())}:${pad2(d.getUTCMinutes())}:${pad2(d.getUTCSeconds())} (UTC${formatOffset(timeStatus.tzOffsetMin || 0)})`;
	}

	function formatAgeMs(ms) {
		if (!ms || ms < 0) return '0s';
		const totalSeconds = Math.floor(ms / 1000);
		const hours = Math.floor(totalSeconds / 3600);
		const minutes = Math.floor((totalSeconds % 3600) / 60);
		const seconds = totalSeconds % 60;
		if (hours > 0) return `${hours}h ${minutes}m ${seconds}s`;
		if (minutes > 0) return `${minutes}m ${seconds}s`;
		return `${seconds}s`;
	}

	async function fetchTimeStatus() {
		if (timeStatusFetchInFlight) return;
		timeStatusFetchInFlight = true;
		try {
			const res = await fetchWithTimeout('/api/status');
			if (res.ok) {
				const data = await res.json();
				const t = data?.time || {};
				const now = Date.now();
				timeStatus.valid = !!t.valid;
				timeStatus.source = Number(t.source || 0);
				timeStatus.confidence = Number(t.confidence || 0);
				timeStatus.epochMs = Number(t.epochMs || 0);
				timeStatus.tzOffsetMin = Number(t.tzOffsetMin ?? t.tzOffsetMinutes ?? 0);
				timeStatus.ageMs = Number(t.ageMs || 0);
				timeStatus.sampleClientMs = timeStatus.valid ? now : 0;
			}
		} catch (e) {
			console.error('Failed to fetch time status:', e);
		} finally {
			timeStatusFetchInFlight = false;
		}
	}

	async function syncTimeFromPhone() {
		timeStatus.syncing = true;
		try {
			const res = await fetchWithTimeout('/api/time/set', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({
					unixMs: Date.now(),
					tzOffsetMin: new Date().getTimezoneOffset() * -1,
					source: 'client'
				})
			});
			const data = await res.json().catch(() => ({}));
			if (res.ok && (data.ok || data.success)) {
				const now = Date.now();
				timeStatus.valid = !!data.timeValid;
				timeStatus.source = Number(data.timeSource || 0);
				timeStatus.confidence = Number(data.timeConfidence || 0);
				timeStatus.epochMs = Number(data.epochMs || 0);
				timeStatus.tzOffsetMin = Number(data.tzOffsetMin ?? data.tzOffsetMinutes ?? 0);
				timeStatus.ageMs = Number(data.ageMs ?? data.epochAgeMs ?? 0);
				timeStatus.sampleClientMs = timeStatus.valid ? now : 0;
				message = { type: 'success', text: 'Time synced from phone.' };
			} else {
				message = { type: 'error', text: data.error || 'Failed to sync time' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Failed to sync time' };
		} finally {
			timeStatus.syncing = false;
			await fetchTimeStatus();
		}
	}
	
	async function startWifiScan() {
		wifiScanning = true;
		wifiNetworks = [];
		showWifiModal = true;
		
		// Start polling for scan results
		stopWifiPoll();
		wifiPoll = createPoll(async () => {
			await pollWifiScan();
		}, 1000);
		wifiPoll.start();
		
		try {
			await fetchWithTimeout('/api/wifi/scan', { method: 'POST' });
		} catch (e) {
			message = { type: 'error', text: 'Failed to start WiFi scan' };
			wifiScanning = false;
			stopWifiPoll();
		}
	}
	
	async function pollWifiScan() {
		try {
			const res = await fetchWithTimeout('/api/wifi/scan', { method: 'POST' });
			if (res.ok) {
				const data = await res.json();
				if (!data.scanning && data.networks.length > 0) {
					wifiNetworks = data.networks;
					wifiScanning = false;
					stopWifiPoll();
				} else if (!data.scanning) {
					// Scan complete but no networks
					wifiScanning = false;
					stopWifiPoll();
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
			const res = await fetchWithTimeout('/api/wifi/connect', {
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
				stopWifiPoll();
				wifiPoll = createPoll(async () => {
					await fetchWifiStatus();
					if (wifiStatus.state === 'connected') {
						stopWifiPoll();
						showWifiModal = false;
						message = { type: 'success', text: `Connected to ${wifiStatus.connectedSSID}!` };
					} else if (wifiStatus.state === 'failed') {
						stopWifiPoll();
						message = { type: 'error', text: 'Connection failed. Check password.' };
					}
				}, 1000);
				wifiPoll.start();
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
			await fetchWithTimeout('/api/wifi/disconnect', { method: 'POST' });
			await fetchWifiStatus();
			message = { type: 'success', text: 'Disconnected from WiFi' };
		} catch (e) {
			message = { type: 'error', text: 'Failed to disconnect' };
		}
	}
	
	async function forgetWifi() {
		if (!confirm('Forget saved WiFi network? You will need to reconnect manually.')) return;
		
		try {
			await fetchWithTimeout('/api/wifi/forget', { method: 'POST' });
			await fetchWifiStatus();
			message = { type: 'success', text: 'WiFi credentials forgotten' };
		} catch (e) {
			message = { type: 'error', text: 'Failed to forget network' };
		}
	}
	
	async function toggleWifiClient(enabled) {
		try {
			const res = await fetchWithTimeout('/api/wifi/enable', {
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
		stopWifiPoll();
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
			
			const res = await postSettingsForm(formData);
			
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
			const res = await fetchWithTimeout('/api/settings/backup');
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
		if (!confirm('Warning: This will overwrite all your current settings and profiles.\n\nAre you sure you want to restore from this backup?')) {
			return;
		}
		
		restoring = true;
		message = null;
		
		try {
			const text = await restoreFile.text();
			const res = await fetchWithTimeout('/api/settings/restore', {
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

<div class="page-stack">
	<PageHeader title="Settings" subtitle="Network, proxy, power, and backup configuration." />
	
	<StatusAlert {message} />
	
	{#if loading}
		<div class="state-loading">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<!-- AP Settings -->
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Access Point (AP)"
					subtitle="Device hosts its own hotspot for direct connection."
				/>
				
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
							class="toggle toggle-primary" 
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

		<!-- Device Time -->
		<div class="surface-card">
			<div class="card-body space-y-3">
				<CardSectionHead title="Device Time" subtitle="Manual phone sync only. No background NTP." />
				<div class="copy-subtle space-y-1">
					<div><strong>timeValid:</strong> {timeStatus.valid ? 1 : 0}</div>
					<div><strong>timeSource:</strong> {timeStatus.source} ({getTimeSourceLabel(timeStatus.source)})</div>
					<div><strong>timeConfidence:</strong> {timeStatus.confidence} ({getTimeConfidenceLabel(timeStatus.confidence)})</div>
					{#if timeStatus.valid}
						<div><strong>deviceTime:</strong> <span class="font-mono">{formatDeviceDateTime()}</span></div>
						<div><strong>timeAge:</strong> {formatAgeMs(getProjectedAgeMs())}</div>
						<div><strong>epochMs (projected):</strong> <span class="font-mono">{getProjectedEpochMs()}</span></div>
						<div><strong>tzOffsetMin:</strong> {timeStatus.tzOffsetMin}</div>
					{:else}
						<div class="copy-warning"><strong>status:</strong> time not set</div>
					{/if}
				</div>
				<button class="btn btn-primary btn-sm w-fit" onclick={syncTimeFromPhone} disabled={timeStatus.syncing}>
					{#if timeStatus.syncing}
						<span class="loading loading-spinner loading-sm"></span>
					{/if}
					Sync Time from Phone
				</button>
			</div>
		</div>
		
		<!-- WiFi Client (Connect to Network) -->
		<div class="surface-card">
			<div class="card-body space-y-4">
				<CardSectionHead title="WiFi Client" subtitle="Connect to an existing WiFi network.">
					<input 
						type="checkbox" 
						class="toggle toggle-primary" 
						checked={wifiStatus.enabled}
						onchange={(e) => toggleWifiClient(e.target.checked)}
					/>
				</CardSectionHead>
				
				{#if wifiStatus.enabled}
					{#if wifiStatus.state === 'connected'}
						<StatusAlert message={{ type: 'success', text: `Connected to ${wifiStatus.connectedSSID} • ${wifiStatus.ip} • ${wifiStatus.rssi} dBm` }} />
						<div class="flex gap-2">
							<button class="btn btn-outline btn-sm" onclick={disconnectWifi}>
								Disconnect
							</button>
							<button class="btn btn-error btn-outline btn-sm" onclick={forgetWifi}>
								Forget Network
							</button>
						</div>
					{:else if wifiStatus.state === 'connecting'}
						<StatusAlert message={{ type: 'info', text: `Connecting to ${wifiStatus.savedSSID}...` }} busy />
					{:else if wifiStatus.savedSSID}
						<StatusAlert message={{ type: 'warning', text: `Not connected to ${wifiStatus.savedSSID}` }} />
						<div class="flex gap-2">
							<button class="btn btn-primary btn-sm" onclick={startWifiScan}>
								Scan for Networks
							</button>
							<button class="btn btn-error btn-outline btn-sm" onclick={forgetWifi}>
								Forget Network
							</button>
						</div>
					{:else}
						<button class="btn btn-primary btn-sm" onclick={startWifiScan}>
							Scan for Networks
						</button>
					{/if}
				{:else}
					<div class="surface-note copy-muted">
						WiFi client is disabled. Enable to connect to a network.
					</div>
				{/if}
			</div>
		</div>
		
		<!-- WiFi Scan Modal -->
		{#if showWifiModal}
		<div class="modal modal-open">
			<div class="modal-box surface-modal max-w-md">
				<h3 class="font-bold text-lg">Select WiFi Network</h3>
				
				{#if wifiScanning}
					<div class="state-loading stack">
						<span class="loading loading-spinner loading-lg"></span>
						<p class="mt-4 copy-muted">Scanning for networks...</p>
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
							<StatusAlert message="This is an open network" fallbackType="warning" />
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
							<p class="state-empty center">No networks found</p>
						{:else}
							<ul class="menu surface-menu max-h-64 overflow-y-auto">
								{#each wifiNetworks as network}
									<li>
										<button onclick={() => selectNetwork(network)} class="flex justify-between">
											<span class="flex items-center gap-2">
												{#if network.secure}🔒{:else}🔓{/if}
												{network.ssid}
											</span>
											<span class="copy-muted">
												{network.rssi} dBm
											</span>
										</button>
									</li>
								{/each}
							</ul>
						{/if}
						
						<div class="flex gap-2 justify-end mt-4">
							<button class="btn btn-ghost btn-sm" onclick={startWifiScan}>
								Rescan
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
		<div class="surface-card">
			<div class="card-body space-y-4">
				<CardSectionHead title="Bluetooth Proxy" subtitle="Relay V1 data to phone apps." />
				<label class="label cursor-pointer">
					<span class="label-text">Enable Proxy</span>
					<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.proxy_ble} />
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
		<div class="surface-card">
			<div class="card-body space-y-4">
				<CardSectionHead
					title="Auto Power Off"
					subtitle="Automatically power off when V1 disconnects (e.g., when you turn off your car)."
				/>
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
		<div class="surface-card">
			<div class="card-body space-y-4">
				<CardSectionHead
					title="Backup & Restore"
					subtitle="Download your settings or restore from a backup file."
				/>
				
					<div class="flex flex-col gap-3">
						<button class="btn btn-outline btn-sm" onclick={downloadBackup}>
							Download Backup
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
							Restore from Backup
						</button>
					</div>
			</div>
		</div>
	{/if}
</div>
