<script>
	import { onMount } from 'svelte';
	
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
	
	let settings = $state({
		enableMultiAlert: true
	});
	
	let loading = $state(true);
	let error = $state(null);
	let savingMultiAlert = $state(false);
	
	onMount(async () => {
		await Promise.all([fetchStatus(), fetchSettings()]);
		// Poll status every 2 seconds for responsive alerts
		const interval = setInterval(fetchStatus, 2000);
		return () => clearInterval(interval);
	});
	
	async function fetchStatus() {
		try {
			const res = await fetch('/api/status');
			if (res.ok) {
				const data = await res.json();
				status = data;
				error = null;
			} else {
				error = 'API error';
			}
		} catch (e) {
			error = 'Connection lost';
		} finally {
			loading = false;
		}
	}
	
	async function fetchSettings() {
		try {
			const res = await fetch('/api/settings');
			if (res.ok) {
				const data = await res.json();
				settings = { ...settings, ...data };
			}
		} catch (e) {
			// Silent fail - settings will use defaults
		}
	}
	
	async function toggleMultiAlert() {
		savingMultiAlert = true;
		try {
			const formData = new FormData();
			formData.append('enableMultiAlert', !settings.enableMultiAlert);
			
			const res = await fetch('/api/settings', {
				method: 'POST',
				body: formData
			});
			
			if (res.ok) {
				settings.enableMultiAlert = !settings.enableMultiAlert;
			}
		} catch (e) {
			error = 'Failed to save setting';
		} finally {
			savingMultiAlert = false;
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

<div class="space-y-6">
	<!-- Alert Banner (shown when active) -->
	{#if status.alert?.active}
		<div class="alert alert-warning shadow-lg animate-pulse" role="alert" aria-live="assertive">
			<svg xmlns="http://www.w3.org/2000/svg" class="stroke-current shrink-0 h-6 w-6" fill="none" viewBox="0 0 24 24" aria-hidden="true">
				<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z" />
			</svg>
			<div>
				<span class="font-bold text-2xl">{status.alert.band}</span>
				<span class="text-lg ml-2">{status.alert.frequency} MHz</span>
				<span class="ml-4">Strength: {status.alert.strength}/8</span>
			</div>
		</div>
	{/if}

	<!-- Header -->
	<div class="hero bg-base-200 rounded-box p-4">
		<div class="hero-content text-center">
			<div>
				<h1 class="text-3xl font-bold text-primary">V1 Gen2 Display</h1>
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

	<!-- Status Cards -->
	<div class="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-4 gap-4">
		<!-- V1 Connection -->
		<div class="card bg-base-200 shadow-xl">
			<div class="card-body p-4">
				<h2 class="card-title text-sm">
					<span class="text-xl">📡</span>
					Valentine One
				</h2>
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

		<!-- WiFi Status -->
		<div class="card bg-base-200 shadow-xl">
			<div class="card-body p-4">
				<h2 class="card-title text-sm">
					<span class="text-xl">📶</span>
					WiFi
				</h2>
				{#if loading}
					<span class="loading loading-spinner loading-sm"></span>
				{:else}
					<div class="text-xl font-bold {status.wifi.sta_connected ? 'text-success' : 'text-info'}">
						{status.wifi.sta_connected ? 'Online' : 'AP Only'}
					</div>
					{#if status.wifi.sta_connected}
						<div class="text-xs {getRssiClass(status.wifi.rssi)}">
							{status.wifi.rssi} dBm
						</div>
					{/if}
				{/if}
			</div>
		</div>

		<!-- Uptime -->
		<div class="card bg-base-200 shadow-xl">
			<div class="card-body p-4">
				<h2 class="card-title text-sm">
					<span class="text-xl">⏱️</span>
					Uptime
				</h2>
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

		<!-- Alert Status -->
		<div class="card bg-base-200 shadow-xl">
			<div class="card-body p-4">
				<h2 class="card-title text-sm">
					<span class="text-xl">🚨</span>
					Alerts
				</h2>
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
	</div>

	<!-- Quick Actions -->
	<div class="card bg-base-200 shadow-xl">
		<div class="card-body p-4">
			<h2 class="card-title text-sm mb-2">Quick Actions</h2>
			<div class="flex flex-wrap gap-2">
				<a href="/autopush" class="btn btn-accent btn-sm">🚗 Auto-Push</a>
				<a href="/profiles" class="btn btn-primary btn-sm">📊 Profiles</a>
				<a href="/devices" class="btn btn-secondary btn-sm">📡 Saved V1s</a>
				<a href="/colors" class="btn btn-info btn-sm">🎨 Colors</a>
				<a href="/settings" class="btn btn-ghost btn-sm">⚙️ Settings</a>
			</div>
		</div>
	</div>

	<!-- Display Options -->
	<div class="card bg-base-200 shadow-xl">
		<div class="card-body p-4">
			<h2 class="card-title text-sm mb-2">Display Options</h2>
			<label class="label cursor-pointer justify-start gap-3">
				<input 
					type="checkbox" 
					class="toggle toggle-primary" 
					checked={settings.enableMultiAlert}
					onchange={toggleMultiAlert}
					disabled={savingMultiAlert}
				/>
				<div>
					<span class="label-text font-medium">Multi-Alert Cards</span>
					<p class="text-xs text-base-content/60">Show secondary alerts as mini cards at bottom</p>
				</div>
			</label>
		</div>
	</div>

	<!-- Error Toast -->
	{#if error}
		<div class="toast toast-end">
			<div class="alert alert-error">
				<span>{error}</span>
			</div>
		</div>
	{/if}
</div>
