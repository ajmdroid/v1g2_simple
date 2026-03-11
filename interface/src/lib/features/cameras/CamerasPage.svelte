<script>
	import { onMount } from 'svelte';
	import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	import {
		RANGE_MAX_CM,
		RANGE_MIN_CM,
		formatFeet,
		formatMiles,
		formatMilesInput,
		parseMilesInput
	} from '$lib/features/cameras/cameraFormat';

	const STATUS_POLL_INTERVAL_MS = 2500;

	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);

	let settings = $state({
		cameraAlertsEnabled: true,
		cameraAlertRangeCm: 128748
	});

	let status = $state({
		cameraCount: 0,
		displayActive: false,
		distanceCm: null
	});

	let alertMilesInput = $state('0.80');

	const statusPoll = createPoll(async () => {
		await fetchStatus();
	}, STATUS_POLL_INTERVAL_MS);

	onMount(async () => {
		await Promise.all([fetchSettings(), fetchStatus()]);
		statusPoll.start();
		return () => {
			statusPoll.stop();
		};
	});

	async function fetchSettings() {
		loading = true;
		try {
			const res = await fetchWithTimeout('/api/cameras/settings');
			if (!res.ok) {
				throw new Error('settings');
			}
			const data = await res.json();
			settings = {
				cameraAlertsEnabled: data.cameraAlertsEnabled === true,
				cameraAlertRangeCm:
					typeof data.cameraAlertRangeCm === 'number'
						? data.cameraAlertRangeCm
						: settings.cameraAlertRangeCm
			};
			syncRangeInput();
			message = null;
		} catch (error) {
			message = { type: 'error', text: 'Failed to load ALPR settings' };
		} finally {
			loading = false;
		}
	}

	async function fetchStatus() {
		try {
			const res = await fetchWithTimeout('/api/cameras/status');
			if (!res.ok) {
				throw new Error('status');
			}
			const data = await res.json();
			status = {
				cameraCount: typeof data.cameraCount === 'number' ? data.cameraCount : 0,
				displayActive: data.displayActive === true,
				distanceCm: typeof data.distanceCm === 'number' ? data.distanceCm : null
			};
		} catch (error) {
			if (!message || message.type !== 'error') {
				message = { type: 'error', text: 'Live ALPR status unavailable' };
			}
		}
	}

	async function saveSettings() {
		saving = true;
		message = null;

		try {
			const params = new URLSearchParams();
			params.append('cameraAlertsEnabled', settings.cameraAlertsEnabled);
			params.append('cameraAlertRangeCm', settings.cameraAlertRangeCm);

			const res = await fetchWithTimeout('/api/cameras/settings', {
				method: 'POST',
				headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
				body: params
			});

			if (!res.ok) {
				throw new Error('save');
			}

			syncRangeInput();
			message = { type: 'success', text: 'ALPR settings saved.' };
			await fetchStatus();
		} catch (error) {
			message = { type: 'error', text: 'Failed to save ALPR settings' };
		} finally {
			saving = false;
		}
	}

	function syncRangeInput() {
		alertMilesInput = formatMilesInput(settings.cameraAlertRangeCm);
	}

	function handleRangeInput(value) {
		alertMilesInput = value;
		const parsedCm = parseMilesInput(value, RANGE_MIN_CM, RANGE_MAX_CM);
		if (parsedCm !== null) {
			settings.cameraAlertRangeCm = parsedCm;
		}
	}

	function normalizeRangeInput() {
		const parsedCm = parseMilesInput(alertMilesInput, RANGE_MIN_CM, RANGE_MAX_CM);
		if (parsedCm === null) {
			syncRangeInput();
			return;
		}
		settings.cameraAlertRangeCm = parsedCm;
		syncRangeInput();
	}
</script>

<div class="page-stack">
	<PageHeader title="ALPR Cameras" subtitle="ALPR-only map coverage and live status." />

	<StatusAlert {message} fallbackType="success" />

	{#if loading}
		<div class="state-loading">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Coverage"
					subtitle="Control whether ALPR overlays can take over the resting display and how far ahead they are considered."
				/>

				<div class="space-y-4">
					<label class="label cursor-pointer surface-panel">
						<div>
							<span class="label-text font-medium">Enable ALPR Alerts</span>
							<p class="copy-caption-soft">Only ALPR overlays are supported by this build.</p>
						</div>
						<input
							type="checkbox"
							class="toggle toggle-primary"
							bind:checked={settings.cameraAlertsEnabled}
						/>
					</label>

					<div class="surface-panel">
						<div class="flex items-center justify-between gap-4">
							<div>
								<p class="font-medium">Alert Distance</p>
								<p class="copy-caption-soft">Search radius and display handoff threshold.</p>
							</div>
							<div class="text-right">
								<p class="text-lg font-mono">{formatFeet(settings.cameraAlertRangeCm)}</p>
								<p class="copy-caption-soft">{formatMiles(settings.cameraAlertRangeCm)}</p>
							</div>
						</div>
						<div class="mt-4 flex items-center gap-2">
							<input
								type="number"
								min={formatMilesInput(RANGE_MIN_CM)}
								max={formatMilesInput(RANGE_MAX_CM)}
								step="0.01"
								class="input input-bordered w-full font-mono"
								value={alertMilesInput}
								oninput={(event) => handleRangeInput(event.currentTarget.value)}
								onchange={normalizeRangeInput}
							/>
							<span class="badge badge-outline badge-lg font-mono">mi</span>
						</div>
						<p class="mt-2 copy-caption-soft">
							Range: {formatMiles(RANGE_MIN_CM)} to {formatMiles(RANGE_MAX_CM)}
						</p>
					</div>

					<div class="surface-panel">
						<p class="copy-caption-soft mb-2">Live status</p>
						<div class="grid gap-3 md:grid-cols-3">
							<div>
								<p class="copy-caption-soft">Loaded ALPR cameras</p>
								<p class="text-2xl font-semibold">{status.cameraCount}</p>
							</div>
							<div>
								<p class="copy-caption-soft">Display owner</p>
								<p class="text-2xl font-semibold">{status.displayActive ? 'ALPR' : 'Idle'}</p>
							</div>
							<div>
								<p class="copy-caption-soft">Distance</p>
								{#if status.distanceCm !== null}
									<p class="text-2xl font-semibold">{formatFeet(status.distanceCm)}</p>
									<p class="copy-caption-soft">{formatMiles(status.distanceCm)}</p>
								{:else}
									<p class="text-2xl font-semibold">-</p>
								{/if}
							</div>
						</div>
					</div>

					<div class="surface-panel">
						<p class="font-medium">Scope</p>
						<p class="copy-caption-soft">
							Speed, red-light, bus-lane, camera voice, and camera color controls have been removed.
						</p>
					</div>
				</div>
			</div>
		</div>

		<div class="flex flex-wrap gap-3">
			<button class="btn btn-primary" onclick={saveSettings} disabled={saving}>
				{#if saving}
					<span class="loading loading-spinner loading-sm"></span>
				{/if}
				Save ALPR Settings
			</button>
			<button class="btn btn-secondary" onclick={fetchSettings} disabled={saving}>
				Reload
			</button>
		</div>
	{/if}
</div>
