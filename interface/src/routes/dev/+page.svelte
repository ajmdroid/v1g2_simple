<script>
	import { onMount, onDestroy } from 'svelte';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';

	let acknowledged = $state(false);
	const DEV_WARNING_ACK_BYPASS_KEY = 'v1simple:devWarningAckBypass';
	const PASSWORD_WARNING_DISMISSED_PERSIST_KEY = 'v1simple:passwordWarningDismissedPersist';
	const PASSWORD_WARNING_DISMISSED_SESSION_KEY = 'passwordWarningDismissed';
	const PASSWORD_WARNING_EVENT = 'v1simple-password-warning-dismissed-change';

	let warningPreferences = $state({
		hidePasswordWarningBanner: false,
		hideDevWarningBanner: false
	});

	let settings = $state({
		enableWifiAtBoot: false,
		enableSignalTraceLogging: true
	});
	let loading = $state(true);
	let saving = $state(false);
	let message = $state('');

	// Performance metrics state
	let metricsExpanded = $state(false);
	let metrics = $state(null);
	let metricsLoading = $state(false);
	let metricsAutoRefresh = $state(false);
	let metricsRefreshInterval = $state(null);

	// Perf CSV file management
	let perfFiles = $state([]);
	let perfFilesLoading = $state(true);
	let perfFileActionBusy = $state('');
	let perfFilesInfo = $state({
		storageReady: false,
		onSdCard: false,
		path: '/perf'
	});

	const formatBytes = (bytes) => {
		if (!bytes) return '0 B';
		if (bytes < 1024) return `${bytes} B`;
		if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
		return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
	};

	function loadWarningPreferences() {
		if (typeof window === 'undefined') return;
		warningPreferences.hidePasswordWarningBanner = localStorage.getItem(PASSWORD_WARNING_DISMISSED_PERSIST_KEY) === '1';
		warningPreferences.hideDevWarningBanner = localStorage.getItem(DEV_WARNING_ACK_BYPASS_KEY) === '1';
		if (warningPreferences.hideDevWarningBanner) {
			acknowledged = true;
		}
	}

	function togglePasswordWarningPreference(enabled) {
		warningPreferences.hidePasswordWarningBanner = enabled;
		if (typeof window === 'undefined') return;
		if (enabled) {
			localStorage.setItem(PASSWORD_WARNING_DISMISSED_PERSIST_KEY, '1');
			sessionStorage.setItem(PASSWORD_WARNING_DISMISSED_SESSION_KEY, 'true');
		} else {
			localStorage.removeItem(PASSWORD_WARNING_DISMISSED_PERSIST_KEY);
			sessionStorage.removeItem(PASSWORD_WARNING_DISMISSED_SESSION_KEY);
		}
		window.dispatchEvent(new CustomEvent(PASSWORD_WARNING_EVENT, { detail: { dismissed: enabled } }));
	}

	function toggleDevWarningPreference(enabled) {
		warningPreferences.hideDevWarningBanner = enabled;
		if (typeof window !== 'undefined') {
			if (enabled) {
				localStorage.setItem(DEV_WARNING_ACK_BYPASS_KEY, '1');
			} else {
				localStorage.removeItem(DEV_WARNING_ACK_BYPASS_KEY);
			}
		}
		acknowledged = enabled ? true : false;
	}

	onMount(async () => {
		loadWarningPreferences();
		await Promise.all([loadSettings(), loadPerfFiles()]);
	});

	async function loadSettings() {
		try {
			const response = await fetch('/api/settings');
			const data = await response.json();
			
			settings.enableWifiAtBoot = data.enableWifiAtBoot || false;
			settings.enableSignalTraceLogging = data.enableSignalTraceLogging ?? true;
			
			loading = false;
		} catch (error) {
			console.error('Failed to load settings:', error);
			message = 'Failed to load settings';
			loading = false;
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
			params.append('enableWifiAtBoot', settings.enableWifiAtBoot.toString());
			params.append('enableSignalTraceLogging', settings.enableSignalTraceLogging.toString());
			params.append('skipPreview', 'true');

			const response = await fetch('/api/displaycolors', {
				method: 'POST',
				headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
				body: params
			});

			if (response.ok) {
				message = 'Settings saved!';
				await loadSettings();  // Reload to confirm persistence
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

	async function resetDefaults() {
		if (!acknowledged) {
			message = 'Please acknowledge the warning before resetting';
			return;
		}

		if (!confirm('Reset all development settings to defaults?')) return;

		settings.enableWifiAtBoot = false;
		settings.enableSignalTraceLogging = true;
		
		await saveSettings();
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

	async function loadPerfFiles() {
		perfFilesLoading = true;
		try {
			const response = await fetch('/api/debug/perf-files?limit=24');
			if (!response.ok) throw new Error('Failed to load perf files');
			const data = await response.json();
			perfFiles = data.files || [];
			perfFilesInfo.storageReady = data.storageReady ?? false;
			perfFilesInfo.onSdCard = data.onSdCard ?? false;
			perfFilesInfo.path = data.path || '/perf';
		} catch (error) {
			console.error('Failed to load perf files:', error);
			perfFiles = [];
			message = 'Failed to load perf files';
		} finally {
			perfFilesLoading = false;
		}
	}

	function downloadPerfFile(name) {
		if (!acknowledged) {
			message = 'Please acknowledge the warning before downloading files';
			return;
		}
		if (!name) return;
		try {
			window.open(`/api/debug/perf-files/download?name=${encodeURIComponent(name)}`, '_blank');
			message = `Downloading ${name}...`;
		} catch (error) {
			console.error('Perf file download failed:', error);
			message = `Failed to download ${name}`;
		}
	}

	async function deletePerfFile(name) {
		if (!acknowledged) {
			message = 'Please acknowledge the warning before deleting files';
			return;
		}
		if (!name) return;
		if (!confirm(`Delete ${name} from /perf?`)) return;

		perfFileActionBusy = name;
		try {
			const params = new URLSearchParams();
			params.append('name', name);
			const response = await fetch('/api/debug/perf-files/delete', {
				method: 'POST',
				headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
				body: params
			});
			const data = await response.json().catch(() => ({}));
			if (response.ok && data.success) {
				message = `Deleted ${name}`;
				await loadPerfFiles();
			} else {
				message = data.error ? `Failed to delete ${name}: ${data.error}` : `Failed to delete ${name}`;
			}
		} catch (error) {
			console.error('Perf file delete failed:', error);
			message = `Failed to delete ${name}`;
		} finally {
			perfFileActionBusy = '';
		}
	}

	onDestroy(() => {
		stopMetricsAutoRefresh();
	});
</script>

<div class="page-stack">
	<PageHeader title="Development Settings" subtitle="Advanced features and debugging tools" />

	{#if loading}
		<div class="state-loading tall">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead title="Warning Acknowledgements" subtitle="Local browser toggles for warning banners." />

				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text font-semibold">Hide default password banner</span>
							<p class="copy-caption-soft mt-1">Suppress the top security banner when default AP password is detected.</p>
						</div>
						<input
							type="checkbox"
							class="toggle toggle-primary"
							checked={warningPreferences.hidePasswordWarningBanner}
							onchange={(event) => togglePasswordWarningPreference(event.currentTarget.checked)}
						/>
					</label>
				</div>

				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text font-semibold">Auto-accept development warning</span>
							<p class="copy-caption-soft mt-1">Hide the advanced warning banner and keep dev controls unlocked.</p>
						</div>
						<input
							type="checkbox"
							class="toggle toggle-primary"
							checked={warningPreferences.hideDevWarningBanner}
							onchange={(event) => toggleDevWarningPreference(event.currentTarget.checked)}
						/>
					</label>
				</div>
				<p class="copy-micro">Stored in this browser.</p>
			</div>
		</div>

		{#if !warningPreferences.hideDevWarningBanner}
			<!-- Warning Banner -->
			<div class="surface-alert alert-warning warning-strong">
				<svg xmlns="http://www.w3.org/2000/svg" class="stroke-current shrink-0 h-6 w-6" fill="none" viewBox="0 0 24 24">
					<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z" />
				</svg>
				<div class="flex-1">
					<h3 class="font-bold">Warning: Advanced Settings</h3>
					<div class="copy-subtle">
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
		{/if}

		<!-- Message Display -->
		{#if message}
			<div class="surface-alert" class:alert-success={message.includes('saved')} class:alert-error={message.includes('Failed')}>
				<span>{message}</span>
			</div>
		{/if}

		<!-- WiFi Settings -->
		<div class="surface-card" class:opacity-50={!acknowledged}>
			<div class="card-body">
				<CardSectionHead title="WiFi & Network" />
				
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text font-semibold">Enable WiFi at Boot</span>
							<p class="copy-caption-soft mt-1">
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

				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text font-semibold">Signal Trace Logging (All Bands)</span>
							<p class="copy-caption-soft mt-1">
								Default ON. Best-effort logging of priority alerts (including Ka) to lockout CSV for bench analysis.
							</p>
						</div>
						<input
							type="checkbox"
							class="toggle toggle-primary"
							bind:checked={settings.enableSignalTraceLogging}
							disabled={!acknowledged}
						/>
					</label>
				</div>
			</div>
		</div>

		<!-- Performance Metrics -->
		<div class="surface-card" class:opacity-50={!acknowledged}>
			<div class="card-body">
				<CardSectionHead title="Performance Metrics">
					<button 
						class="btn btn-sm btn-ghost"
						onclick={() => { metricsExpanded = !metricsExpanded; if (metricsExpanded && !metrics) loadMetrics(); }}
					>
						{metricsExpanded ? 'Collapse' : 'Expand'}
					</button>
				</CardSectionHead>
				
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
								Refresh
							{/if}
						</button>
						<label class="btn btn-sm swap flex-1" class:btn-primary={metricsAutoRefresh} class:btn-outline={!metricsAutoRefresh}>
							<input type="checkbox" checked={metricsAutoRefresh} onchange={toggleMetricsAutoRefresh} />
							<span class="swap-on">Stop Auto</span>
							<span class="swap-off">Auto (2s)</span>
						</label>
						</div>

						{#if metrics}
							<!-- BLE Queue Stats -->
							<div class="surface-panel">
								<h3 class="font-semibold text-sm mb-2">BLE Queue (V1 to Display)</h3>
								<div class="grid grid-cols-2 gap-x-4 gap-y-1 text-xs">
									<div class="flex justify-between">
										<span class="copy-caption">RX Packets:</span>
										<span class="font-mono">{metrics.rxPackets?.toLocaleString() || 0}</span>
									</div>
									<div class="flex justify-between">
										<span class="copy-caption">Parse OK:</span>
										<span class="font-mono">{metrics.parseSuccesses?.toLocaleString() || 0}</span>
									</div>
									<div class="flex justify-between">
										<span class="copy-caption">Queue Drops:</span>
										<span class="font-mono" class:text-error={metrics.queueDrops > 0}>{metrics.queueDrops || 0}</span>
									</div>
									<div class="flex justify-between">
										<span class="copy-caption">Queue High-Water:</span>
										<span class="font-mono">{metrics.queueHighWater || 0}/64</span>
									</div>
								</div>
							</div>

							<!-- Display Stats -->
							<div class="surface-panel">
								<h3 class="font-semibold text-sm mb-2">Display</h3>
								<div class="grid grid-cols-2 gap-x-4 gap-y-1 text-xs">
									<div class="flex justify-between">
										<span class="copy-caption">Updates:</span>
										<span class="font-mono">{metrics.displayUpdates?.toLocaleString() || 0}</span>
									</div>
									<div class="flex justify-between">
										<span class="copy-caption">Skipped:</span>
										<span class="font-mono">{metrics.displaySkips || 0}</span>
									</div>
								</div>
							</div>

							<!-- Latency Stats (when PERF_METRICS enabled) -->
							{#if metrics.monitoringEnabled}
								<div class="surface-panel">
									<h3 class="font-semibold text-sm mb-2">BLE to Flush Latency</h3>
									<div class="grid grid-cols-3 gap-x-4 gap-y-1 text-xs">
										<div class="flex justify-between">
											<span class="copy-caption">Min:</span>
											<span class="font-mono">{formatLatency(metrics.latencyMinUs)}</span>
										</div>
										<div class="flex justify-between">
											<span class="copy-caption">Avg:</span>
											<span class="font-mono">{formatLatency(metrics.latencyAvgUs)}</span>
										</div>
										<div class="flex justify-between">
											<span class="copy-caption">Max:</span>
											<span class="font-mono" class:text-warning={metrics.latencyMaxUs > 100000}>{formatLatency(metrics.latencyMaxUs)}</span>
										</div>
									</div>
									<div class="copy-micro mt-1">
										Samples: {metrics.latencySamples?.toLocaleString() || 0} (1 in 8 packets)
									</div>
								</div>
							{/if}

							<!-- Proxy Stats -->
							{#if metrics.proxy}
								<div class="surface-panel">
									<h3 class="font-semibold text-sm mb-2">V1 Proxy (to JBV1/V1C)</h3>
									<div class="grid grid-cols-2 gap-x-4 gap-y-1 text-xs">
										<div class="flex justify-between">
											<span class="copy-caption">Connected:</span>
											<span class="font-mono" class:text-success={metrics.proxy.connected}>{metrics.proxy.connected ? 'Yes' : 'No'}</span>
										</div>
										<div class="flex justify-between">
											<span class="copy-caption">Packets Sent:</span>
											<span class="font-mono">{metrics.proxy.sendCount?.toLocaleString() || 0}</span>
										</div>
										<div class="flex justify-between">
											<span class="copy-caption">Drops:</span>
											<span class="font-mono" class:text-error={metrics.proxy.dropCount > 0}>{metrics.proxy.dropCount || 0}</span>
										</div>
										<div class="flex justify-between">
											<span class="copy-caption">Errors:</span>
											<span class="font-mono" class:text-error={metrics.proxy.errorCount > 0}>{metrics.proxy.errorCount || 0}</span>
										</div>
									</div>
								</div>
							{/if}

							<!-- Connection Stats -->
							<div class="surface-panel">
								<h3 class="font-semibold text-sm mb-2">Connection</h3>
								<div class="grid grid-cols-2 gap-x-4 gap-y-1 text-xs">
									<div class="flex justify-between">
										<span class="copy-caption">Reconnects:</span>
										<span class="font-mono">{metrics.reconnects || 0}</span>
									</div>
									<div class="flex justify-between">
										<span class="copy-caption">Disconnects:</span>
										<span class="font-mono">{metrics.disconnects || 0}</span>
									</div>
								</div>
							</div>
						{:else if metricsLoading}
							<div class="state-loading inline">
								<span class="loading loading-spinner loading-sm"></span>
							</div>
						{:else}
							<div class="text-center copy-muted py-4">
								Click Refresh or enable Auto to load metrics
							</div>
						{/if}
					</div>
				{/if}
			</div>
		</div>

		<!-- Perf CSV Files -->
		<div class="surface-card" class:opacity-50={!acknowledged}>
			<div class="card-body">
				<CardSectionHead title="Perf CSV Files">
					<button
						class="btn btn-sm btn-outline"
						onclick={loadPerfFiles}
						disabled={perfFilesLoading}
					>
						{#if perfFilesLoading}
							<span class="loading loading-spinner loading-xs"></span>
						{:else}
							Refresh
						{/if}
					</button>
				</CardSectionHead>

				<p class="copy-caption-soft">
					Files under <span class="font-mono">{perfFilesInfo.path}</span>.
					Download or delete without opening contents.
				</p>

				{#if perfFilesLoading}
					<div class="state-loading inline">
						<span class="loading loading-spinner loading-sm"></span>
					</div>
					{:else if !perfFilesInfo.storageReady || !perfFilesInfo.onSdCard}
						<div class="surface-alert alert-warning">
							<span class="copy-caption-soft">SD storage not ready. Perf CSV files are unavailable.</span>
						</div>
				{:else if perfFiles.length === 0}
					<div class="state-empty py-2">
						No perf CSV files found.
					</div>
				{:else}
					<div class="surface-table-wrap">
						<table class="table table-sm">
							<thead>
								<tr>
									<th>File</th>
									<th class="text-right">Size</th>
									<th class="text-right">Actions</th>
								</tr>
							</thead>
							<tbody>
								{#each perfFiles as file}
									<tr>
										<td class="font-mono text-xs">
											{file.name}
											{#if file.active}
												<span class="badge badge-xs badge-primary ml-2">active</span>
											{/if}
										</td>
										<td class="text-right text-xs">{formatBytes(file.sizeBytes || 0)}</td>
										<td class="text-right">
											<div class="flex justify-end gap-2">
												<button
													class="btn btn-xs btn-outline"
													onclick={() => downloadPerfFile(file.name)}
													disabled={!acknowledged || perfFileActionBusy === file.name}
												>
													Download
												</button>
												<button
													class="btn btn-xs btn-outline btn-error"
													onclick={() => deletePerfFile(file.name)}
													disabled={!acknowledged || perfFileActionBusy === file.name}
												>
													{#if perfFileActionBusy === file.name}
														<span class="loading loading-spinner loading-xs"></span>
													{:else}
														Delete
													{/if}
												</button>
											</div>
										</td>
									</tr>
								{/each}
							</tbody>
						</table>
					</div>
				{/if}
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
