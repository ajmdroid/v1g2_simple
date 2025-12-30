<script>
	import { onMount } from 'svelte';
	
	let status = $state({
		connected: false,
		bleConnected: false,
		rssi: 0,
		uptime: 0,
		freeHeap: 0
	});
	
	let loading = $state(true);
	
	onMount(async () => {
		await fetchStatus();
		// Poll status every 5 seconds
		const interval = setInterval(fetchStatus, 5000);
		return () => clearInterval(interval);
	});
	
	async function fetchStatus() {
		try {
			const res = await fetch('/api/status');
			if (res.ok) {
				const data = await res.json();
				status = { ...status, ...data, connected: true };
			}
		} catch (e) {
			status.connected = false;
		} finally {
			loading = false;
		}
	}
	
	function formatUptime(seconds) {
		const h = Math.floor(seconds / 3600);
		const m = Math.floor((seconds % 3600) / 60);
		const s = seconds % 60;
		return `${h}h ${m}m ${s}s`;
	}
</script>

<div class="space-y-6">
	<!-- Header -->
	<div class="hero bg-base-200 rounded-box p-6">
		<div class="hero-content text-center">
			<div>
				<h1 class="text-4xl font-bold text-primary">V1 Gen2 Display</h1>
				<p class="py-4 text-base-content/70">Valentine One Radar Detector Interface</p>
			</div>
		</div>
	</div>

	<!-- Status Cards -->
	<div class="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
		<!-- Connection Status -->
		<div class="card bg-base-200 shadow-xl">
			<div class="card-body">
				<h2 class="card-title">
					<svg xmlns="http://www.w3.org/2000/svg" class="h-6 w-6" fill="none" viewBox="0 0 24 24" stroke="currentColor">
						<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8.111 16.404a5.5 5.5 0 017.778 0M12 20h.01m-7.08-7.071c3.904-3.905 10.236-3.905 14.141 0M1.394 9.393c5.857-5.857 15.355-5.857 21.213 0" />
					</svg>
					WiFi Status
				</h2>
				{#if loading}
					<span class="loading loading-spinner loading-md"></span>
				{:else}
					<div class="stat-value text-lg {status.connected ? 'text-success' : 'text-error'}">
						{status.connected ? 'Connected' : 'Disconnected'}
					</div>
					{#if status.rssi}
						<div class="stat-desc">Signal: {status.rssi} dBm</div>
					{/if}
				{/if}
			</div>
		</div>

		<!-- BLE Status -->
		<div class="card bg-base-200 shadow-xl">
			<div class="card-body">
				<h2 class="card-title">
					<svg xmlns="http://www.w3.org/2000/svg" class="h-6 w-6" fill="none" viewBox="0 0 24 24" stroke="currentColor">
						<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9.75 17L9 20l-1 1h8l-1-1-.75-3M3 13h18M5 17h14a2 2 0 002-2V5a2 2 0 00-2-2H5a2 2 0 00-2 2v10a2 2 0 002 2z" />
					</svg>
					V1 Connection
				</h2>
				{#if loading}
					<span class="loading loading-spinner loading-md"></span>
				{:else}
					<div class="stat-value text-lg {status.bleConnected ? 'text-success' : 'text-warning'}">
						{status.bleConnected ? 'Connected' : 'Searching...'}
					</div>
					<div class="stat-desc">Bluetooth LE</div>
				{/if}
			</div>
		</div>

		<!-- System Info -->
		<div class="card bg-base-200 shadow-xl">
			<div class="card-body">
				<h2 class="card-title">
					<svg xmlns="http://www.w3.org/2000/svg" class="h-6 w-6" fill="none" viewBox="0 0 24 24" stroke="currentColor">
						<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 3v2m6-2v2M9 19v2m6-2v2M5 9H3m2 6H3m18-6h-2m2 6h-2M7 19h10a2 2 0 002-2V7a2 2 0 00-2-2H7a2 2 0 00-2 2v10a2 2 0 002 2zM9 9h6v6H9V9z" />
					</svg>
					System
				</h2>
				{#if loading}
					<span class="loading loading-spinner loading-md"></span>
				{:else}
					<div class="text-sm space-y-1">
						<div>Uptime: {formatUptime(status.uptime || 0)}</div>
						<div>Free RAM: {Math.round((status.freeHeap || 0) / 1024)} KB</div>
					</div>
				{/if}
			</div>
		</div>
	</div>

	<!-- Quick Actions -->
	<div class="card bg-base-200 shadow-xl">
		<div class="card-body">
			<h2 class="card-title">Quick Actions</h2>
			<div class="flex flex-wrap gap-2">
				<a href="/settings" class="btn btn-primary btn-sm">WiFi Settings</a>
				<a href="/logs" class="btn btn-secondary btn-sm">View Logs</a>
				<a href="/profiles" class="btn btn-accent btn-sm">V1 Profiles</a>
				<button class="btn btn-outline btn-sm" onclick={() => document.getElementById('mute-modal').showModal()}>
					Mute V1
				</button>
			</div>
		</div>
	</div>
</div>

<!-- Mute Modal -->
<dialog id="mute-modal" class="modal">
	<div class="modal-box">
		<h3 class="font-bold text-lg">Mute Valentine One</h3>
		<p class="py-4">Send mute command to the V1?</p>
		<div class="modal-action">
			<form method="dialog">
				<button class="btn btn-ghost">Cancel</button>
				<button class="btn btn-primary" onclick={() => fetch('/mute', {method: 'POST'})}>Mute</button>
			</form>
		</div>
	</div>
</dialog>
