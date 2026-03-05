<script>
	import { onMount } from 'svelte';
	import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';

	const METERS_PER_MILE = 1609.344;

	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);
	let settings = $state({
		cameraAlertsEnabled: true,
		cameraAlertRangeM: 805,
		cameraTypeAlpr: true,
		cameraTypeRedLight: true,
		cameraTypeSpeed: true,
		cameraTypeBusLane: false,
		cameraVoiceEnabled: true,
		cameraVoiceClose: true,
		cameraCount: 0
	});
	let status = $state({
		active: false,
		encounterActive: false,
		type: null,
		distanceMiles: null,
		pendingFar: false,
		pendingNear: false
	});

	const statusPoll = createPoll(async () => {
		await fetchStatus();
	}, 2000);

	const rangeMiles = $derived((settings.cameraAlertRangeM / METERS_PER_MILE).toFixed(2));

	onMount(async () => {
		await fetchSettings();
		await fetchStatus();
		statusPoll.start();
		return () => statusPoll.stop();
	});

	async function fetchSettings() {
		loading = true;
		try {
			const res = await fetchWithTimeout('/api/cameras/settings');
			if (!res.ok) throw new Error('failed');
			const data = await res.json();
			settings = { ...settings, ...data };
		} catch (e) {
			message = { type: 'error', text: 'Failed to load camera settings' };
		} finally {
			loading = false;
		}
	}

	async function fetchStatus() {
		try {
			const res = await fetchWithTimeout('/api/cameras/status');
			if (!res.ok) return;
			const data = await res.json();
			status = { ...status, ...data };
		} catch (e) {
			// Ignore status poll failures to avoid UI spam.
		}
	}

	function updateRangeFromMiles(milesString) {
		const miles = Number(milesString);
		if (!Number.isFinite(miles)) return;
		const meters = Math.round(miles * METERS_PER_MILE);
		settings.cameraAlertRangeM = Math.max(50, Math.min(5000, meters));
	}

	async function saveSettings() {
		saving = true;
		message = null;
		try {
			const payload = {
				cameraAlertsEnabled: settings.cameraAlertsEnabled,
				cameraAlertRangeM: settings.cameraAlertRangeM,
				cameraTypeAlpr: settings.cameraTypeAlpr,
				cameraTypeRedLight: settings.cameraTypeRedLight,
				cameraTypeSpeed: settings.cameraTypeSpeed,
				cameraTypeBusLane: settings.cameraTypeBusLane,
				cameraVoiceEnabled: settings.cameraVoiceEnabled,
				cameraVoiceClose: settings.cameraVoiceClose
			};

			const res = await fetchWithTimeout('/api/cameras/settings', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify(payload)
			});
			if (!res.ok) throw new Error('save failed');
			const data = await res.json();
			settings = { ...settings, ...data };
			message = { type: 'success', text: 'Camera settings saved.' };
			await fetchStatus();
		} catch (e) {
			message = { type: 'error', text: 'Failed to save camera settings' };
		} finally {
			saving = false;
		}
	}
</script>

<div class="page-stack">
	<PageHeader title="Camera Alerts" subtitle="Proximity camera display + two-stage voice announcements" />
	<StatusAlert {message} fallbackType="success" />

	{#if loading}
		<div class="state-loading">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead title="Feature" subtitle="Enable display and audio camera proximity alerts." />
				<div class="form-control">
					<label class="label cursor-pointer">
						<span class="label-text font-medium">Enable Camera Alerts</span>
						<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.cameraAlertsEnabled} />
					</label>
				</div>

				<div class="form-control">
					<label class="label" for="camera-range-mi">
						<span class="label-text font-medium">Visual Range</span>
						<span class="label-text-alt">{rangeMiles} mi ({settings.cameraAlertRangeM} m)</span>
					</label>
					<input
						id="camera-range-mi"
						type="range"
						min="0.03"
						max="3.11"
						step="0.01"
						value={rangeMiles}
						oninput={(e) => updateRangeFromMiles(e.currentTarget.value)}
						class="range range-primary"
					/>
				</div>
			</div>
		</div>

		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead title="Camera Types" subtitle="Choose which camera categories participate in alerts." />
				<div class="grid md:grid-cols-2 gap-3">
					<label class="label cursor-pointer"><span class="label-text">Speed</span><input type="checkbox" class="toggle toggle-primary" bind:checked={settings.cameraTypeSpeed} /></label>
					<label class="label cursor-pointer"><span class="label-text">Red Light</span><input type="checkbox" class="toggle toggle-primary" bind:checked={settings.cameraTypeRedLight} /></label>
					<label class="label cursor-pointer"><span class="label-text">ALPR</span><input type="checkbox" class="toggle toggle-primary" bind:checked={settings.cameraTypeAlpr} /></label>
					<label class="label cursor-pointer"><span class="label-text">Bus Lane</span><input type="checkbox" class="toggle toggle-primary" bind:checked={settings.cameraTypeBusLane} /></label>
				</div>
			</div>
		</div>

		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead title="Voice Stages" subtitle="Far at 1000 ft and close at 500 ft; each stage can be disabled." />
				<div class="grid md:grid-cols-2 gap-3">
					<label class="label cursor-pointer">
						<div><span class="label-text font-medium">1000 ft Voice</span><p class="copy-caption-soft">`cam_<type> + ahead`</p></div>
						<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.cameraVoiceEnabled} />
					</label>
					<label class="label cursor-pointer">
						<div><span class="label-text font-medium">500 ft Voice</span><p class="copy-caption-soft">`cam_<type> + close`</p></div>
						<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.cameraVoiceClose} />
					</label>
				</div>
			</div>
		</div>

		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead title="Live Status" subtitle="Diagnostic snapshot from /api/cameras/status." />
				<div class="grid md:grid-cols-2 gap-3 text-sm">
					<div class="surface-panel p-3">
						<p><strong>Map cameras:</strong> {settings.cameraCount ?? 0}</p>
						<p><strong>Display active:</strong> {status.active ? 'yes' : 'no'}</p>
						<p><strong>Encounter active:</strong> {status.encounterActive ? 'yes' : 'no'}</p>
					</div>
					<div class="surface-panel p-3">
						<p><strong>Type:</strong> {status.type ?? '—'}</p>
						<p><strong>Distance:</strong> {status.distanceMiles != null ? `${Number(status.distanceMiles).toFixed(2)} mi` : '—'}</p>
						<p><strong>Pending voice:</strong> {status.pendingNear ? 'near' : status.pendingFar ? 'far' : 'none'}</p>
					</div>
				</div>
			</div>
		</div>

		<div class="sticky bottom-4 flex justify-end">
			<button class="btn btn-primary" onclick={saveSettings} disabled={saving}>
				{#if saving}
					<span class="loading loading-spinner loading-sm"></span> Saving...
				{:else}
					Save Camera Settings
				{/if}
			</button>
		</div>
	{/if}
</div>
