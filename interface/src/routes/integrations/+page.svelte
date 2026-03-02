<script>
	import { onMount } from 'svelte';
	import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
	import { formatFrequencyMhz } from '$lib/utils/format';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';

	let loading = $state(true);
	let message = $state(null);
	let gpsStatusFetchInFlight = false;
	let savingGpsEnabled = $state(false);

	const STATUS_POLL_INTERVAL_MS = 2500;

	const statusPoll = createPoll(async () => {
		await fetchGpsStatus();
	}, STATUS_POLL_INTERVAL_MS);

	let gpsStatus = $state({
		enabled: false,
		runtimeEnabled: false,
		mode: 'scaffold',
		hasFix: false,
		stableHasFix: false,
		satellites: 0,
		stableSatellites: 0,
		sampleAgeMs: null,
		stableFixAgeMs: null,
		moduleDetected: false,
		detectionTimedOut: false,
		parserActive: false
	});

	onMount(async () => {
		await fetchGpsStatus();
		loading = false;
		statusPoll.start();

		return () => {
			statusPoll.stop();
		};
	});

	function setMsg(type, text) {
		message = { type, text };
	}

	function gpsHasFixStable() {
		return (typeof gpsStatus.stableHasFix === 'boolean') ? gpsStatus.stableHasFix : !!gpsStatus.hasFix;
	}

	function gpsSatellitesStable() {
		if (typeof gpsStatus.stableSatellites === 'number') return gpsStatus.stableSatellites;
		return gpsStatus.satellites || 0;
	}

	async function fetchGpsStatus() {
		if (gpsStatusFetchInFlight) return;
		gpsStatusFetchInFlight = true;
		try {
			const res = await fetchWithTimeout('/api/gps/status');
			if (!res.ok) return;
			const data = await res.json();
			gpsStatus = { ...gpsStatus, ...data };
		} catch (e) {
			// Polling should fail silently.
		} finally {
			gpsStatusFetchInFlight = false;
		}
	}

	async function toggleGpsEnabled(enabled) {
		if (savingGpsEnabled) return;
		const previous = !!gpsStatus.enabled;
		gpsStatus = { ...gpsStatus, enabled };
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
				gpsStatus = { ...gpsStatus, enabled: previous };
				return;
			}

			setMsg('success', `GPS ${enabled ? 'enabled' : 'disabled'}`);
			await fetchGpsStatus();
		} catch (e) {
			gpsStatus = { ...gpsStatus, enabled: previous };
			setMsg('error', 'Failed to update GPS setting');
		} finally {
			savingGpsEnabled = false;
		}
	}

</script>

<div class="page-stack">
	<PageHeader title="Integrations" subtitle="GPS runtime controls and connectivity status.">
		<div class="flex items-center gap-2">
			<a href="/lockouts" class="btn btn-outline btn-sm">Open Lockouts</a>
		</div>
	</PageHeader>

	<StatusAlert {message} />

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
						checked={!!gpsStatus.enabled}
						onchange={(e) => toggleGpsEnabled(e.currentTarget.checked)}
						disabled={savingGpsEnabled}
					/>
				</label>
			</CardSectionHead>

			<div class="surface-stats">
				<div class="stat py-3 px-4">
					<div class="stat-title">Mode</div>
					<div class="stat-value text-base">{gpsStatus.mode || 'scaffold'}</div>
					<div class="stat-desc">{gpsStatus.runtimeEnabled ? 'runtime active' : 'runtime idle'}</div>
				</div>
					<div class="stat py-3 px-4">
						<div class="stat-title">Fix</div>
						<div class="stat-value text-base">{gpsHasFixStable() ? 'Yes' : 'No'}</div>
						<div class="stat-desc">{gpsStatus.moduleDetected ? 'module detected' : 'waiting for module'}</div>
					</div>
					<div class="stat py-3 px-4">
						<div class="stat-title">Satellites</div>
						<div class="stat-value text-base">{gpsSatellitesStable()}</div>
						<div class="stat-desc">{gpsStatus.parserActive ? 'parser active' : 'parser idle'}</div>
					</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Sample Age</div>
					<div class="stat-value text-base">
						{typeof gpsStatus.sampleAgeMs === 'number' ? `${Math.round(gpsStatus.sampleAgeMs / 1000)}s` : '—'}
						</div>
						<div class="stat-desc">
							{gpsStatus.detectionTimedOut ? 'module timeout' : gpsHasFixStable() ? 'latest fix sample' : 'waiting for fix'}
						</div>
					</div>
			</div>
		</div>
	</div>
</div>
