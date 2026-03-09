	<script>
		import { onMount } from 'svelte';
		import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
		import CardSectionHead from '$lib/components/CardSectionHead.svelte';
		import CameraColorPickerModal from '$lib/features/cameras/CameraColorPickerModal.svelte';
		import PageHeader from '$lib/components/PageHeader.svelte';
		import StatusAlert from '$lib/components/StatusAlert.svelte';
		import {
			NEAR_RANGE_MIN_CM,
			RANGE_MAX_CM,
			RANGE_MIN_CM,
			formatFeet,
			formatMiles,
			formatMilesInput,
			formatType,
			parseColorInput,
			parseMilesInput,
			rgb565ToChannels,
			rgb565ToHex,
			rgb565ToHexStr
		} from '$lib/features/cameras/cameraFormat';

		const STATUS_POLL_INTERVAL_MS = 2500;

	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);

	let settings = $state({
		cameraAlertsEnabled: true,
		cameraAlertRangeCm: 128748,
		cameraAlertNearRangeCm: 15240,
		cameraTypeAlpr: true,
		cameraTypeRedLight: true,
		cameraTypeSpeed: true,
		cameraTypeBusLane: false,
		colorCameraArrow: 0x780f,
		colorCameraText: 0x780f,
		cameraVoiceFarEnabled: true,
		cameraVoiceNearEnabled: true
	});

	let status = $state({
		cameraCount: 0,
		displayActive: false,
		type: null,
		distanceCm: null
	});

	let pickerOpen = $state(false);
	let pickerKey = $state(null);
	let pickerLabel = $state('');
	let pickerR = $state(0);
	let pickerG = $state(0);
	let pickerB = $state(0);
	let firstAlertMilesInput = $state('0.80');
	let closeAlertMilesInput = $state('0.09');

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
				...settings,
				...data
			};
			syncDistanceInputs();
		} catch (error) {
			message = { type: 'error', text: 'Failed to load camera settings' };
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
				type: typeof data.type === 'string' ? data.type : null,
				distanceCm: typeof data.distanceCm === 'number' ? data.distanceCm : null
			};
		} catch (error) {
			if (!message || message.type !== 'error') {
				message = { type: 'error', text: 'Live camera status unavailable' };
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
			params.append('cameraAlertNearRangeCm', settings.cameraAlertNearRangeCm);
			params.append('cameraTypeAlpr', settings.cameraTypeAlpr);
			params.append('cameraTypeRedLight', settings.cameraTypeRedLight);
			params.append('cameraTypeSpeed', settings.cameraTypeSpeed);
			params.append('cameraTypeBusLane', settings.cameraTypeBusLane);
			params.append('colorCameraArrow', settings.colorCameraArrow);
			params.append('colorCameraText', settings.colorCameraText);
			params.append('cameraVoiceFarEnabled', settings.cameraVoiceFarEnabled);
			params.append('cameraVoiceNearEnabled', settings.cameraVoiceNearEnabled);

			const res = await fetchWithTimeout('/api/cameras/settings', {
				method: 'POST',
				headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
				body: params
			});

			if (!res.ok) {
				throw new Error('save');
			}

			syncDistanceInputs();
			message = { type: 'success', text: 'Camera settings saved.' };
			await fetchStatus();
		} catch (error) {
			message = { type: 'error', text: 'Failed to save camera settings' };
		} finally {
			saving = false;
		}
	}

	function syncDistanceInputs() {
		firstAlertMilesInput = formatMilesInput(settings.cameraAlertRangeCm);
		closeAlertMilesInput = formatMilesInput(settings.cameraAlertNearRangeCm);
	}

	function handleFirstAlertInput(value) {
		firstAlertMilesInput = value;
		const parsedCm = parseMilesInput(value, RANGE_MIN_CM, RANGE_MAX_CM);
		if (parsedCm === null) {
			return;
		}

		settings.cameraAlertRangeCm = parsedCm;
		if (settings.cameraAlertNearRangeCm > parsedCm) {
			settings.cameraAlertNearRangeCm = parsedCm;
			closeAlertMilesInput = formatMilesInput(parsedCm);
		}
	}

	function normalizeFirstAlertInput() {
		const parsedCm = parseMilesInput(firstAlertMilesInput, RANGE_MIN_CM, RANGE_MAX_CM);
		if (parsedCm === null) {
			syncDistanceInputs();
			return;
		}

		settings.cameraAlertRangeCm = parsedCm;
		if (settings.cameraAlertNearRangeCm > parsedCm) {
			settings.cameraAlertNearRangeCm = parsedCm;
		}
		syncDistanceInputs();
	}

	function handleCloseAlertInput(value) {
		closeAlertMilesInput = value;
		const parsedCm = parseMilesInput(value, NEAR_RANGE_MIN_CM, settings.cameraAlertRangeCm);
		if (parsedCm === null) {
			return;
		}

		settings.cameraAlertNearRangeCm = parsedCm;
	}

	function normalizeCloseAlertInput() {
		const parsedCm = parseMilesInput(closeAlertMilesInput, NEAR_RANGE_MIN_CM, settings.cameraAlertRangeCm);
		if (parsedCm === null) {
			syncDistanceInputs();
			return;
		}

		settings.cameraAlertNearRangeCm = parsedCm;
		closeAlertMilesInput = formatMilesInput(parsedCm);
	}

	function handleHexInput(key, value) {
		const parsed = parseColorInput(value);
		if (parsed !== null) {
			settings[key] = parsed;
		}
	}

	function openPicker(key, label) {
		pickerKey = key;
		pickerLabel = label;
		const channels = rgb565ToChannels(settings[key] || 0);
		pickerR = channels.r;
		pickerG = channels.g;
		pickerB = channels.b;
		pickerOpen = true;
	}

	function applyPickerColor() {
		if (pickerKey) {
			const r = pickerR >> 3;
			const g = pickerG >> 2;
			const b = pickerB >> 3;
			settings[pickerKey] = (r << 11) | (g << 5) | b;
		}
		pickerOpen = false;
	}

	function cancelPicker() {
		pickerOpen = false;
	}
</script>

<div class="page-stack">
	<PageHeader title="Camera Alerts" subtitle="Map-based camera settings, voice stages, and live status." />

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
					subtitle="Choose whether camera overlays can interrupt the resting display and how far ahead they search."
				/>

				<div class="space-y-4">
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Enable Camera Alerts</span>
								<p class="copy-caption-soft">Map-backed cameras can take over the idle display when confirmed.</p>
							</div>
							<input
								type="checkbox"
								class="toggle toggle-primary"
								bind:checked={settings.cameraAlertsEnabled}
							/>
						</label>
					</div>

					<div class="grid gap-4 md:grid-cols-2">
						<div class="surface-panel">
							<div class="flex items-center justify-between gap-4">
								<div>
									<p class="font-medium">First Alert Distance</p>
									<p class="copy-caption-soft">Search radius, display handoff, and far-stage voice trigger</p>
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
									value={firstAlertMilesInput}
									oninput={(event) => handleFirstAlertInput(event.currentTarget.value)}
									onchange={normalizeFirstAlertInput}
								/>
								<span class="badge badge-outline badge-lg font-mono">mi</span>
							</div>
							<p class="mt-2 copy-caption-soft">Range: {formatMiles(RANGE_MIN_CM)} to {formatMiles(RANGE_MAX_CM)}</p>
						</div>

						<div class="surface-panel">
							<div class="flex items-center justify-between gap-4">
								<div>
									<p class="font-medium">Close Alert Distance</p>
									<p class="copy-caption-soft">Near-stage voice trigger. Must stay at or under the first alert distance.</p>
								</div>
								<div class="text-right">
									<p class="text-lg font-mono">{formatFeet(settings.cameraAlertNearRangeCm)}</p>
									<p class="copy-caption-soft">{formatMiles(settings.cameraAlertNearRangeCm)}</p>
								</div>
							</div>
							<div class="mt-4 flex items-center gap-2">
								<input
									type="number"
									min={formatMilesInput(NEAR_RANGE_MIN_CM)}
									max={formatMilesInput(settings.cameraAlertRangeCm)}
									step="0.01"
									class="input input-bordered w-full font-mono"
									value={closeAlertMilesInput}
									oninput={(event) => handleCloseAlertInput(event.currentTarget.value)}
									onchange={normalizeCloseAlertInput}
								/>
								<span class="badge badge-outline badge-lg font-mono">mi</span>
							</div>
							<p class="mt-2 copy-caption-soft">Range: {formatMiles(NEAR_RANGE_MIN_CM)} to {formatMiles(settings.cameraAlertRangeCm)}</p>
						</div>
					</div>

					<div class="surface-panel">
						<p class="copy-caption-soft mb-2">Live status</p>
						<div class="grid gap-3 md:grid-cols-3">
							<div>
								<p class="copy-caption-soft">Loaded cameras</p>
								<p class="text-2xl font-semibold">{status.cameraCount}</p>
							</div>
							<div>
								<p class="copy-caption-soft">Display owner</p>
								<p class="text-2xl font-semibold">{status.displayActive ? formatType(status.type) : 'Idle'}</p>
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
				</div>
			</div>
		</div>

		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Camera Types"
					subtitle="Keep the detector quiet for camera overlays you do not care about."
				/>

				<div class="grid gap-3 md:grid-cols-2">
					<label class="label cursor-pointer surface-panel">
						<span class="label-text font-medium">Speed</span>
						<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.cameraTypeSpeed} />
					</label>
					<label class="label cursor-pointer surface-panel">
						<span class="label-text font-medium">Red Light</span>
						<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.cameraTypeRedLight} />
					</label>
					<label class="label cursor-pointer surface-panel">
						<span class="label-text font-medium">Bus Lane</span>
						<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.cameraTypeBusLane} />
					</label>
					<label class="label cursor-pointer surface-panel">
						<span class="label-text font-medium">ALPR</span>
						<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.cameraTypeAlpr} />
					</label>
				</div>
			</div>
		</div>

		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Voice + Display"
					subtitle="Far and near voice stages share the same display owner but can be tuned independently."
				/>

				<div class="grid gap-4 lg:grid-cols-2">
					<div class="space-y-3">
						<label class="label cursor-pointer surface-panel">
							<div>
								<span class="label-text font-medium">Far Voice Stage</span>
								<p class="copy-caption-soft">Plays the type clip when the confirmed encounter enters the far threshold.</p>
							</div>
							<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.cameraVoiceFarEnabled} />
						</label>
						<label class="label cursor-pointer surface-panel">
							<div>
								<span class="label-text font-medium">Near Voice Stage</span>
								<p class="copy-caption-soft">Plays the close clip when the confirmed encounter reaches the near threshold.</p>
							</div>
							<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.cameraVoiceNearEnabled} />
						</label>
					</div>

					<div class="space-y-3">
						<div class="surface-panel">
							<div class="flex items-center justify-between gap-4">
								<div>
									<p class="font-medium">Arrow Color</p>
									<p class="copy-caption-soft">Front arrow when a camera owns the screen</p>
								</div>
								<div class="flex items-center gap-2">
									<button
										type="button"
										aria-label="Camera arrow color"
										class="color-swatch-btn md"
										style="background-color: {rgb565ToHex(settings.colorCameraArrow)}"
										onclick={() => openPicker('colorCameraArrow', 'Camera Arrow Color')}
									></button>
									<input
										type="text"
										class="input input-bordered input-xs w-16 font-mono text-xs"
										value={rgb565ToHexStr(settings.colorCameraArrow)}
										onchange={(event) => handleHexInput('colorCameraArrow', event.currentTarget.value)}
										title="RGB565 hex (or RGB888)"
									/>
									<span class="text-2xl font-bold" style="color: {rgb565ToHex(settings.colorCameraArrow)}">▲</span>
								</div>
							</div>
						</div>

						<div class="surface-panel">
							<div class="flex items-center justify-between gap-4">
								<div>
									<p class="font-medium">Label Color</p>
									<p class="copy-caption-soft">Text color for the camera type label in the frequency area</p>
								</div>
								<div class="flex items-center gap-2">
									<button
										type="button"
										aria-label="Camera label color"
										class="color-swatch-btn md"
										style="background-color: {rgb565ToHex(settings.colorCameraText)}"
										onclick={() => openPicker('colorCameraText', 'Camera Label Color')}
									></button>
									<input
										type="text"
										class="input input-bordered input-xs w-16 font-mono text-xs"
										value={rgb565ToHexStr(settings.colorCameraText)}
										onchange={(event) => handleHexInput('colorCameraText', event.currentTarget.value)}
										title="RGB565 hex (or RGB888)"
									/>
									<span
										class="font-mono text-sm font-bold tracking-[0.18em]"
										style="color: {rgb565ToHex(settings.colorCameraText)}"
									>SPEED</span>
								</div>
							</div>
						</div>
					</div>
				</div>
			</div>
		</div>

		<div class="flex flex-wrap gap-3">
			<button class="btn btn-primary" onclick={saveSettings} disabled={saving}>
				{#if saving}
					<span class="loading loading-spinner loading-sm"></span>
				{/if}
				Save Camera Settings
			</button>
			<button class="btn btn-secondary" onclick={fetchSettings} disabled={saving}>
				Reload
			</button>
		</div>
	{/if}
</div>

<CameraColorPickerModal
	open={pickerOpen}
	label={pickerLabel}
	bind:r={pickerR}
	bind:g={pickerG}
	bind:b={pickerB}
	oncancel={cancelPicker}
	onapply={applyPickerColor}
/>
