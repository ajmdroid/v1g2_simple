<script>
	import { onMount } from 'svelte';
	import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
	import { postSettingsForm } from '$lib/api/settings';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import SettingsAutoPowerOffCard from '$lib/features/settings/SettingsAutoPowerOffCard.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import SettingsBackupCard from '$lib/features/settings/SettingsBackupCard.svelte';
	import * as settingsLazyComponents from '$lib/features/settings/settingsLazyComponents.js';
	import SettingsTimeCard from '$lib/features/settings/SettingsTimeCard.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	import {
		retainRuntimeStatus,
		runtimeStatus
	} from '$lib/stores/runtimeStatus.svelte.js';

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
	let backingUpNow = $state(false);

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
	let wifiConnectPollStartedMs = $state(0);
	let clientNowMs = $state(Date.now());
	let wifiStatusFetchInFlight = false;
	let SettingsWifiModalComponent = $state(null);
	let wifiModalLoading = $state(false);
	const WIFI_STATUS_ERROR_TEXT = 'Failed to load WiFi status';
	const WIFI_SCAN_ERROR_TEXT = 'Failed to update WiFi scan';
	const WIFI_CONNECT_TIMEOUT_TEXT = 'Wi-Fi connection timed out. Check status and retry.';
	const RECOGNIZED_BACKUP_TYPES = new Set(['v1simple_backup', 'v1simple_sd_backup']);
	const WIFI_CONNECT_POLL_INTERVAL_MS = 1000;
	const WIFI_CONNECT_TIMEOUT_MS = 30000;
	const TIME_TICK_INTERVAL_MS = 1000;
	const timeTickPoll = createPoll(async () => {
		clientNowMs = Date.now();
	}, TIME_TICK_INTERVAL_MS);

	let timeStatus = $state({
		valid: false,
		source: 0,
		confidence: 0,
		epochMs: 0,
		tzOffsetMin: 0,
		ageMs: 0,
		sampleClientMs: 0
	});

	function applyTimeStatus(snapshot, sampleClientMs = Date.now()) {
		const t = snapshot || {};
		timeStatus.valid = !!t.valid;
		timeStatus.source = Number(t.source || 0);
		timeStatus.confidence = Number(t.confidence || 0);
		timeStatus.epochMs = Number(t.epochMs || 0);
		timeStatus.tzOffsetMin = Number(t.tzOffsetMin ?? t.tzOffsetMinutes ?? 0);
		timeStatus.ageMs = Number(t.ageMs || 0);
		timeStatus.sampleClientMs = timeStatus.valid ? sampleClientMs : 0;
	}

	onMount(async () => {
		const releaseRuntimeStatus = retainRuntimeStatus({ needsStatus: true });
		const unsubscribeRuntimeStatus = runtimeStatus.subscribe((status) => {
			applyTimeStatus(status?.time);
		});
		await fetchSettings();
		await fetchWifiStatus();
		timeTickPoll.start();

		return () => {
			stopWifiPoll();
			timeTickPoll.stop();
			unsubscribeRuntimeStatus();
			releaseRuntimeStatus();
		};
	});

	function stopWifiPoll() {
		if (!wifiPoll) return;
		wifiPoll.stop();
		wifiPoll = null;
		wifiConnectPollStartedMs = 0;
	}

	function finishWifiConnectAttempt({ closeModal = false } = {}) {
		stopWifiPoll();
		wifiConnecting = false;

		if (closeModal) {
			showWifiModal = false;
			selectedNetwork = null;
			wifiPassword = '';
		}
	}

	function isWifiConnectTerminalState(state) {
		return ['connected', 'failed', 'disabled', 'disconnected', 'unknown'].includes(state);
	}

	function clearMessageText(text) {
		if (message?.text === text) {
			message = null;
		}
	}

	function getBackupValidationError(text) {
		let parsed;

		try {
			parsed = JSON.parse(text);
		} catch (_error) {
			return 'Selected file is not valid JSON.';
		}

		if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
			return 'Backup file must contain a JSON object.';
		}

		if (!RECOGNIZED_BACKUP_TYPES.has(parsed._type)) {
			return 'Selected file is not a V1 Simple settings backup.';
		}

		return null;
	}
	
	async function fetchSettings() {
		try {
			const res = await fetchWithTimeout('/api/device/settings');
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
				clearMessageText(WIFI_STATUS_ERROR_TEXT);
			} else {
				message = { type: 'error', text: WIFI_STATUS_ERROR_TEXT };
			}
		} catch (e) {
			message = { type: 'error', text: WIFI_STATUS_ERROR_TEXT };
		} finally {
			wifiStatusFetchInFlight = false;
		}
	}

	async function startWifiScan() {
		wifiScanning = true;
		wifiNetworks = [];
		showWifiModal = true;
		void ensureWifiModalLoaded();
		
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
				clearMessageText(WIFI_SCAN_ERROR_TEXT);
				if (!data.scanning && data.networks.length > 0) {
					wifiNetworks = data.networks;
					wifiScanning = false;
					stopWifiPoll();
				} else if (!data.scanning) {
					// Scan complete but no networks
					wifiScanning = false;
					stopWifiPoll();
				}
			} else {
				message = { type: 'error', text: WIFI_SCAN_ERROR_TEXT };
			}
		} catch (e) {
			message = { type: 'error', text: WIFI_SCAN_ERROR_TEXT };
		}
		
		// Also update status
		await fetchWifiStatus();
	}
	
	function selectNetwork(network) {
		selectedNetwork = network;
		wifiPassword = '';
	}
	
	async function connectToNetwork() {
		if (!selectedNetwork || wifiConnecting || (selectedNetwork.secure && !wifiPassword)) return;

		const ssid = selectedNetwork.ssid;
		const password = wifiPassword;
		wifiConnecting = true;

		try {
			const res = await fetchWithTimeout('/api/wifi/connect', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ ssid, password })
			});

			if (!res.ok) {
				message = { type: 'error', text: 'Failed to initiate connection' };
				return;
			}

			message = { type: 'success', text: `Connecting to ${ssid}...` };
			stopWifiPoll();
			wifiConnectPollStartedMs = Date.now();
			wifiPoll = createPoll(async () => {
				await fetchWifiStatus();

				if (wifiStatus.state === 'connected') {
					finishWifiConnectAttempt({ closeModal: true });
					message = { type: 'success', text: `Connected to ${wifiStatus.connectedSSID}!` };
				} else if (wifiStatus.state === 'failed') {
					finishWifiConnectAttempt();
					message = { type: 'error', text: 'Connection failed. Check password.' };
				} else if (
					isWifiConnectTerminalState(wifiStatus.state) ||
					Date.now() - wifiConnectPollStartedMs >= WIFI_CONNECT_TIMEOUT_MS
				) {
					finishWifiConnectAttempt();
					message = { type: 'error', text: WIFI_CONNECT_TIMEOUT_TEXT };
				}
			}, WIFI_CONNECT_POLL_INTERVAL_MS);
			wifiPoll.start();
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		} finally {
			if (!wifiPoll) {
				wifiConnecting = false;
			}
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
		if (wifiConnecting) return;

		showWifiModal = false;
		selectedNetwork = null;
		wifiPassword = '';
		stopWifiPoll();
	}

	async function ensureWifiModalLoaded() {
		if (SettingsWifiModalComponent || wifiModalLoading) return;
		wifiModalLoading = true;
		try {
			const module = await settingsLazyComponents.loadSettingsWifiModal();
			SettingsWifiModalComponent = module.default;
		} catch (error) {
			closeWifiModal();
			message = { type: 'error', text: 'Failed to load WiFi modal' };
		} finally {
			wifiModalLoading = false;
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

			
			const res = await postSettingsForm(formData, '/api/device/settings');
			
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

	async function backupNowToSd() {
		backingUpNow = true;
		try {
			const res = await fetchWithTimeout('/api/settings/backup-now', { method: 'POST' });
			const data = await res.json().catch(() => ({}));
			if (res.ok && data.success) {
				message = { type: 'success', text: data.message || 'Backup saved to SD card.' };
			} else {
				message = { type: 'error', text: data.error || 'Failed to save backup to SD' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		} finally {
			backingUpNow = false;
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
			const validationError = getBackupValidationError(text);
			if (validationError) {
				message = { type: 'error', text: validationError };
				return;
			}

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

		<SettingsTimeCard
			{timeStatus}
			{clientNowMs}
		/>
		
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
		
			{#if showWifiModal}
				{#if SettingsWifiModalComponent}
					<SettingsWifiModalComponent
						open={showWifiModal}
						{wifiScanning}
						{wifiNetworks}
						bind:selectedNetwork
						bind:wifiPassword
						{wifiConnecting}
						onstartWifiScan={startWifiScan}
						onselectNetwork={selectNetwork}
						onconnectToNetwork={connectToNetwork}
						oncloseWifiModal={closeWifiModal}
					/>
				{:else if wifiModalLoading}
					<div class="modal modal-open">
						<div class="modal-box surface-modal max-w-md">
							<div class="state-loading stack">
								<span class="loading loading-spinner loading-md"></span>
								<p class="copy-muted">Loading WiFi modal...</p>
							</div>
						</div>
					</div>
				{/if}
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
		
			<SettingsAutoPowerOffCard {settings} />

		
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
		
			<SettingsBackupCard
				{backingUpNow}
				onbackupNowToSd={backupNowToSd}
				ondownloadBackup={downloadBackup}
				{restoreFile}
				{restoring}
				onfileSelect={handleFileSelect}
				onrestoreBackup={restoreBackup}
			/>
		{/if}
	</div>
