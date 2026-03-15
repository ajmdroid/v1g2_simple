<script>
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';

	let {
		acknowledged,
		metrics = null,
		metricsError = null,
		metricsLoading = false,
		metricsAutoRefresh = false,
		onrefresh,
		ontoggleautorefresh
	} = $props();

	function formatLatency(us) {
		if (!us) return '-';
		if (us < 1000) return `${us}us`;
		return `${(us / 1000).toFixed(1)}ms`;
	}
</script>

<div class="space-y-4 mt-2">
	<div class="flex gap-2">
		<button
			class="btn btn-sm btn-outline flex-1"
			onclick={onrefresh}
			disabled={metricsLoading}
		>
			{#if metricsLoading}
				<span class="loading loading-spinner loading-xs"></span>
			{:else}
				Refresh
			{/if}
		</button>
		<label class="btn btn-sm swap flex-1" class:btn-primary={metricsAutoRefresh} class:btn-outline={!metricsAutoRefresh}>
			<input type="checkbox" checked={metricsAutoRefresh} onchange={ontoggleautorefresh} />
			<span class="swap-on">Stop Auto</span>
			<span class="swap-off">Auto (2s)</span>
		</label>
	</div>

	<StatusAlert message={metricsError ? { type: 'error', text: metricsError } : null} />

	{#if metrics}
		<div class="surface-panel">
			<h3 class="copy-subheading mb-2">BLE Queue (V1 to Display)</h3>
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

		<div class="surface-panel">
			<h3 class="copy-subheading mb-2">Display</h3>
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

		{#if metrics.monitoringEnabled}
			<div class="surface-panel">
				<h3 class="copy-subheading mb-2">BLE to Flush Latency</h3>
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

		{#if metrics.proxy}
			<div class="surface-panel">
				<h3 class="copy-subheading mb-2">V1 Proxy (to companion app)</h3>
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

		<div class="surface-panel">
			<h3 class="copy-subheading mb-2">Connection</h3>
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
