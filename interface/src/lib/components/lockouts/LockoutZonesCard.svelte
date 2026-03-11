<script>
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	import { formatFrequencyMhz } from '$lib/utils/format';
	import {
		formatBandMask,
		formatCoordinate,
		formatDirectionSummary,
		formatEpochMs,
		formatZoneRadiusFeet,
		mapHrefFromZone
	} from '$lib/utils/lockout';

	let {
		advancedUnlocked,
		zoneEditorSaving,
		importingZones,
		exportingZones,
		clearingAllZones,
		lockoutZonesLoading,
		lockoutZonesError,
		lockoutZonesStats,
		activeLockoutZones = [],
		pendingLockoutZones = [],
		importFileInput = $bindable(),
		promptLockoutImport,
		exportLockoutZones,
		refreshZones,
		clearAllZones,
		handleImportFileSelected,
		deleteZone,
		deletingZoneSlot = null
	} = $props();

	function formatRoundedFrequencyMhz(mhz) {
		return formatFrequencyMhz(mhz, { roundMhz: true });
	}
</script>

<div class="surface-card">
	<div class="card-body gap-3">
		<CardSectionHead
			title="Lockout Zones"
			subtitle="Active learned lockouts and pending candidates. Manual create/update has been removed."
		>
			<div class="flex flex-wrap gap-2">
				<button
					class="btn btn-outline btn-sm"
					onclick={promptLockoutImport}
					disabled={!advancedUnlocked || importingZones || zoneEditorSaving}
				>
					{#if importingZones}
						<span class="loading loading-spinner loading-xs"></span>
					{/if}
					Import
				</button>
				<button class="btn btn-outline btn-sm" onclick={exportLockoutZones} disabled={exportingZones}>
					{#if exportingZones}
						<span class="loading loading-spinner loading-xs"></span>
					{/if}
					Export
				</button>
				<button class="btn btn-outline btn-sm" onclick={refreshZones} disabled={lockoutZonesLoading}>
					{#if lockoutZonesLoading}
						<span class="loading loading-spinner loading-xs"></span>
					{/if}
					Refresh
				</button>
				<button
					class="btn btn-outline btn-error btn-sm"
					onclick={clearAllZones}
					disabled={!advancedUnlocked || clearingAllZones || importingZones || (lockoutZonesStats.activeCount === 0 && lockoutZonesStats.pendingCount === 0)}
				>
					{#if clearingAllZones}
						<span class="loading loading-spinner loading-xs"></span>
					{/if}
					Clear All
				</button>
				<input
					type="file"
					accept=".json,application/json"
					class="hidden"
					bind:this={importFileInput}
					onchange={handleImportFileSelected}
				/>
			</div>
		</CardSectionHead>

		{#if lockoutZonesError}
			<StatusAlert message={lockoutZonesError} fallbackType="warning" />
		{/if}

		<div class="surface-stats">
			<div class="stat py-3 px-4">
				<div class="stat-title">Active</div>
				<div class="stat-value text-base">{lockoutZonesStats.activeReturned}/{lockoutZonesStats.activeCount}</div>
				<div class="stat-desc">showing/total zones</div>
			</div>
			<div class="stat py-3 px-4">
				<div class="stat-title">Pending</div>
				<div class="stat-value text-base">{lockoutZonesStats.pendingReturned}/{lockoutZonesStats.pendingCount}</div>
				<div class="stat-desc">showing/total candidates</div>
			</div>
			<div class="stat py-3 px-4">
				<div class="stat-title">Promotion Hits</div>
				<div class="stat-value text-base">{lockoutZonesStats.promotionHits || '—'}</div>
				<div class="stat-desc">candidate threshold</div>
			</div>
		</div>

		{#if lockoutZonesLoading}
			<div class="state-loading tight">
				<span class="loading loading-spinner loading-md"></span>
			</div>
		{:else}
			<div class="grid grid-cols-1 gap-4">
				<div class="surface-table-wrap">
					<div class="copy-mini-title">Active Zones</div>
					{#if activeLockoutZones.length === 0}
						<div class="state-empty">No active lockout zones.</div>
					{:else}
						<table class="table table-sm min-w-[1120px]">
							<thead>
								<tr>
									<th>Slot</th>
									<th>Source</th>
									<th>Controls</th>
									<th>Band</th>
									<th>Freq</th>
									<th>Conf</th>
									<th>Radius</th>
									<th>Direction</th>
									<th>Auto-Remove</th>
									<th>Location</th>
								</tr>
							</thead>
							<tbody>
								{#each activeLockoutZones as zone}
									<tr>
										<td class="font-mono text-xs">{zone.slot}</td>
										<td class="text-xs">
											<div class="flex flex-wrap gap-1">
												{#if zone.learned}
													<div class="badge badge-info badge-outline badge-xs">learned</div>
												{/if}
												{#if !zone.learned}
													<div class="badge badge-ghost badge-xs">active</div>
												{/if}
											</div>
										</td>
										<td class="text-xs">
											<button
												class="btn btn-xs btn-error btn-outline"
												onclick={() => deleteZone(zone)}
												disabled={!advancedUnlocked || deletingZoneSlot === zone.slot}
											>
												{#if deletingZoneSlot === zone.slot}
													<span class="loading loading-spinner loading-xs"></span>
												{/if}
												Delete
											</button>
										</td>
										<td>{formatBandMask(zone.bandMask)}</td>
										<td class="whitespace-nowrap">{formatRoundedFrequencyMhz(zone.frequencyMHz)}</td>
										<td>{typeof zone.confidence === 'number' ? zone.confidence : '—'}</td>
										<td class="whitespace-nowrap">{formatZoneRadiusFeet(zone)}</td>
										<td class="text-xs whitespace-nowrap">{formatDirectionSummary(zone)}</td>
										<td class="text-xs">
											{#if typeof zone.demotionMissThreshold === 'number'}
												{zone.missCount ?? 0}/{zone.demotionMissThreshold} misses
												{#if typeof zone.demotionMissesRemaining === 'number'}
													({zone.demotionMissesRemaining} to remove)
												{/if}
											{:else}
												disabled
											{/if}
										</td>
										<td>
											<div class="font-mono text-xs">
												{formatCoordinate(zone.latitude)}, {formatCoordinate(zone.longitude)}
											</div>
											{#if mapHrefFromZone(zone)}
												<a
													class="link link-primary text-xs"
													href={mapHrefFromZone(zone)}
													target="_blank"
													rel="noopener noreferrer"
												>
													map
												</a>
											{/if}
										</td>
									</tr>
								{/each}
							</tbody>
						</table>
					{/if}
				</div>

				<div class="surface-table-wrap">
					<div class="copy-mini-title">Pending Candidates</div>
					{#if pendingLockoutZones.length === 0}
						<div class="state-empty">No pending candidates.</div>
					{:else}
						<table class="table table-sm min-w-[860px]">
							<thead>
								<tr>
									<th>Slot</th>
									<th>Band</th>
									<th>Freq</th>
									<th>Hits</th>
									<th>Remaining</th>
									<th>Last Seen</th>
									<th>Next Hit</th>
									<th>Location</th>
								</tr>
							</thead>
							<tbody>
								{#each pendingLockoutZones as zone}
									<tr>
										<td class="font-mono text-xs">{zone.slot}</td>
										<td>{zone.band || 'UNK'}</td>
										<td class="whitespace-nowrap">{formatRoundedFrequencyMhz(zone.frequencyMHz)}</td>
										<td>{typeof zone.hitCount === 'number' ? zone.hitCount : '—'}</td>
										<td>{typeof zone.hitsRemaining === 'number' ? zone.hitsRemaining : '—'}</td>
										<td class="text-xs">{formatEpochMs(zone.lastSeenMs)}</td>
										<td class="text-xs">{formatEpochMs(zone.nextEligibleHitMs)}</td>
										<td>
											<div class="font-mono text-xs">
												{formatCoordinate(zone.latitude)}, {formatCoordinate(zone.longitude)}
											</div>
											<a
												class="link link-primary text-xs"
												href={`https://maps.google.com/?q=${zone.latitude},${zone.longitude}`}
												target="_blank"
												rel="noopener noreferrer"
											>
												map
											</a>
										</td>
									</tr>
								{/each}
							</tbody>
						</table>
					{/if}
				</div>
			</div>
		{/if}
	</div>
</div>
