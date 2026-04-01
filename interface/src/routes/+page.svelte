<script>
	import { onMount } from 'svelte';
	import BrandMark from '$lib/components/BrandMark.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	import {
		retainRuntimeStatus,
		runtimeStatus,
		runtimeStatusError,
		runtimeStatusLoading
	} from '$lib/stores/runtimeStatus.svelte.js';

	onMount(() => retainRuntimeStatus({ needsStatus: true }));

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
	<PageHeader title="Dashboard" subtitle="Live system status and quick health checks." />

	{#if $runtimeStatus.alert?.active}
		<div class="surface-alert alert-warning warning-strong animate-pulse" role="alert" aria-live="assertive">
			<span class="font-bold text-2xl">{$runtimeStatus.alert.band}</span>
			<span class="text-lg ml-2">{$runtimeStatus.alert.frequency} MHz</span>
			<span class="ml-4">Strength: {$runtimeStatus.alert.strength}/8</span>
		</div>
	{/if}

	<div class="surface-hero">
		<div class="text-center">
			<div class="mb-2 flex justify-center">
				<BrandMark />
			</div>
			<p class="mb-1 copy-caption">v{$runtimeStatus.device?.firmware_version || '...'}</p>
			<p class="copy-subtle">
				{#if $runtimeStatus.wifi.sta_connected}
					{$runtimeStatus.wifi.ssid} • {$runtimeStatus.wifi.sta_ip}
				{:else}
					AP Mode • {$runtimeStatus.wifi.ap_ip}
				{/if}
			</p>
		</div>
	</div>

	<div class="grid grid-cols-1 gap-4 md:grid-cols-2 lg:grid-cols-4">
		<div class="surface-card">
			<div class="card-body">
				<div class="copy-mini-title">Valentine One</div>
				{#if $runtimeStatusLoading}
					<span class="loading loading-spinner loading-sm"></span>
				{:else}
					<div class="status-heading {$runtimeStatus.v1_connected ? 'status-heading-success' : 'status-heading-warning'}">
						{$runtimeStatus.v1_connected ? 'Connected' : 'Scanning...'}
					</div>
					<div class="copy-caption">Bluetooth LE</div>
				{/if}
			</div>
		</div>

		<div class="surface-card">
			<div class="card-body">
				<div class="copy-mini-title">WiFi</div>
				{#if $runtimeStatusLoading}
					<span class="loading loading-spinner loading-sm"></span>
				{:else}
					<div class="status-heading {$runtimeStatus.wifi.sta_connected ? 'status-heading-success' : 'status-heading-info'}">
						{$runtimeStatus.wifi.sta_connected ? 'Online' : 'AP Only'}
					</div>
					{#if $runtimeStatus.wifi.sta_connected}
						<div class="copy-caption {getRssiClass($runtimeStatus.wifi.rssi)}">
							{$runtimeStatus.wifi.ssid} • {$runtimeStatus.wifi.rssi} dBm
						</div>
					{/if}
				{/if}
			</div>
		</div>

		<div class="surface-card">
			<div class="card-body">
				<div class="copy-mini-title">Uptime</div>
				{#if $runtimeStatusLoading}
					<span class="loading loading-spinner loading-sm"></span>
				{:else}
					<div class="status-heading">
						{formatUptime($runtimeStatus.device?.uptime || 0)}
					</div>
					<div class="copy-caption">
						{Math.round(($runtimeStatus.device?.heap_free || 0) / 1024)} KB free
					</div>
				{/if}
			</div>
		</div>

		<div class="surface-card">
			<div class="card-body">
				<div class="copy-mini-title">Alerts</div>
				{#if $runtimeStatusLoading}
					<span class="loading loading-spinner loading-sm"></span>
				{:else if $runtimeStatus.alert?.active}
					<div class="status-heading-warning">
						{$runtimeStatus.alert.band}
					</div>
					<div class="copy-caption">{$runtimeStatus.alert.frequency} MHz</div>
				{:else}
					<div class="status-heading-success">Clear</div>
					<div class="copy-caption">No threats</div>
				{/if}
			</div>
		</div>
	</div>

	<StatusAlert message={$runtimeStatusError} fallbackType="error" />
</div>
