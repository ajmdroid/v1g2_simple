<script>
	import { onMount } from 'svelte';
	import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';

	const RANGE_MIN_CM = 16093;
	const RANGE_MAX_CM = 160934;
	const STATUS_POLL_INTERVAL_MS = 2500;
	const CM_PER_MILE = 160934;
	const CM_PER_FOOT = 30.48;

	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);

	let settings = $state({
		cameraAlertsEnabled: true,
		cameraAlertRangeCm: 128748,
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

			message = { type: 'success', text: 'Camera settings saved.' };
			await fetchStatus();
		} catch (error) {
			message = { type: 'error', text: 'Failed to save camera settings' };
		} finally {
			saving = false;
		}
	}

	function cmToMiles(cm) {
		return (cm / CM_PER_MILE).toFixed(2);
	}

	function cmToFeet(cm) {
		return Math.round(cm / CM_PER_FOOT);
	}

	function formatFeet(cm) {
		return `${cmToFeet(cm).toLocaleString()} ft`;
	}

	function formatMiles(cm) {
		return `${cmToMiles(cm)} mi`;
	}

	function formatType(type) {
		switch (type) {
			case 'speed':
				return 'Speed';
			case 'red_light':
				return 'Red Light';
			case 'bus_lane':
				return 'Bus Lane';
			case 'alpr':
				return 'ALPR';
			default:
				return 'None';
		}
	}

	function rgb565ToHex(rgb565) {
		const val = typeof rgb565 === 'number' ? rgb565 : 0;
		const r = ((val >> 11) & 0x1f) << 3;
		const g = ((val >> 5) & 0x3f) << 2;
		const b = (val & 0x1f) << 3;
		return `#${[r, g, b].map((x) => x.toString(16).padStart(2, '0')).join('')}`;
	}

	function rgb565ToHexStr(rgb565) {
		const val = typeof rgb565 === 'number' ? rgb565 : 0;
		return val.toString(16).toUpperCase().padStart(4, '0');
	}

	function hexToRgb565(hex) {
		if (!hex || hex.length < 7) return 0;
		const r = parseInt(hex.slice(1, 3), 16) >> 3;
		const g = parseInt(hex.slice(3, 5), 16) >> 2;
		const b = parseInt(hex.slice(5, 7), 16) >> 3;
		return (r << 11) | (g << 5) | b;
	}

	function parseColorInput(input) {
		let clean = input.trim().toUpperCase();

		if (clean.startsWith('0X')) clean = clean.slice(2);
		if (clean.startsWith('#')) clean = clean.slice(1);
		if (!/^[0-9A-F]+$/.test(clean)) return null;

		if (clean.length <= 5) {
			const value = parseInt(clean, 16);
			return value <= 0xffff ? value : null;
		}

		if (clean.length === 6) {
			const r = parseInt(clean.slice(0, 2), 16) >> 3;
			const g = parseInt(clean.slice(2, 4), 16) >> 2;
			const b = parseInt(clean.slice(4, 6), 16) >> 3;
			return (r << 11) | (g << 5) | b;
		}

		return null;
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
		const rgb565 = settings[key] || 0;
		pickerR = ((rgb565 >> 11) & 0x1f) << 3;
		pickerG = ((rgb565 >> 5) & 0x3f) << 2;
		pickerB = (rgb565 & 0x1f) << 3;
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

	function getPickerHex() {
		return `#${[pickerR, pickerG, pickerB].map((x) => Math.min(255, x).toString(16).padStart(2, '0')).join('')}`;
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

					<div class="surface-panel">
						<div class="flex items-center justify-between gap-4">
							<div>
								<p class="font-medium">Alert Range</p>
								<p class="copy-caption-soft">Search radius and display handoff distance</p>
							</div>
							<div class="text-right">
								<p class="text-lg font-mono">{formatFeet(settings.cameraAlertRangeCm)}</p>
								<p class="copy-caption-soft">{formatMiles(settings.cameraAlertRangeCm)}</p>
							</div>
						</div>
						<input
							type="range"
							min={RANGE_MIN_CM}
							max={RANGE_MAX_CM}
							step="1"
							class="range range-primary mt-4"
							bind:value={settings.cameraAlertRangeCm}
						/>
						<div class="mt-2 flex justify-between copy-caption-soft">
							<span>{formatFeet(RANGE_MIN_CM)}</span>
							<span>{formatFeet(RANGE_MAX_CM)}</span>
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

{#if pickerOpen}
	<div class="modal modal-open">
		<div class="modal-box surface-modal">
			<h3 class="font-bold text-lg mb-4">{pickerLabel}</h3>

			<div class="surface-color-preview" style="background-color: {getPickerHex()}"></div>

			<div class="space-y-4">
				<div class="form-control">
					<label class="label" for="camera-picker-red">
						<span class="label-text font-semibold text-error">Red</span>
						<span class="label-text-alt font-mono">{pickerR}</span>
					</label>
					<input
						id="camera-picker-red"
						type="range"
						min="0"
						max="248"
						step="8"
						bind:value={pickerR}
						class="range range-error"
					/>
				</div>

				<div class="form-control">
					<label class="label" for="camera-picker-green">
						<span class="label-text font-semibold text-success">Green</span>
						<span class="label-text-alt font-mono">{pickerG}</span>
					</label>
					<input
						id="camera-picker-green"
						type="range"
						min="0"
						max="252"
						step="4"
						bind:value={pickerG}
						class="range range-success"
					/>
				</div>

				<div class="form-control">
					<label class="label" for="camera-picker-blue">
						<span class="label-text font-semibold text-info">Blue</span>
						<span class="label-text-alt font-mono">{pickerB}</span>
					</label>
					<input
						id="camera-picker-blue"
						type="range"
						min="0"
						max="248"
						step="8"
						bind:value={pickerB}
						class="range range-info"
					/>
				</div>
			</div>

			<div class="mt-4">
				<span class="copy-muted">Quick colors:</span>
				<div class="flex gap-2 mt-2 flex-wrap">
					<button class="btn btn-sm" style="background-color: #f80000" onclick={() => { pickerR = 248; pickerG = 0; pickerB = 0; }}>Red</button>
					<button class="btn btn-sm" style="background-color: #00fc00" onclick={() => { pickerR = 0; pickerG = 252; pickerB = 0; }}>Green</button>
					<button class="btn btn-sm" style="background-color: #0000f8" onclick={() => { pickerR = 0; pickerG = 0; pickerB = 248; }}>Blue</button>
					<button class="btn btn-sm" style="background-color: #f8fc00" onclick={() => { pickerR = 248; pickerG = 252; pickerB = 0; }}>Yellow</button>
					<button class="btn btn-sm" style="background-color: #00fcf8" onclick={() => { pickerR = 0; pickerG = 252; pickerB = 248; }}>Cyan</button>
					<button class="btn btn-sm" style="background-color: #f800f8" onclick={() => { pickerR = 248; pickerG = 0; pickerB = 248; }}>Magenta</button>
					<button class="btn btn-sm" style="background-color: #f8a000" onclick={() => { pickerR = 248; pickerG = 160; pickerB = 0; }}>Orange</button>
					<button class="btn btn-sm bg-white text-black" onclick={() => { pickerR = 248; pickerG = 252; pickerB = 248; }}>White</button>
				</div>
			</div>

			<div class="modal-action">
				<button class="btn btn-ghost" onclick={cancelPicker}>Cancel</button>
				<button class="btn btn-primary" onclick={applyPickerColor}>Apply</button>
			</div>
		</div>
		<div
			class="modal-backdrop"
			onclick={cancelPicker}
			onkeydown={(event) => {
				if (event.key === 'Enter' || event.key === ' ') {
					event.preventDefault();
					cancelPicker();
				}
			}}
			role="button"
			tabindex="0"
			aria-label="Close color picker"
		></div>
	</div>
{/if}
