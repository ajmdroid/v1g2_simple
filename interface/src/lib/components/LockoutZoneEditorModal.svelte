<script>
	import {
		LOCKOUT_BAND_OPTIONS,
		DIRECTION_MODE_OPTIONS,
		LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT,
		LEARNER_RADIUS_E5_MIN,
		LEARNER_RADIUS_E5_MAX,
		clampInt,
		radiusE5ToFeet,
		normalizeLearnerRadiusFeet,
		normalizeDirectionMode
	} from '$lib/utils/lockout';

	/**
	 * @type {{
	 *   open: boolean,
	 *   slot: number | null,
	 *   editor: object,
	 *   saving: boolean,
	 *   onclose: () => void,
	 *   onsave: () => void
	 * }}
	 */
	let { open, zoneSlot, editor = $bindable(), saving, onclose, onsave } = $props();
</script>

{#if open}
	<div class="modal modal-open">
		<div class="modal-box surface-modal max-w-3xl">
			<h3 class="font-bold text-lg">
				{zoneSlot === null ? 'Create Manual Lockout Zone' : `Edit Lockout Zone ${zoneSlot}`}
			</h3>
			<p class="py-2 copy-subtle">
				Use conservative values. Invalid coordinates or heading values are rejected by the firmware.
			</p>

			<div class="grid grid-cols-1 md:grid-cols-2 gap-3">
				<label class="form-control">
					<span class="label-text-field">Latitude</span>
					<input
						type="number"
						step="0.00001"
						min="-90"
						max="90"
						class="input input-bordered input-sm"
						value={editor.latitude}
						onchange={(e) => {
							editor.latitude = e.currentTarget.value;
						}}
					/>
				</label>
				<label class="form-control">
					<span class="label-text-field">Longitude</span>
					<input
						type="number"
						step="0.00001"
						min="-180"
						max="180"
						class="input input-bordered input-sm"
						value={editor.longitude}
						onchange={(e) => {
							editor.longitude = e.currentTarget.value;
						}}
					/>
				</label>
				<label class="form-control">
					<span class="label-text-field">Band</span>
					<select
						class="select select-bordered select-sm"
						value={editor.bandMask}
						onchange={(e) => {
							editor.bandMask = clampInt(e.currentTarget.value, 1, 255, 0x04);
						}}
					>
						{#each LOCKOUT_BAND_OPTIONS as option}
							<option value={option.value}>{option.label}</option>
						{/each}
					</select>
				</label>
				<label class="form-control">
					<span class="label-text-field">Frequency (MHz, optional)</span>
					<input
						type="number"
						min="0"
						max="65535"
						class="input input-bordered input-sm"
						value={editor.frequencyMHz}
						onchange={(e) => {
							editor.frequencyMHz = e.currentTarget.value;
						}}
					/>
				</label>
				<label class="form-control">
					<span class="label-text-field">Frequency tolerance (MHz)</span>
					<input
						type="number"
						min="0"
						max="65535"
						class="input input-bordered input-sm"
						value={editor.frequencyToleranceMHz}
						onchange={(e) => {
							editor.frequencyToleranceMHz = clampInt(
								e.currentTarget.value,
								0,
								65535,
								LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT
							);
						}}
					/>
				</label>
				<label class="form-control">
					<span class="label-text-field">Radius (ft)</span>
					<input
						type="number"
						min={radiusE5ToFeet(LEARNER_RADIUS_E5_MIN)}
						max={radiusE5ToFeet(LEARNER_RADIUS_E5_MAX)}
						class="input input-bordered input-sm"
						value={editor.radiusFt}
						onchange={(e) => {
							editor.radiusFt = normalizeLearnerRadiusFeet(e.currentTarget.value);
						}}
					/>
				</label>
				<label class="form-control">
					<span class="label-text-field">Confidence</span>
					<input
						type="number"
						min="0"
						max="255"
						class="input input-bordered input-sm"
						value={editor.confidence}
						onchange={(e) => {
							editor.confidence = clampInt(e.currentTarget.value, 0, 255, 100);
						}}
					/>
				</label>
				<label class="form-control">
					<span class="label-text-field">Direction mode</span>
					<select
						class="select select-bordered select-sm"
						value={editor.directionMode}
						onchange={(e) => {
							editor.directionMode = normalizeDirectionMode(e.currentTarget.value);
							if (editor.directionMode === 'all') {
								editor.headingDeg = '';
							}
						}}
					>
						{#each DIRECTION_MODE_OPTIONS as option}
							<option value={option.value}>{option.label}</option>
						{/each}
					</select>
				</label>
				<label class="form-control">
					<span class="label-text-field">Heading tolerance (degrees)</span>
					<input
						type="number"
						min="0"
						max="90"
						class="input input-bordered input-sm"
						value={editor.headingToleranceDeg}
						onchange={(e) => {
							editor.headingToleranceDeg = clampInt(e.currentTarget.value, 0, 90, 45);
						}}
					/>
				</label>
				{#if editor.directionMode !== 'all'}
					<label class="form-control md:col-span-2">
						<span class="label-text-field">Heading (0-359 degrees)</span>
						<input
							type="number"
							min="0"
							max="359"
							class="input input-bordered input-sm"
							value={editor.headingDeg}
							onchange={(e) => {
								editor.headingDeg = e.currentTarget.value;
							}}
						/>
					</label>
				{/if}
			</div>

			<div class="modal-action">
				<button class="btn btn-outline btn-sm" onclick={onclose} disabled={saving}>
					Cancel
				</button>
				<button class="btn btn-primary btn-sm" onclick={onsave} disabled={saving}>
					{#if saving}
						<span class="loading loading-spinner loading-xs"></span>
					{/if}
					Save Zone
				</button>
			</div>
		</div>
	</div>
{/if}
