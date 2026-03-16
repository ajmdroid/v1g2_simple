<script>
	import { onDestroy, onMount } from 'svelte';
	import { postSettingsForm } from '$lib/api/settings';
	import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import * as devLazyComponents from '$lib/features/dev/devLazyComponents.js';
	import DevPerfFilesPanel from '$lib/features/dev/DevPerfFilesPanel.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';

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
	let message = $state(null);

	// Performance metrics state
	let metricsExpanded = $state(false);
	let metrics = $state(null);
	let metricsError = $state(null);
	let metricsLoading = $state(false);
	let metricsAutoRefresh = $state(false);
	let DevMetricsPanelComponent = $state(null);
	let devMetricsPanelLoading = $state(false);
	const METRICS_REFRESH_INTERVAL_MS = 2000;

	// Perf CSV file management
	let perfFiles = $state([]);
	let perfFilesLoading = $state(true);
	let perfFileActionBusy = $state('');
	let perfFilesInfo = $state({
		storageReady: false,
		onSdCard: false,
		path: '/perf'
	});

	const metricsPoll = createPoll(async () => {
		await loadMetrics();
	}, METRICS_REFRESH_INTERVAL_MS);

	function setMessage(type, text) {
		message = { type, text };
	}

	function clearMessage() {
		message = null;
	}

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
			const response = await fetchWithTimeout('/api/device/settings');
			const data = await response.json();
			settings.enableWifiAtBoot = data.enableWifiAtBoot || false;
			settings.enableSignalTraceLogging = data.enableSignalTraceLogging ?? true;
			loading = false;
		} catch (error) {
			console.error('Failed to load settings:', error);
			setMessage('error', 'Failed to load settings');
			loading = false;
		}
	}

	async function saveSettings() {
		if (!acknowledged) {
			setMessage('warning', 'Please acknowledge the warning before saving');
			return;
		}

		saving = true;
		clearMessage();

		try {
			const formData = new FormData();
			formData.append('enableWifiAtBoot', settings.enableWifiAtBoot.toString());
			formData.append('enableSignalTraceLogging', settings.enableSignalTraceLogging.toString());

			const response = await postSettingsForm(formData, '/api/device/settings');

			if (response.ok) {
				setMessage('success', 'Settings saved!');
				await loadSettings(); // Reload to confirm persistence
			} else {
				setMessage('error', 'Failed to save settings');
			}
		} catch (error) {
			console.error('Save failed:', error);
			setMessage('error', 'Failed to save settings');
		} finally {
			saving = false;
		}
	}

	async function resetDefaults() {
		if (!acknowledged) {
			setMessage('warning', 'Please acknowledge the warning before resetting');
			return;
		}

		if (!confirm('Reset all development settings to defaults?')) return;

		settings.enableWifiAtBoot = false;
		settings.enableSignalTraceLogging = true;
		await saveSettings();
	}

	async function loadMetrics() {
		metricsLoading = true;
		try {
			const response = await fetchWithTimeout('/api/debug/metrics');
			if (!response.ok) throw new Error('Failed to load metrics');
			metrics = await response.json();
			metricsError = null;
		} catch (error) {
			metricsError = 'Failed to load metrics';
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
		void loadMetrics(); // Load immediately
		metricsPoll.start();
	}

	function stopMetricsAutoRefresh() {
		metricsAutoRefresh = false;
		metricsPoll.stop();
	}

	async function ensureDevMetricsPanelLoaded() {
		if (DevMetricsPanelComponent || devMetricsPanelLoading) return;
		devMetricsPanelLoading = true;
		try {
			const mod = await devLazyComponents.loadDevMetricsPanel();
			DevMetricsPanelComponent = mod.default;
		} catch (error) {
			console.error('Failed to load metrics panel:', error);
			setMessage('error', 'Failed to load metrics panel');
			metricsExpanded = false;
		} finally {
			devMetricsPanelLoading = false;
		}
	}

	function toggleMetricsExpanded() {
		metricsExpanded = !metricsExpanded;
		if (!metricsExpanded) return;
		void ensureDevMetricsPanelLoaded();
		if (!metrics) {
			void loadMetrics();
		}
	}

	async function loadPerfFiles() {
		perfFilesLoading = true;
		try {
			const response = await fetchWithTimeout('/api/debug/perf-files?limit=24');
			if (!response.ok) throw new Error('Failed to load perf files');
			const data = await response.json();
			perfFiles = data.files || [];
			perfFilesInfo.storageReady = data.storageReady ?? false;
			perfFilesInfo.onSdCard = data.onSdCard ?? false;
			perfFilesInfo.path = data.path || '/perf';
		} catch (error) {
			console.error('Failed to load perf files:', error);
			perfFiles = [];
			setMessage('error', 'Failed to load perf files');
		} finally {
			perfFilesLoading = false;
		}
	}

	function downloadPerfFile(name) {
		if (!acknowledged) {
			setMessage('warning', 'Please acknowledge the warning before downloading files');
			return;
		}
		if (!name) return;
		try {
			window.open(`/api/debug/perf-files/download?name=${encodeURIComponent(name)}`, '_blank');
			setMessage('info', `Downloading ${name}...`);
		} catch (error) {
			console.error('Perf file download failed:', error);
			setMessage('error', `Failed to download ${name}`);
		}
	}

	async function deletePerfFile(name) {
		if (!acknowledged) {
			setMessage('warning', 'Please acknowledge the warning before deleting files');
			return;
		}
		if (!name) return;
		if (!confirm(`Delete ${name} from /perf?`)) return;

		perfFileActionBusy = name;
		try {
			const params = new URLSearchParams();
			params.append('name', name);
			const response = await fetchWithTimeout('/api/debug/perf-files/delete', {
				method: 'POST',
				headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
				body: params
			});
			const data = await response.json().catch(() => ({}));
			if (response.ok && data.success) {
				setMessage('success', `Deleted ${name}`);
				await loadPerfFiles();
			} else {
				setMessage('error', data.error ? `Failed to delete ${name}: ${data.error}` : `Failed to delete ${name}`);
			}
		} catch (error) {
			console.error('Perf file delete failed:', error);
			setMessage('error', `Failed to delete ${name}`);
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
		<StatusAlert message={message} />

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
								Default ON. Best-effort logging of all active V1 alerts (including Ka) to lockout CSV for bench analysis.
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
						onclick={toggleMetricsExpanded}
					>
						{metricsExpanded ? 'Collapse' : 'Expand'}
					</button>
				</CardSectionHead>
				
				{#if metricsExpanded}
					{#if DevMetricsPanelComponent}
						<DevMetricsPanelComponent
							{acknowledged}
							{metrics}
							{metricsError}
							{metricsLoading}
							{metricsAutoRefresh}
							onrefresh={loadMetrics}
							ontoggleautorefresh={toggleMetricsAutoRefresh}
						/>
					{:else if devMetricsPanelLoading}
						<div class="state-loading stack mt-2">
							<span class="loading loading-spinner loading-md"></span>
							<p class="copy-muted">Loading metrics panel...</p>
						</div>
					{/if}
				{/if}
			</div>
		</div>

		<DevPerfFilesPanel
			{acknowledged}
			{perfFiles}
			{perfFilesLoading}
			{perfFileActionBusy}
			{perfFilesInfo}
			onrefresh={loadPerfFiles}
			ondownload={downloadPerfFile}
			ondelete={deletePerfFile}
		/>

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
