<script>
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	import { formatFrequencyMhz } from '$lib/utils/format';
	import {
		formatBootTime,
		formatCoordinate,
		formatFixAgeMs,
		formatHdop,
		GPS_MIN_SATELLITES,
		signalMapHref
	} from '$lib/utils/lockout';

	let {
		loading,
		lockoutLoading,
		lockoutError,
		lockoutStats,
		lockoutEvents = [],
		lockoutConfig,
		advancedUnlocked,
		zoneEditorSaving,
		refreshEvents,
	} = $props();

	function formatRoundedFrequencyMhz(mhz) {
		return formatFrequencyMhz(mhz, { roundMhz: true });
	}
</script>

<div class="surface-card">
	<div class="card-body gap-3">
		<CardSectionHead
			title="Signal Observations"
			subtitle="Recent signals seen during driving. These observations feed the area learner; manual lockout writes are disabled."
		>
			<button class="btn btn-outline btn-sm" onclick={refreshEvents} disabled={lockoutLoading}>
				{#if lockoutLoading}
					<span class="loading loading-spinner loading-xs"></span>
				{/if}
				Refresh
			</button>
		</CardSectionHead>

		{#if lockoutError}
			<StatusAlert message={lockoutError} fallbackType="warning" />
		{/if}

		<div class="surface-stats">
			<div class="stat py-3 px-4">
				<div class="stat-title">Buffer</div>
				<div class="stat-value text-base">{lockoutStats.size}/{lockoutStats.capacity || '—'}</div>
				<div class="stat-desc">recent observations</div>
			</div>
			<div class="stat py-3 px-4">
				<div class="stat-title">Published</div>
				<div class="stat-value text-base">{lockoutStats.published}</div>
				<div class="stat-desc">since boot</div>
			</div>
			<div class="stat py-3 px-4">
				<div class="stat-title">Drops</div>
				<div class="stat-value text-base">{lockoutStats.drops}</div>
				<div class="stat-desc">oldest overwritten</div>
			</div>
			<div class="stat py-3 px-4">
				<div class="stat-title">Latest</div>
				<div class="stat-value text-base">{lockoutEvents[0]?.band || '—'}</div>
				<div class="stat-desc">
					{lockoutEvents[0]
						? `${formatRoundedFrequencyMhz(lockoutEvents[0].frequencyMHz)} • fix age ${formatFixAgeMs(lockoutEvents[0].fixAgeMs)}`
						: 'no samples yet'}
				</div>
			</div>
		</div>

		{#if loading || lockoutLoading}
			<div class="state-loading tight">
				<span class="loading loading-spinner loading-md"></span>
			</div>
		{:else if lockoutEvents.length === 0}
			<div class="state-empty">No candidates logged yet. Run a drive test, then refresh this card.</div>
		{:else}
			<div class="surface-table-wrap">
				<table class="table table-sm">
					<thead>
						<tr>
							<th>Seen (boot)</th>
							<th>Band</th>
							<th>Frequency</th>
							<th>Strength</th>
							<th>Fix Age</th>
							<th>Sats</th>
							<th>HDOP</th>
							<th>Location</th>
						</tr>
					</thead>
					<tbody>
						{#each lockoutEvents as event}
							<tr>
								<td class="font-mono text-xs">{formatBootTime(event.tsMs)}</td>
								<td>{event.band || 'UNK'}</td>
								<td>{formatRoundedFrequencyMhz(event.frequencyMHz)}</td>
								<td>{typeof event.strength === 'number' ? event.strength : '—'}</td>
								<td>{formatFixAgeMs(event.fixAgeMs)}</td>
								<td class:text-error={typeof event.satellites === 'number' && event.satellites < GPS_MIN_SATELLITES}>
									{typeof event.satellites === 'number' ? event.satellites : '—'}
								</td>
								<td class:text-error={typeof event.hdop === 'number' && event.hdop > lockoutConfig.maxHdopX10 / 10}>
									{formatHdop(event.hdop)}
								</td>
								<td>
									{#if event.locationValid}
										<div class="font-mono text-xs">
											{formatCoordinate(event.latitude)}, {formatCoordinate(event.longitude)}
										</div>
										{#if signalMapHref(event)}
											<a class="link link-primary text-xs" href={signalMapHref(event)} target="_blank" rel="noopener noreferrer">
												map
											</a>
										{/if}
									{:else}
										<span class="copy-caption">no fix</span>
									{/if}
								</td>
							</tr>
						{/each}
					</tbody>
				</table>
			</div>
		{/if}
	</div>
</div>
