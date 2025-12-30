<script>
	import { onMount } from 'svelte';
	
	let alerts = $state([]);
	let loading = $state(true);
	let error = $state(null);
	let filter = $state('all');
	
	onMount(async () => {
		await fetchAlerts();
	});
	
	async function fetchAlerts() {
		loading = true;
		try {
			const res = await fetch('/api/alerts');
			if (res.ok) {
				const data = await res.json();
				alerts = data.alerts || [];
				error = null;
			} else {
				error = 'Failed to load alerts';
			}
		} catch (e) {
			error = 'Connection error';
		} finally {
			loading = false;
		}
	}
	
	async function clearAlerts() {
		if (confirm('Clear all alerts from the log?')) {
			try {
				await fetch('/api/alerts/clear', { method: 'POST' });
				alerts = [];
			} catch (e) {
				error = 'Failed to clear';
			}
		}
	}
	
	function getBandClass(band) {
		switch (band) {
			case 'Ka': return 'badge-error';
			case 'K': return 'badge-warning';
			case 'X': return 'badge-info';
			case 'LASER': return 'badge-secondary';
			default: return 'badge-ghost';
		}
	}
	
	function formatTime(timestamp) {
		if (!timestamp) return '';
		const date = new Date(timestamp * 1000);
		return date.toLocaleTimeString();
	}
	
	function formatDate(timestamp) {
		if (!timestamp) return '';
		const date = new Date(timestamp * 1000);
		return date.toLocaleDateString();
	}
	
	let filteredAlerts = $derived(
		filter === 'all' ? alerts : alerts.filter(a => a.band === filter)
	);
</script>

<div class="space-y-4">
	<div class="flex justify-between items-center">
		<h1 class="text-2xl font-bold">Alert Log</h1>
		<div class="flex gap-2">
			<button class="btn btn-sm btn-outline" onclick={fetchAlerts}>
				🔄 Refresh
			</button>
			<button class="btn btn-sm btn-error btn-outline" onclick={clearAlerts}>
				🗑️ Clear
			</button>
		</div>
	</div>
	
	<!-- Filter Tabs -->
	<div class="tabs tabs-boxed bg-base-200">
		<button class="tab {filter === 'all' ? 'tab-active' : ''}" onclick={() => filter = 'all'}>
			All
		</button>
		<button class="tab {filter === 'Ka' ? 'tab-active' : ''}" onclick={() => filter = 'Ka'}>
			Ka
		</button>
		<button class="tab {filter === 'K' ? 'tab-active' : ''}" onclick={() => filter = 'K'}>
			K
		</button>
		<button class="tab {filter === 'X' ? 'tab-active' : ''}" onclick={() => filter = 'X'}>
			X
		</button>
		<button class="tab {filter === 'LASER' ? 'tab-active' : ''}" onclick={() => filter = 'LASER'}>
			Laser
		</button>
	</div>

	{#if loading}
		<div class="flex justify-center p-8">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else if error}
		<div class="alert alert-error">
			<span>{error}</span>
		</div>
	{:else if filteredAlerts.length === 0}
		<div class="card bg-base-200">
			<div class="card-body text-center">
				<p class="text-base-content/60">No alerts recorded</p>
			</div>
		</div>
	{:else}
		<div class="overflow-x-auto">
			<table class="table table-zebra">
				<thead>
					<tr>
						<th>Time</th>
						<th>Band</th>
						<th>Frequency</th>
						<th>Strength</th>
						<th>Direction</th>
					</tr>
				</thead>
				<tbody>
					{#each filteredAlerts as alert}
						<tr>
							<td>
								<div class="text-sm">{formatTime(alert.timestamp)}</div>
								<div class="text-xs text-base-content/50">{formatDate(alert.timestamp)}</div>
							</td>
							<td>
								<span class="badge {getBandClass(alert.band)}">{alert.band}</span>
							</td>
							<td>{alert.frequency} MHz</td>
							<td>
								<div class="flex items-center gap-1">
									{#each Array(8) as _, i}
										<div class="w-2 h-4 rounded {i < alert.strength ? 'bg-primary' : 'bg-base-300'}"></div>
									{/each}
								</div>
							</td>
							<td>
								{#if alert.direction === 0}
									⬆️ Front
								{:else if alert.direction === 1}
									⬇️ Rear
								{:else}
									↔️ Side
								{/if}
							</td>
						</tr>
					{/each}
				</tbody>
			</table>
		</div>
		
		<div class="text-sm text-base-content/50 text-center">
			Showing {filteredAlerts.length} of {alerts.length} alerts
		</div>
	{/if}
</div>
