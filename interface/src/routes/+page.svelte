<script>
	import { onMount, onDestroy } from 'svelte';
	import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
	import BrandMark from '$lib/components/BrandMark.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';

	let status = $state({
		wifi: {
			sta_connected: false,
			ap_active: false,
			sta_ip: '',
			ap_ip: '',
			ssid: '',
			rssi: 0
		},
		device: {
			uptime: 0,
			heap_free: 0,
			hostname: 'v1g2'
		},
		v1_connected: false,
		alert: null
	});
	let gps = $state({
		enabled: false,
		runtimeEnabled: false,
		mode: 'scaffold',
		hasFix: false,
		stableHasFix: false,
		satellites: 0,
		stableSatellites: 0,
		speedMph: null,
		hdop: null,
		moduleDetected: false,
		detectionTimedOut: false
	});

	let loading = $state(true);
	let error = $state(null);
	let statusFetchInFlight = false;
	let gpsFetchInFlight = false;
	let statusPollTicks = 0;
	const STATUS_POLL_INTERVAL_MS = 3000;
	const GPS_POLL_MULTIPLIER = 3;

	const statusPoll = createPoll(async () => {
		await fetchStatus();
		statusPollTicks += 1;
		if ((statusPollTicks % GPS_POLL_MULTIPLIER) === 0) {
			await fetchGpsStatus();
		}
	}, STATUS_POLL_INTERVAL_MS);

	onMount(async () => {
		await fetchStatus();
		void fetchGpsStatus();
		statusPoll.start();
	});

	onDestroy(() => statusPoll.stop());

	async function fetchStatus() {
		if (statusFetchInFlight) return;
		statusFetchInFlight = true;
		try {
			const statusRes = await fetchWithTimeout('/api/status');

			if (statusRes.ok) {
				status = await statusRes.json();
				error = null;
			} else {
				error = 'API error';
			}
		} catch (e) {
			error = 'Connection lost';
		} finally {
			loading = false;
			statusFetchInFlight = false;
		}
	}

	async function fetchGpsStatus() {
		if (gpsFetchInFlight) return;
		gpsFetchInFlight = true;
		try {
			const gpsRes = await fetchWithTimeout('/api/gps/status');
			if (!gpsRes.ok) return;
			const gpsData = await gpsRes.json();
			gps = { ...gps, ...gpsData };
		} catch (e) {
			// Keep dashboard rendering from status endpoint if GPS poll fails.
		} finally {
			gpsFetchInFlight = false;
		}
	}

	function formatUptime(seconds) {
		const d = Math.floor(seconds / 86400);
		const h = Math.floor((seconds % 86400) / 3600);
		const m = Math.floor((seconds % 3600) / 60);
		if (d > 0) return `${d}d ${h}h ${m}m`;
		if (h > 0) return `${h}h ${m}m`;
		return `${m}m`;
	}

	function getRssiClass(rssi) {
		if (rssi >= -50) return 'text-success';
		if (rssi >= -70) return 'text-warning';
		return 'text-error';
	}

	function gpsHasFixStable() {
		return (typeof gps.stableHasFix === 'boolean') ? gps.stableHasFix : !!gps.hasFix;
	}

	function gpsSatCountStable() {
		if (typeof gps.stableSatellites === 'number') return gps.stableSatellites;
		return gps.satellites || 0;
	}
</script>

<div class="page-stack">
	<PageHeader title="Dashboard" subtitle="Live system status and quick health checks." />

	{#if status.alert?.active}
		<div class="surface-alert alert-warning warning-strong animate-pulse" role="alert" aria-live="assertive">
			<span class="font-bold text-2xl">{status.alert.band}</span>
			<span class="text-lg ml-2">{status.alert.frequency} MHz</span>
			<span class="ml-4">Strength: {status.alert.strength}/8</span>
		</div>
	{/if}

	<div class="surface-hero">
		<div class="text-center">
			<div class="mb-2 flex justify-center">
				<BrandMark />
			</div>
			<p class="mb-1 copy-caption">v{status.device?.firmware_version || '...'}</p>
			<p class="copy-subtle">
				{#if status.wifi.sta_connected}
					{status.wifi.ssid} • {status.wifi.sta_ip}
				{:else}
					AP Mode • {status.wifi.ap_ip}
				{/if}
			</p>
		</div>
	</div>

	<div class="grid grid-cols-1 gap-4 md:grid-cols-2 lg:grid-cols-5">
		<div class="surface-card">
			<div class="card-body">
				<div class="copy-mini-title">Valentine One</div>
				{#if loading}
					<span class="loading loading-spinner loading-sm"></span>
				{:else}
					<div class="status-heading {status.v1_connected ? 'status-heading-success' : 'status-heading-warning'}">
						{status.v1_connected ? 'Connected' : 'Scanning...'}
					</div>
					<div class="copy-caption">Bluetooth LE</div>
				{/if}
			</div>
		</div>

		<div class="surface-card">
			<div class="card-body">
				<div class="copy-mini-title">WiFi</div>
				{#if loading}
					<span class="loading loading-spinner loading-sm"></span>
				{:else}
					<div class="status-heading {status.wifi.sta_connected ? 'status-heading-success' : 'status-heading-info'}">
						{status.wifi.sta_connected ? 'Online' : 'AP Only'}
					</div>
					{#if status.wifi.sta_connected}
						<div class="copy-caption {getRssiClass(status.wifi.rssi)}">
							{status.wifi.ssid} • {status.wifi.rssi} dBm
						</div>
					{/if}
				{/if}
			</div>
		</div>

		<div class="surface-card">
			<div class="card-body">
				<div class="copy-mini-title">Uptime</div>
				{#if loading}
					<span class="loading loading-spinner loading-sm"></span>
				{:else}
					<div class="status-heading">
						{formatUptime(status.device?.uptime || 0)}
					</div>
					<div class="copy-caption">
						{Math.round((status.device?.heap_free || 0) / 1024)} KB free
					</div>
				{/if}
			</div>
		</div>

		<div class="surface-card">
			<div class="card-body">
				<div class="copy-mini-title">Alerts</div>
				{#if loading}
					<span class="loading loading-spinner loading-sm"></span>
				{:else if status.alert?.active}
					<div class="status-heading-warning">
						{status.alert.band}
					</div>
					<div class="copy-caption">{status.alert.frequency} MHz</div>
				{:else}
					<div class="status-heading-success">Clear</div>
					<div class="copy-caption">No threats</div>
				{/if}
			</div>
		</div>

		<div class="surface-card">
			<div class="card-body">
				<div class="copy-mini-title">GPS</div>
				{#if loading}
					<span class="loading loading-spinner loading-sm"></span>
				{:else if !gps.enabled}
					<div class="status-heading-muted">Disabled</div>
					<div class="copy-caption">Enable in Integrations</div>
				{:else if gps.detectionTimedOut}
					<div class="status-heading-warning">Not Found</div>
					<div class="copy-caption">Module timeout</div>
					{:else if gpsHasFixStable()}
						<div class="status-heading-success">
							{gpsSatCountStable()} sats
						</div>
					<div class="copy-caption">
						{typeof gps.hdop === 'number' ? `HDOP ${gps.hdop.toFixed(1)}` : 'Fix acquired'}
					</div>
				{:else if gps.moduleDetected}
					<div class="status-heading-info">Searching</div>
					<div class="copy-caption">No fix yet</div>
				{:else}
					<div class="status-heading-muted">Idle</div>
					<div class="copy-caption">{gps.mode}</div>
				{/if}
			</div>
		</div>
	</div>

	<StatusAlert message={error} fallbackType="error" />
</div>
