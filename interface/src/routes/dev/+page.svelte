<script>
	import { onMount } from 'svelte';

	let acknowledged = $state(false);
	let settings = $state({
		enableWifiAtBoot: false,
			enableDebugLogging: false,
			kittScannerEnabled: false,
			logAlerts: true,
			logWifi: true,
			logBle: false,
			logGps: false,
			logObd: false,
			logSystem: true,
			logDisplay: false
	});
	let loading = $state(true);
	let saving = $state(false);
	let message = $state('');

	let logInfo = $state({
		enabled: false,
		canEnable: false,
		exists: false,
		sizeBytes: 0,
		maxSizeBytes: 0,
		storageReady: false,
		onSdCard: false,
		path: ''
	});
	let logLoading = $state(true);
	let logActionBusy = $state(false);

	const formatBytes = (bytes) => {
		if (!bytes) return '0 B';
		if (bytes < 1024) return `${bytes} B`;
		if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
		return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
	};

	onMount(async () => {
		await Promise.all([loadSettings(), loadLogInfo()]);
	});

	async function loadSettings() {
		try {
			const response = await fetch('/api/settings');
			const data = await response.json();
			
			settings.enableWifiAtBoot = data.enableWifiAtBoot || false;
			settings.enableDebugLogging = data.enableDebugLogging || false;
			settings.kittScannerEnabled = data.kittScannerEnabled || false;
			settings.logAlerts = data.logAlerts ?? true;
			settings.logWifi = data.logWifi ?? true;
			settings.logBle = data.logBle ?? false;
			settings.logGps = data.logGps ?? false;
			settings.logObd = data.logObd ?? false;
			settings.logSystem = data.logSystem ?? true;
			settings.logDisplay = data.logDisplay ?? false;
			
			loading = false;
		} catch (error) {
			console.error('Failed to load settings:', error);
			message = 'Failed to load settings';
			loading = false;
		}
	}

	async function loadLogInfo() {
		logLoading = true;
		try {
			const response = await fetch('/api/debug/logs');
			if (!response.ok) throw new Error('Failed to load log metadata');
			const data = await response.json();

			logInfo.enabled = data.enabled ?? false;
			logInfo.canEnable = data.canEnable ?? false;
			logInfo.exists = data.exists ?? false;
			logInfo.sizeBytes = data.sizeBytes ?? 0;
			logInfo.maxSizeBytes = data.maxSizeBytes ?? 0;
			logInfo.storageReady = data.storageReady ?? false;
			logInfo.onSdCard = data.onSdCard ?? false;
			logInfo.path = data.path ?? '';
		} catch (error) {
			console.error('Failed to load log info:', error);
			message = 'Failed to load log info';
		} finally {
			logLoading = false;
		}
	}

	async function saveSettings() {
		if (!acknowledged) {
			message = 'Please acknowledge the warning before saving';
			return;
		}

		saving = true;
		message = '';

		try {
			const params = new URLSearchParams();
			params.append('enableWifiAtBoot', settings.enableWifiAtBoot);
			params.append('enableDebugLogging', settings.enableDebugLogging);
			params.append('kittScannerEnabled', settings.kittScannerEnabled);
			params.append('logAlerts', settings.logAlerts);
			params.append('logWifi', settings.logWifi);
			params.append('logBle', settings.logBle);
			params.append('logGps', settings.logGps);
			params.append('logObd', settings.logObd);
			params.append('logSystem', settings.logSystem);
			params.append('logDisplay', settings.logDisplay);

			const response = await fetch('/api/displaycolors', {
				method: 'POST',
				headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
				body: params
			});

			if (response.ok) {
				message = 'Settings saved! Changes will take effect on next reboot.';
				await loadLogInfo();
			} else {
				message = 'Failed to save settings';
			}
		} catch (error) {
			console.error('Save failed:', error);
			message = 'Failed to save settings';
		} finally {
			saving = false;
		}
	}

	async function downloadLogs() {
		if (!acknowledged) {
			message = 'Please acknowledge the warning before downloading logs';
			return;
		}

		if (!logInfo.exists) {
			message = 'No debug log found to download';
			return;
		}

		try {
			window.open('/api/debug/logs/download', '_blank');
			message = 'Downloading debug log...';
		} catch (error) {
			console.error('Download failed:', error);
			message = 'Failed to start download';
		}
	}

	async function clearLogs() {
		if (!acknowledged) {
			message = 'Please acknowledge the warning before deleting logs';
			return;
		}

		if (!confirm('Delete debug log file from storage?')) return;

		logActionBusy = true;
		try {
			const response = await fetch('/api/debug/logs/clear', { method: 'POST' });
			const data = await response.json();
			if (response.ok && data.success) {
				message = 'Debug log deleted';
			} else {
				message = 'Failed to delete log';
			}
		} catch (error) {
			console.error('Delete failed:', error);
			message = 'Failed to delete log';
		} finally {
			await loadLogInfo();
			logActionBusy = false;
		}
	}

	async function resetDefaults() {
		if (!acknowledged) {
			message = 'Please acknowledge the warning before resetting';
			return;
		}

		if (!confirm('Reset all development settings to defaults?')) return;

		settings.enableWifiAtBoot = false;
		settings.enableDebugLogging = false;
		settings.kittScannerEnabled = false;
		settings.logAlerts = true;
		settings.logWifi = true;
		settings.logBle = false;
		settings.logGps = false;
		settings.logObd = false;
		settings.logSystem = true;
		settings.logDisplay = false;
		
		await saveSettings();
	}
</script>

<div class="space-y-6">
	<!-- Header -->
	<div>
		<h1 class="text-3xl font-bold">Development Settings</h1>
		<p class="text-sm opacity-70 mt-2">Advanced features and debugging tools</p>
	</div>

	{#if loading}
		<div class="flex justify-center items-center py-12">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<!-- Warning Banner -->
		<div class="alert alert-warning shadow-lg">
			<svg xmlns="http://www.w3.org/2000/svg" class="stroke-current shrink-0 h-6 w-6" fill="none" viewBox="0 0 24 24">
				<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z" />
			</svg>
			<div class="flex-1">
				<h3 class="font-bold">⚠️ Warning: Advanced Settings</h3>
				<div class="text-sm">
					These settings can cause instability or unexpected behavior. Only modify if you know what you're doing.
				</div>
				<div class="form-control mt-2">
					<label class="label cursor-pointer justify-start gap-2">
						<input 
							type="checkbox" 
							class="checkbox checkbox-warning" 
							bind:checked={acknowledged}
						/>
						<span class="label-text font-semibold">I understand the risks</span>
					</label>
				</div>
			</div>
		</div>

		<!-- Message Display -->
		{#if message}
			<div class="alert" class:alert-success={message.includes('saved')} class:alert-error={message.includes('Failed')}>
				<span>{message}</span>
			</div>
		{/if}

		<!-- WiFi Settings -->
		<div class="card bg-base-200 shadow-xl" class:opacity-50={!acknowledged}>
			<div class="card-body">
				<h2 class="card-title">WiFi & Network</h2>
				
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text font-semibold">Enable WiFi at Boot</span>
							<p class="text-xs opacity-70 mt-1">
								Automatically start WiFi on power-up (bypasses BOOT button requirement)
							</p>
						</div>
						<input 
							type="checkbox" 
							class="toggle toggle-primary"
							bind:checked={settings.enableWifiAtBoot}
							disabled={!acknowledged}
						/>
					</label>
				</div>
			</div>
		</div>

		<!-- Debug & Logging -->
		<div class="card bg-base-200 shadow-xl" class:opacity-50={!acknowledged}>
			<div class="card-body">
				<h2 class="card-title">Debug & Logging</h2>
				
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text font-semibold">Enable Debug Logging</span>
							<p class="text-xs opacity-70 mt-1">
								{#if !logInfo.canEnable}
									<span class="text-error">⚠️ Requires SD card (not detected)</span>
								{:else}
									Write debug logs to SD card (may impact performance)
								{/if}
							</p>
						</div>
						<input 
							type="checkbox" 
							class="toggle toggle-primary"
							bind:checked={settings.enableDebugLogging}
							disabled={!acknowledged || !logInfo.canEnable}
						/>
					</label>
				</div>

				{#if settings.enableDebugLogging}
					<div class="mt-4 space-y-2">
						<h3 class="font-semibold text-sm opacity-70">Log Categories</h3>
						<p class="text-xs opacity-60">Toggle specific channels to reduce noise while debugging.</p>
						<div class="grid grid-cols-2 gap-3">
							<label class="label cursor-pointer justify-start gap-3 px-0">
								<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.logAlerts} disabled={!acknowledged}>
								<span class="label-text text-sm">Alerts</span>
							</label>
							<label class="label cursor-pointer justify-start gap-3 px-0">
								<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.logWifi} disabled={!acknowledged}>
								<span class="label-text text-sm">WiFi</span>
							</label>
							<label class="label cursor-pointer justify-start gap-3 px-0">
								<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.logBle} disabled={!acknowledged}>
								<span class="label-text text-sm">BLE</span>
							</label>
							<label class="label cursor-pointer justify-start gap-3 px-0">
								<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.logGps} disabled={!acknowledged}>
								<span class="label-text text-sm">GPS</span>
							</label>
							<label class="label cursor-pointer justify-start gap-3 px-0">
								<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.logObd} disabled={!acknowledged}>
								<span class="label-text text-sm">OBD</span>
							</label>
							<label class="label cursor-pointer justify-start gap-3 px-0">
								<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.logSystem} disabled={!acknowledged}>
								<span class="label-text text-sm">System</span>
							</label>
							<label class="label cursor-pointer justify-start gap-3 px-0">
								<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.logDisplay} disabled={!acknowledged}>
								<span class="label-text text-sm">Display</span>
							</label>
						</div>
					</div>
				{/if}

				<div class="divider"></div>

				<!-- Log Management -->
				<div class="space-y-2">
					<h3 class="font-semibold text-sm opacity-70">Log Management</h3>

					{#if logLoading}
						<div class="flex items-center justify-center py-2">
							<span class="loading loading-spinner loading-xs"></span>
						</div>
					{:else}
						<div class="text-xs opacity-70 flex items-center justify-between">
							<span>Storage: {logInfo.storageReady ? (logInfo.onSdCard ? 'SD card' : 'LittleFS') : 'Unavailable'}</span>
							<span>Size: {logInfo.exists ? formatBytes(logInfo.sizeBytes) : 'No log file'}</span>
						</div>
					{/if}

					<div class="btn-group w-full">
						<button 
							class="btn btn-sm btn-outline flex-1"
							onclick={downloadLogs}
							disabled={!acknowledged || logActionBusy || !logInfo.storageReady || !logInfo.exists}
						>
							📥 Download Logs
						</button>
						<button 
							class="btn btn-sm btn-outline btn-error flex-1"
							onclick={clearLogs}
							disabled={!acknowledged || logActionBusy || !logInfo.storageReady}
						>
							🗑️ Delete Logs
						</button>
					</div>
					<p class="text-[11px] opacity-60 text-center">
						Debug logging is {settings.enableDebugLogging ? 'enabled' : 'disabled'}; file capped at {formatBytes(logInfo.maxSizeBytes || 0)}.
					</p>
				</div>
			</div>
		</div>

		<!-- Fun & Experimental -->
		<div class="card bg-base-200 shadow-xl" class:opacity-50={!acknowledged}>
			<div class="card-body">
				<h2 class="card-title">Fun & Experimental</h2>
				
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text font-semibold">🔴 KITT Scanner Mode</span>
							<p class="text-xs opacity-70 mt-1">
								Knight Rider style alert animation (easter egg)
							</p>
						</div>
						<input 
							type="checkbox" 
							class="toggle toggle-error"
							bind:checked={settings.kittScannerEnabled}
							disabled={!acknowledged}
						/>
					</label>
				</div>
			</div>
		</div>

		<!-- Action Buttons -->
		<div class="flex gap-4">
			<button 
				class="btn btn-primary flex-1"
				onclick={saveSettings}
				disabled={!acknowledged || saving}
			>
				{#if saving}
					<span class="loading loading-spinner loading-sm"></span>
				{/if}
				Save Settings
			</button>
			<button 
				class="btn btn-outline flex-1"
				onclick={resetDefaults}
				disabled={!acknowledged || saving}
			>
				Reset to Defaults
			</button>
		</div>
	{/if}
</div>
