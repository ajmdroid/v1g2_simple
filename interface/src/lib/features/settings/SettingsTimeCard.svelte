<script>
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import {
		formatAgeMs,
		formatDeviceDateTime,
		getTimeConfidenceLabel,
		getTimeSourceLabel,
		projectAgeMs,
		projectEpochMs
	} from '$lib/features/settings/settingsTime.js';

	let {
		timeStatus,
		clientNowMs
	} = $props();
</script>

<div class="surface-card">
	<div class="card-body space-y-3">
		<CardSectionHead
			title="Device Time"
			subtitle="Read-only runtime clock snapshot. Time is sourced by device services, not the browser."
		/>
		<div class="copy-subtle space-y-1">
			<div><strong>timeValid:</strong> {timeStatus.valid ? 1 : 0}</div>
			<div><strong>timeSource:</strong> {timeStatus.source} ({getTimeSourceLabel(timeStatus.source)})</div>
			<div><strong>timeConfidence:</strong> {timeStatus.confidence} ({getTimeConfidenceLabel(timeStatus.confidence)})</div>
			{#if timeStatus.valid}
				<div><strong>deviceTime:</strong> <span class="font-mono">{formatDeviceDateTime(timeStatus, clientNowMs)}</span></div>
				<div><strong>timeAge:</strong> {formatAgeMs(projectAgeMs(timeStatus, clientNowMs))}</div>
				<div><strong>epochMs (projected):</strong> <span class="font-mono">{projectEpochMs(timeStatus, clientNowMs)}</span></div>
				<div><strong>tzOffsetMin:</strong> {timeStatus.tzOffsetMin}</div>
			{:else}
				<div class="copy-warning"><strong>status:</strong> time not set</div>
			{/if}
		</div>
	</div>
</div>
