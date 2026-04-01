<script>
	import { onMount } from 'svelte';
	import { fetchWithTimeout } from '$lib/utils/poll';
	import { formatFrequencyMhz } from '$lib/utils/format';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	import SettingsObdCard from '$lib/features/settings/SettingsObdCard.svelte';
	import {
		runtimeGpsError,
		fetchRuntimeGpsStatus,
		retainRuntimeStatus,
		runtimeGpsStatus
	} from '$lib/stores/runtimeStatus.svelte.js';

	let message = $state(null);
	let savingGpsEnabled = $state(false);

	const GPS_POLL_INTERVAL_MS = 2500;

	onMount(() => retainRuntimeStatus({ gpsPollIntervalMs: GPS_POLL_INTERVAL_MS }));

	function setMsg(type, text) {
		message = { type, text };
	}

	function gpsHasFixStable() {
		return (typeof $runtimeGpsStatus.stableHasFix === 'boolean') ? $runtimeGpsStatus.stableHasFix : !!$runtimeGpsStatus.hasFix;
	}

	function gpsSatellitesStable() {
		if (typeof $runtimeGpsStatus.stableSatellites === 'number') return $runtimeGpsStatus.stableSatellites;
		return $runtimeGpsStatus.satellites || 0;
	}

	async function toggleGpsEnabled(enabled) {
		if (savingGpsEnabled) return;
		const previous = !!$runtimeGpsStatus.enabled;
		runtimeGpsStatus.update((current) => ({ ...current, enabled }));
		savingGpsEnabled = true;

		try {
			const res = await fetchWithTimeout('/api/gps/config', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ enabled })
			});
			const data = await res.json().catch(() => ({}));
			if (!res.ok) {
				setMsg('error', data.message || 'Failed to update GPS setting');
				runtimeGpsStatus.update((current) => ({ ...current, enabled: previous }));
				return;
			}

			setMsg('success', `GPS ${enabled ? 'enabled' : 'disabled'}`);
			await fetchRuntimeGpsStatus();
		} catch (e) {
			runtimeGpsStatus.update((current) => ({ ...current, enabled: previous }));
			setMsg('error', 'Failed to update GPS setting');
		} finally {
			savingGpsEnabled = false;
		}
	}

</script>

<div class="page-stack">
	<PageHeader title="Integrations" subtitle="GPS runtime controls and connectivity status." />

	<StatusAlert {message} />
	<StatusAlert message={$runtimeGpsError ? { type: 'error', text: $runtimeGpsError } : null} />

	<div class="surface-card">
		<div class="card-body gap-3">
			<CardSectionHead
				title="GPS Runtime"
				subtitle="GPS provides fix/location telemetry."
			>
				<label class="label cursor-pointer justify-start gap-3 py-0">
					<span class="label-text">Enabled</span>
					<input
						type="checkbox"
						class="toggle toggle-primary"
						checked={!!$runtimeGpsStatus.enabled}
						onchange={(e) => toggleGpsEnabled(e.currentTarget.checked)}
						disabled={savingGpsEnabled}
					/>
				</label>
			</CardSectionHead>

			<div class="surface-stats">
				<div class="stat py-3 px-4">
					<div class="stat-title">Mode</div>
					<div class="stat-value text-base">{$runtimeGpsStatus.mode || 'scaffold'}</div>
					<div class="stat-desc">{$runtimeGpsStatus.runtimeEnabled ? 'runtime active' : 'runtime idle'}</div>
				</div>
					<div class="stat py-3 px-4">
						<div class="stat-title">Fix</div>
						<div class="stat-value text-base">{gpsHasFixStable() ? 'Yes' : 'No'}</div>
						<div class="stat-desc">{$runtimeGpsStatus.moduleDetected ? 'module detected' : 'waiting for module'}</div>
					</div>
					<div class="stat py-3 px-4">
						<div class="stat-title">Satellites</div>
						<div class="stat-value text-base">{gpsSatellitesStable()}</div>
						<div class="stat-desc">{$runtimeGpsStatus.parserActive ? 'parser active' : 'parser idle'}</div>
					</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Sample Age</div>
					<div class="stat-value text-base">
						{typeof $runtimeGpsStatus.sampleAgeMs === 'number' ? `${Math.round($runtimeGpsStatus.sampleAgeMs / 1000)}s` : '—'}
						</div>
						<div class="stat-desc">
							{$runtimeGpsStatus.detectionTimedOut ? 'module timeout' : gpsHasFixStable() ? 'latest fix sample' : 'waiting for fix'}
						</div>
					</div>
			</div>
		</div>
	</div>

	<SettingsObdCard />
</div>
