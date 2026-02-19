<script>
	import { onMount, onDestroy } from 'svelte';
	import BrandMark from '$lib/components/BrandMark.svelte';

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
		satellites: 0,
		speedMph: null,
		moduleDetected: false,
		detectionTimedOut: false
	});

	let loading = $state(true);
	let error = $state(null);
	let statusInterval = null;
	let statusFetchInFlight = false;
	let gpsFetchInFlight = false;
	let statusPollTicks = 0;
	const STATUS_POLL_INTERVAL_MS = 3000;
	const GPS_POLL_MULTIPLIER = 3;

	onMount(async () => {
		await fetchStatus();
		void fetchGpsStatus();
		statusInterval = setInterval(() => {
			void fetchStatus();
			statusPollTicks += 1;
			if ((statusPollTicks % GPS_POLL_MULTIPLIER) === 0) {
				void fetchGpsStatus();
			}
		}, STATUS_POLL_INTERVAL_MS);
	});

	onDestroy(() => {
		if (statusInterval) clearInterval(statusInterval);
	});

	async function fetchStatus() {
		if (statusFetchInFlight) return;
		statusFetchInFlight = true;
		try {
			const statusRes = await fetch('/api/status');

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
			const gpsRes = await fetch('/api/gps/status');
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
</script>

<div class="page-stack">
	{#if status.alert?.active}
		<div class="alert alert-warning animate-pulse border border-warning/40" role="alert" aria-live="assertive">
			<span class="font-bold text-2xl">{status.alert.band}</span>
			<span class="text-lg ml-2">{status.alert.frequency} MHz</span>
			<span class="ml-4">Strength: {status.alert.strength}/8</span>
		</div>
	{/if}

	<div class="hero bg-base-200 rounded-box p-4">
		<div class="hero-content text-center">
			<div>
				<div class="mb-2 flex justify-center">
					<BrandMark />
				</div>
				<p class="text-xs text-base-content/50 mb-1">v{status.device?.firmware_version || '...'}</p>
				<p class="text-sm text-base-content/70">
					{#if status.wifi.sta_connected}
						{status.wifi.ssid} • {status.wifi.sta_ip}
					{:else}
						AP Mode • {status.wifi.ap_ip}
					{/if}
				</p>
			</div>
		</div>
	</div>

		<div class="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-5 gap-4">
		<div class="surface-card">
			<div class="card-body p-4">
				<h2 class="card-title text-sm">Valentine One</h2>
				{#if loading}
					<span class="loading loading-spinner loading-sm"></span>
				{:else}
					<div class="text-xl font-bold {status.v1_connected ? 'text-success' : 'text-warning'}">
						{status.v1_connected ? 'Connected' : 'Scanning...'}
					</div>
					<div class="text-xs text-base-content/60">Bluetooth LE</div>
				{/if}
			</div>
		</div>

		<div class="surface-card">
			<div class="card-body p-4">
				<h2 class="card-title text-sm">WiFi</h2>
				{#if loading}
					<span class="loading loading-spinner loading-sm"></span>
				{:else}
					<div class="text-xl font-bold {status.wifi.sta_connected ? 'text-success' : 'text-info'}">
						{status.wifi.sta_connected ? 'Online' : 'AP Only'}
					</div>
					{#if status.wifi.sta_connected}
						<div class="text-xs {getRssiClass(status.wifi.rssi)}">
							{status.wifi.ssid} • {status.wifi.rssi} dBm
						</div>
					{/if}
				{/if}
			</div>
		</div>

		<div class="surface-card">
			<div class="card-body p-4">
				<h2 class="card-title text-sm">Uptime</h2>
				{#if loading}
					<span class="loading loading-spinner loading-sm"></span>
				{:else}
					<div class="text-xl font-bold">
						{formatUptime(status.device?.uptime || 0)}
					</div>
					<div class="text-xs text-base-content/60">
						{Math.round((status.device?.heap_free || 0) / 1024)} KB free
					</div>
				{/if}
			</div>
		</div>

			<div class="surface-card">
				<div class="card-body p-4">
					<h2 class="card-title text-sm">Alerts</h2>
					{#if loading}
					<span class="loading loading-spinner loading-sm"></span>
				{:else if status.alert?.active}
					<div class="text-xl font-bold text-warning">
						{status.alert.band}
					</div>
					<div class="text-xs">{status.alert.frequency} MHz</div>
				{:else}
					<div class="text-xl font-bold text-success">Clear</div>
					<div class="text-xs text-base-content/60">No threats</div>
					{/if}
				</div>
			</div>

			<div class="surface-card">
				<div class="card-body p-4">
					<h2 class="card-title text-sm">GPS</h2>
					{#if loading}
						<span class="loading loading-spinner loading-sm"></span>
					{:else if !gps.enabled}
						<div class="text-xl font-bold text-base-content/70">Disabled</div>
						<div class="text-xs text-base-content/60">Enable in Integrations</div>
					{:else if gps.detectionTimedOut}
						<div class="text-xl font-bold text-warning">Not Found</div>
						<div class="text-xs text-base-content/60">Module timeout</div>
					{:else if gps.hasFix}
						<div class="text-xl font-bold text-success">
							{gps.satellites || 0} sats
						</div>
						<div class="text-xs text-base-content/60">
							{typeof gps.speedMph === 'number' ? `${Math.round(gps.speedMph)} mph` : 'Fix acquired'}
						</div>
					{:else if gps.moduleDetected}
						<div class="text-xl font-bold text-info">Searching</div>
						<div class="text-xs text-base-content/60">No fix yet</div>
					{:else}
						<div class="text-xl font-bold text-base-content/70">Idle</div>
						<div class="text-xs text-base-content/60">{gps.mode}</div>
					{/if}
				</div>
			</div>
		</div>

	{#if error}
		<div class="alert alert-error" role="alert">{error}</div>
	{/if}
</div>
