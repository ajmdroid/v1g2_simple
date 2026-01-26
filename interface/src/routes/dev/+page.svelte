<script>
	import { onMount, onDestroy } from 'svelte';

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
			logDisplay: false,
			logPerfMetrics: false,
			logAudio: false,
			logCamera: false,
			logLockout: false,
			logTouch: false
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

	// Log viewer state
	let logViewerOpen = $state(false);
	let logContent = $state('');
	let logViewerLoading = $state(false);
	let logAutoRefresh = $state(false);
	let logRefreshInterval = $state(null);
	let logFilterText = $state('');

	// Performance metrics state
	let metricsExpanded = $state(false);
	let metrics = $state(null);
	let metricsLoading = $state(false);
	let metricsAutoRefresh = $state(false);
	let metricsRefreshInterval = $state(null);

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
			settings.logPerfMetrics = data.logPerfMetrics ?? false;
			settings.logAudio = data.logAudio ?? false;
			settings.logCamera = data.logCamera ?? false;
			settings.logLockout = data.logLockout ?? false;
			settings.logTouch = data.logTouch ?? false;
			
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
			params.append('logPerfMetrics', settings.logPerfMetrics);
			params.append('logAudio', settings.logAudio);
			params.append('logCamera', settings.logCamera);
			params.append('logLockout', settings.logLockout);
			params.append('logTouch', settings.logTouch);

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
		settings.logPerfMetrics = false;
		settings.logAudio = false;
		settings.logCamera = false;
		settings.logLockout = false;
		settings.logTouch = false;
		
		await saveSettings();
	}

	// Log viewer functions
	async function loadLogContent() {
		logViewerLoading = true;
		try {
			const response = await fetch('/api/debug/logs/tail?bytes=32768');
			if (!response.ok) throw new Error('Failed to load logs');
			const data = await response.json();
			logContent = data.content || '[No content]';
			// Update log info while we're at it
			logInfo.exists = data.exists;
			logInfo.sizeBytes = data.totalSize;
		} catch (error) {
			console.error('Failed to load log content:', error);
			logContent = '[Error loading logs]';
		} finally {
			logViewerLoading = false;
		}
	}

	function openLogViewer() {
		logViewerOpen = true;
		loadLogContent();
	}

	function closeLogViewer() {
		logViewerOpen = false;
		stopAutoRefresh();
	}

	function toggleAutoRefresh() {
		if (logAutoRefresh) {
			stopAutoRefresh();
		} else {
			startAutoRefresh();
		}
	}

	function startAutoRefresh() {
		logAutoRefresh = true;
		logRefreshInterval = setInterval(() => {
			loadLogContent();
		}, 3000);  // Refresh every 3 seconds
	}

	function stopAutoRefresh() {
		logAutoRefresh = false;
		if (logRefreshInterval) {
			clearInterval(logRefreshInterval);
			logRefreshInterval = null;
		}
	}

	// Filtered log content based on search
	function getFilteredContent() {
		if (!logFilterText.trim()) return logContent;
		const lines = logContent.split('\n');
		const filtered = lines.filter(line => 
			line.toLowerCase().includes(logFilterText.toLowerCase())
		);
		return filtered.join('\n') || '[No matching lines]';
	}

	// Performance metrics functions
	async function loadMetrics() {
		metricsLoading = true;
		try {
			const response = await fetch('/api/debug/metrics');
			if (!response.ok) throw new Error('Failed to load metrics');
			metrics = await response.json();
		} catch (error) {
			console.error('Failed to load metrics:', error);
		} finally {
			metricsLoading = false;
		}
	}

	function toggleMetricsAutoRefresh() {
		if (metricsAutoRefresh) {
			stopMetricsAutoRefresh();
		} else {
			startMetricsAutoRefresh();
		}
	}

	function startMetricsAutoRefresh() {
		metricsAutoRefresh = true;
		loadMetrics();  // Load immediately
		metricsRefreshInterval = setInterval(() => {
			loadMetrics();
		}, 2000);  // Refresh every 2 seconds
	}

	function stopMetricsAutoRefresh() {
		metricsAutoRefresh = false;
		if (metricsRefreshInterval) {
			clearInterval(metricsRefreshInterval);
			metricsRefreshInterval = null;
		}
	}

	function formatLatency(us) {
		if (!us) return '-';
		if (us < 1000) return `${us}µs`;
		return `${(us / 1000).toFixed(1)}ms`;
	}

	onDestroy(() => {
		stopAutoRefresh();
		stopMetricsAutoRefresh();
	});
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
							<label class="label cursor-pointer justify-start gap-3 px-0">
								<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.logPerfMetrics} disabled={!acknowledged}>
								<span class="label-text text-sm">Perf Metrics</span>
							</label>
							<label class="label cursor-pointer justify-start gap-3 px-0">
								<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.logAudio} disabled={!acknowledged}>
								<span class="label-text text-sm">Audio</span>
							</label>
							<label class="label cursor-pointer justify-start gap-3 px-0">
								<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.logCamera} disabled={!acknowledged}>
								<span class="label-text text-sm">Camera</span>
							</label>
							<label class="label cursor-pointer justify-start gap-3 px-0">
								<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.logLockout} disabled={!acknowledged}>
								<span class="label-text text-sm">Lockout</span>
							</label>
							<label class="label cursor-pointer justify-start gap-3 px-0">
								<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.logTouch} disabled={!acknowledged}>
								<span class="label-text text-sm">Touch</span>
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
					
					<!-- View Logs Button -->
					<button 
						class="btn btn-sm btn-primary w-full"
						onclick={openLogViewer}
						disabled={!acknowledged || !logInfo.storageReady || !logInfo.exists}
					>
						👁️ View Logs
					</button>
					
					<p class="text-[11px] opacity-60 text-center">
						Debug logging is {settings.enableDebugLogging ? 'enabled' : 'disabled'}; file capped at {formatBytes(logInfo.maxSizeBytes || 0)}.
					</p>
				</div>
			</div>
		</div>

		<!-- Performance Metrics -->
		<div class="card bg-base-200 shadow-xl" class:opacity-50={!acknowledged}>
			<div class="card-body">
				<div class="flex items-center justify-between">
					<h2 class="card-title">📊 Performance Metrics</h2>
					<button 
						class="btn btn-sm btn-ghost"
						onclick={() => { metricsExpanded = !metricsExpanded; if (metricsExpanded && !metrics) loadMetrics(); }}
					>
						{metricsExpanded ? '▼' : '▶'}
					</button>
				</div>
				
				{#if metricsExpanded}
					<div class="space-y-4 mt-2">
						<!-- Controls -->
						<div class="flex gap-2">
							<button 
								class="btn btn-sm btn-outline flex-1"
								onclick={loadMetrics}
								disabled={metricsLoading}
							>
								{#if metricsLoading}
									<span class="loading loading-spinner loading-xs"></span>
								{:else}
									🔄 Refresh
								{/if}
							</button>
							<label class="btn btn-sm swap flex-1" class:btn-primary={metricsAutoRefresh} class:btn-outline={!metricsAutoRefresh}>
								<input type="checkbox" checked={metricsAutoRefresh} onchange={toggleMetricsAutoRefresh} />
								<span class="swap-on">⏸️ Stop Auto</span>
								<span class="swap-off">▶️ Auto (2s)</span>
							</label>
						</div>

						{#if metrics}
							<!-- BLE Queue Stats -->
							<div class="bg-base-300 rounded-lg p-3">
								<h3 class="font-semibold text-sm mb-2">📡 BLE Queue (V1→Display)</h3>
								<div class="grid grid-cols-2 gap-x-4 gap-y-1 text-xs">
									<div class="flex justify-between">
										<span class="opacity-70">RX Packets:</span>
										<span class="font-mono">{metrics.rxPackets?.toLocaleString() || 0}</span>
									</div>
									<div class="flex justify-between">
										<span class="opacity-70">Parse OK:</span>
										<span class="font-mono">{metrics.parseSuccesses?.toLocaleString() || 0}</span>
									</div>
									<div class="flex justify-between">
										<span class="opacity-70">Queue Drops:</span>
										<span class="font-mono" class:text-error={metrics.queueDrops > 0}>{metrics.queueDrops || 0}</span>
									</div>
									<div class="flex justify-between">
										<span class="opacity-70">Queue High-Water:</span>
										<span class="font-mono">{metrics.queueHighWater || 0}/64</span>
									</div>
								</div>
							</div>

							<!-- Display Stats -->
							<div class="bg-base-300 rounded-lg p-3">
								<h3 class="font-semibold text-sm mb-2">🖥️ Display</h3>
								<div class="grid grid-cols-2 gap-x-4 gap-y-1 text-xs">
									<div class="flex justify-between">
										<span class="opacity-70">Updates:</span>
										<span class="font-mono">{metrics.displayUpdates?.toLocaleString() || 0}</span>
									</div>
									<div class="flex justify-between">
										<span class="opacity-70">Skipped:</span>
										<span class="font-mono">{metrics.displaySkips || 0}</span>
									</div>
								</div>
							</div>

							<!-- Latency Stats (when PERF_METRICS enabled) -->
							{#if metrics.monitoringEnabled}
								<div class="bg-base-300 rounded-lg p-3">
									<h3 class="font-semibold text-sm mb-2">⏱️ BLE→Flush Latency</h3>
									<div class="grid grid-cols-3 gap-x-4 gap-y-1 text-xs">
										<div class="flex justify-between">
											<span class="opacity-70">Min:</span>
											<span class="font-mono">{formatLatency(metrics.latencyMinUs)}</span>
										</div>
										<div class="flex justify-between">
											<span class="opacity-70">Avg:</span>
											<span class="font-mono">{formatLatency(metrics.latencyAvgUs)}</span>
										</div>
										<div class="flex justify-between">
											<span class="opacity-70">Max:</span>
											<span class="font-mono" class:text-warning={metrics.latencyMaxUs > 100000}>{formatLatency(metrics.latencyMaxUs)}</span>
										</div>
									</div>
									<div class="text-[10px] opacity-50 mt-1">
										Samples: {metrics.latencySamples?.toLocaleString() || 0} (1 in 8 packets)
									</div>
								</div>
							{/if}

							<!-- Proxy Stats -->
							{#if metrics.proxy}
								<div class="bg-base-300 rounded-lg p-3">
									<h3 class="font-semibold text-sm mb-2">📲 V1 Proxy (to JBV1/V1C)</h3>
									<div class="grid grid-cols-2 gap-x-4 gap-y-1 text-xs">
										<div class="flex justify-between">
											<span class="opacity-70">Connected:</span>
											<span class="font-mono" class:text-success={metrics.proxy.connected}>{metrics.proxy.connected ? 'Yes' : 'No'}</span>
										</div>
										<div class="flex justify-between">
											<span class="opacity-70">Packets Sent:</span>
											<span class="font-mono">{metrics.proxy.sendCount?.toLocaleString() || 0}</span>
										</div>
										<div class="flex justify-between">
											<span class="opacity-70">Drops:</span>
											<span class="font-mono" class:text-error={metrics.proxy.dropCount > 0}>{metrics.proxy.dropCount || 0}</span>
										</div>
										<div class="flex justify-between">
											<span class="opacity-70">Errors:</span>
											<span class="font-mono" class:text-error={metrics.proxy.errorCount > 0}>{metrics.proxy.errorCount || 0}</span>
										</div>
									</div>
								</div>
							{/if}

							<!-- Connection Stats -->
							<div class="bg-base-300 rounded-lg p-3">
								<h3 class="font-semibold text-sm mb-2">🔗 Connection</h3>
								<div class="grid grid-cols-2 gap-x-4 gap-y-1 text-xs">
									<div class="flex justify-between">
										<span class="opacity-70">Reconnects:</span>
										<span class="font-mono">{metrics.reconnects || 0}</span>
									</div>
									<div class="flex justify-between">
										<span class="opacity-70">Disconnects:</span>
										<span class="font-mono">{metrics.disconnects || 0}</span>
									</div>
								</div>
							</div>
						{:else if metricsLoading}
							<div class="flex items-center justify-center py-4">
								<span class="loading loading-spinner loading-sm"></span>
							</div>
						{:else}
							<div class="text-center text-sm opacity-60 py-4">
								Click Refresh or enable Auto to load metrics
							</div>
						{/if}
					</div>
				{/if}
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

<!-- Log Viewer Modal -->
{#if logViewerOpen}
	<div class="modal modal-open">
		<div class="modal-box max-w-4xl w-full h-[80vh] flex flex-col">
			<div class="flex items-center justify-between mb-4">
				<h3 class="font-bold text-lg">📋 Log Viewer</h3>
				<button class="btn btn-sm btn-circle btn-ghost" onclick={closeLogViewer}>✕</button>
			</div>
			
			<!-- Controls -->
			<div class="flex flex-wrap gap-2 mb-3">
				<input 
					type="text" 
					placeholder="Filter logs..." 
					class="input input-sm input-bordered flex-1 min-w-[150px]"
					bind:value={logFilterText}
				/>
				<button 
					class="btn btn-sm btn-outline"
					onclick={loadLogContent}
					disabled={logViewerLoading}
				>
					{#if logViewerLoading}
						<span class="loading loading-spinner loading-xs"></span>
					{:else}
						🔄 Refresh
					{/if}
				</button>
				<label class="btn btn-sm swap" class:btn-primary={logAutoRefresh} class:btn-outline={!logAutoRefresh}>
					<input type="checkbox" checked={logAutoRefresh} onchange={toggleAutoRefresh} />
					<span class="swap-on">⏸️ Auto</span>
					<span class="swap-off">▶️ Auto</span>
				</label>
			</div>
			
			<!-- Size info -->
			<div class="text-xs opacity-60 mb-2">
				Showing last ~32KB • Total size: {formatBytes(logInfo.sizeBytes)}
				{#if logFilterText}
					• Filtered by: "{logFilterText}"
				{/if}
			</div>
			
			<!-- Log content -->
			<div class="flex-1 overflow-auto bg-base-300 rounded-lg p-3 font-mono text-xs">
				{#if logViewerLoading && !logContent}
					<div class="flex items-center justify-center h-full">
						<span class="loading loading-spinner loading-lg"></span>
					</div>
				{:else}
					<pre class="whitespace-pre-wrap break-words">{getFilteredContent()}</pre>
				{/if}
			</div>
			
			<!-- Actions -->
			<div class="modal-action">
				<button class="btn btn-sm btn-outline" onclick={downloadLogs}>
					📥 Download Full Log
				</button>
				<button class="btn btn-sm" onclick={closeLogViewer}>Close</button>
			</div>
		</div>
		<div class="modal-backdrop bg-black/50" onclick={closeLogViewer}></div>
	</div>
{/if}