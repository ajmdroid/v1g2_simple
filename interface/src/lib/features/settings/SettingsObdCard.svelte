<script>
	import { onMount, onDestroy } from 'svelte';
	import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';

	let { settings } = $props();

	const STATE_NAMES = ['Idle', 'WaitBoot', 'Scanning', '', 'Connecting', 'Discovering', 'ATInit', 'Polling', 'ErrorBackoff', 'Disconnected'];

	let obdStatus = $state(null);
	let obdMessage = $state(null);
	let scanning = $state(false);
	let forgetting = $state(false);
	let statusFetchInFlight = false;

	const statusPoll = createPoll(async () => {
		await fetchObdStatus();
	}, 2000);

	onMount(() => {
		if (settings.obdEnabled) statusPoll.start();
	});

	onDestroy(() => {
		statusPoll.stop();
	});

	function handleToggle() {
		if (settings.obdEnabled) {
			statusPoll.start();
		} else {
			statusPoll.stop();
			obdStatus = null;
		}
	}

	async function fetchObdStatus() {
		if (statusFetchInFlight) return;
		statusFetchInFlight = true;
		try {
			const res = await fetchWithTimeout('/api/obd/status');
			if (res.ok) obdStatus = await res.json();
		} catch (e) {
			// Silently ignore — poll will retry
		} finally {
			statusFetchInFlight = false;
		}
	}

	async function startScan() {
		scanning = true;
		obdMessage = null;
		try {
			const res = await fetchWithTimeout('/api/obd/scan', { method: 'POST' });
			if (res.ok) {
				obdMessage = { type: 'success', text: 'Scan started.' };
			} else {
				obdMessage = { type: 'error', text: 'Failed to start scan.' };
			}
		} catch (e) {
			obdMessage = { type: 'error', text: 'Connection error.' };
		} finally {
			scanning = false;
		}
	}

	async function forgetDevice() {
		forgetting = true;
		obdMessage = null;
		try {
			const res = await fetchWithTimeout('/api/obd/forget', { method: 'POST' });
			if (res.ok) {
				obdMessage = { type: 'success', text: 'Device forgotten.' };
				obdStatus = null;
			} else {
				obdMessage = { type: 'error', text: 'Failed to forget device.' };
			}
		} catch (e) {
			obdMessage = { type: 'error', text: 'Connection error.' };
		} finally {
			forgetting = false;
		}
	}

	function stateName(s) {
		return STATE_NAMES[s] || `Unknown(${s})`;
	}
</script>

<div class="surface-card">
	<div class="card-body space-y-4">
		<CardSectionHead title="OBD-II Speed Source" subtitle="Connect an OBDLink CX for vehicle speed data." />

		<label class="label cursor-pointer">
			<span class="label-text">Enable OBD</span>
			<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.obdEnabled} onchange={handleToggle} />
		</label>

		{#if settings.obdEnabled}
			<div class="form-control">
				<label class="label" for="obd-min-rssi">
					<span class="label-text">Min RSSI (dBm)</span>
				</label>
				<input
					id="obd-min-rssi"
					type="number"
					class="input input-bordered w-24"
					bind:value={settings.obdMinRssi}
					min="-90"
					max="-40"
					placeholder="-80"
				/>
			</div>

			{#if obdStatus}
				<div class="surface-note copy-muted text-sm space-y-1">
					<div><strong>State:</strong> {stateName(obdStatus.state)}</div>
					{#if obdStatus.connected}
						<div><strong>Speed:</strong> {obdStatus.speedValid ? `${obdStatus.speedMph.toFixed(1)} mph` : 'N/A'}</div>
						<div><strong>RSSI:</strong> {obdStatus.rssi} dBm</div>
					{/if}
					{#if obdStatus.savedAddressValid}
						<div><strong>Saved device:</strong> Yes</div>
					{/if}
					{#if obdStatus.pollCount > 0}
						<div><strong>Polls:</strong> {obdStatus.pollCount} ({obdStatus.pollErrors} errors)</div>
					{/if}
				</div>
			{/if}

			{#if obdMessage}
				<StatusAlert message={obdMessage} />
			{/if}

			<div class="flex gap-2">
				<button class="btn btn-primary btn-sm" onclick={startScan} disabled={scanning || obdStatus?.scanInProgress}>
					{#if scanning || obdStatus?.scanInProgress}
						<span class="loading loading-spinner loading-xs"></span>
					{/if}
					Scan Now
				</button>
				<button class="btn btn-error btn-outline btn-sm" onclick={forgetDevice} disabled={forgetting || !obdStatus?.savedAddressValid}>
					Forget Device
				</button>
			</div>
		{/if}
	</div>
</div>
