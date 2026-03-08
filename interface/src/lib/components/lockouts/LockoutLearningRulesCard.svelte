<script>
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import {
		LOCKOUT_PRESET_BALANCED_BLEND,
		LOCKOUT_PRESET_LEGACY_SAFE,
		LEARNER_PROMOTION_HITS_MIN,
		LEARNER_PROMOTION_HITS_MAX,
		LEARNER_FREQ_TOLERANCE_MHZ_MIN,
		LEARNER_FREQ_TOLERANCE_MHZ_MAX,
		LEARNER_RADIUS_E5_MIN,
		LEARNER_RADIUS_E5_MAX,
		GPS_MAX_HDOP_X10_MIN,
		GPS_MAX_HDOP_X10_MAX,
		GPS_MIN_LEARNER_SPEED_MPH_MIN,
		GPS_MIN_LEARNER_SPEED_MPH_MAX,
		GPS_MIN_SATELLITES,
		clampLearnerPromotionHits,
		clampLearnerFreqToleranceMHz,
		clampLearnerRadiusE5,
		clampIntervalHours,
		clampUnlearnCount,
		clampManualDemotionMissCount,
		clampHdopX10,
		clampMinLearnerSpeed,
		radiusE5ToFeet,
		normalizeLearnerRadiusFeet
	} from '$lib/utils/lockout';

	let {
		advancedUnlocked,
		lockoutConfig = $bindable(),
		stageLearnerPreset,
		requestKaLearningToggle,
		markDirty
	} = $props();
</script>

<div class="surface-card">
	<div class="card-body gap-3">
		<CardSectionHead
			title="Learning Rules"
			subtitle="How the system learns and forgets lockout zones from repeated signal sightings."
		/>
		<div class="flex flex-wrap gap-2">
			<button
				class="btn btn-outline btn-xs"
				onclick={() => stageLearnerPreset(LOCKOUT_PRESET_LEGACY_SAFE)}
				disabled={!advancedUnlocked}
			>
				Preset: Conservative
			</button>
			<button
				class="btn btn-outline btn-xs"
				onclick={() => stageLearnerPreset(LOCKOUT_PRESET_BALANCED_BLEND)}
				disabled={!advancedUnlocked}
			>
				Preset: Balanced
			</button>
		</div>

		<div class="grid grid-cols-1 md:grid-cols-2 gap-3">
			<label class="form-control">
				<span class="label-text font-medium">Sightings Before Lockout</span>
				<input
					type="number"
					min={LEARNER_PROMOTION_HITS_MIN}
					max={LEARNER_PROMOTION_HITS_MAX}
					class="input input-bordered input-sm"
					value={lockoutConfig.learnerPromotionHits}
					disabled={!advancedUnlocked}
					onchange={(event) => {
						lockoutConfig.learnerPromotionHits = clampLearnerPromotionHits(event.currentTarget.value);
						markDirty();
					}}
				/>
			</label>
			<label class="form-control">
				<span class="label-text font-medium">Time Between Sightings</span>
				<select
					class="select select-bordered select-sm"
					value={lockoutConfig.learnerLearnIntervalHours}
					disabled={!advancedUnlocked}
					onchange={(event) => {
						lockoutConfig.learnerLearnIntervalHours = clampIntervalHours(event.currentTarget.value);
						markDirty();
					}}
				>
					<option value={0}>Count every pass</option>
					<option value={1}>At least 1 hour apart</option>
					<option value={4}>At least 4 hours apart</option>
					<option value={12}>At least 12 hours apart</option>
					<option value={24}>At least 24 hours apart</option>
				</select>
			</label>
			<label class="form-control">
				<span class="label-text font-medium">Frequency Match Window (±MHz)</span>
				<input
					type="number"
					min={LEARNER_FREQ_TOLERANCE_MHZ_MIN}
					max={LEARNER_FREQ_TOLERANCE_MHZ_MAX}
					class="input input-bordered input-sm"
					value={lockoutConfig.learnerFreqToleranceMHz}
					disabled={!advancedUnlocked}
					onchange={(event) => {
						lockoutConfig.learnerFreqToleranceMHz = clampLearnerFreqToleranceMHz(
							event.currentTarget.value
						);
						markDirty();
					}}
				/>
			</label>
			<label class="form-control">
				<span class="label-text font-medium">Zone Radius (ft)</span>
				<input
					type="number"
					min={radiusE5ToFeet(LEARNER_RADIUS_E5_MIN)}
					max={radiusE5ToFeet(LEARNER_RADIUS_E5_MAX)}
					class="input input-bordered input-sm"
					value={lockoutConfig.learnerRadiusFt}
					disabled={!advancedUnlocked}
					onchange={(event) => {
						lockoutConfig.learnerRadiusFt = normalizeLearnerRadiusFeet(event.currentTarget.value);
						markDirty();
					}}
				/>
			</label>
			<label class="form-control">
				<span class="label-text font-medium">Auto-Remove Learned Zones</span>
				<select
					class="select select-bordered select-sm"
					value={lockoutConfig.learnerUnlearnCount}
					disabled={!advancedUnlocked}
					onchange={(event) => {
						lockoutConfig.learnerUnlearnCount = clampUnlearnCount(event.currentTarget.value);
						markDirty();
					}}
				>
					<option value={0}>Never — use confidence decay</option>
					<option value={3}>After 3 drives without signal</option>
					<option value={5}>After 5 drives without signal</option>
					<option value={7}>After 7 drives without signal</option>
					<option value={10}>After 10 drives without signal</option>
				</select>
			</label>
			<label class="form-control">
				<span class="label-text font-medium">Time Between Removal Checks</span>
				<select
					class="select select-bordered select-sm"
					value={lockoutConfig.learnerUnlearnIntervalHours}
					disabled={!advancedUnlocked}
					onchange={(event) => {
						lockoutConfig.learnerUnlearnIntervalHours = clampIntervalHours(event.currentTarget.value);
						markDirty();
					}}
				>
					<option value={0}>Check every pass</option>
					<option value={1}>At least 1 hour apart</option>
					<option value={4}>At least 4 hours apart</option>
					<option value={12}>At least 12 hours apart</option>
					<option value={24}>At least 24 hours apart</option>
				</select>
			</label>
			<label class="form-control">
				<span class="label-text font-medium">Auto-Remove Manual Zones</span>
				<select
					class="select select-bordered select-sm"
					value={lockoutConfig.manualDemotionMissCount}
					disabled={!advancedUnlocked}
					onchange={(event) => {
						lockoutConfig.manualDemotionMissCount = clampManualDemotionMissCount(
							event.currentTarget.value
						);
						markDirty();
					}}
				>
					<option value={0}>Never — persist forever</option>
					<option value={10}>After 10 drives without signal</option>
					<option value={25}>After 25 drives without signal</option>
					<option value={50}>After 50 drives without signal</option>
				</select>
			</label>
		</div>

		<div class="divider text-xs my-1">Band Learning</div>

		<div class="form-control">
			<label class="label cursor-pointer">
				<div>
					<span class="label-text font-medium">K Band Learning</span>
					<p class="copy-caption-soft">Learn and lock out K-band false alerts (door openers, speed signs) — the primary use case for lockouts</p>
				</div>
				<input
					type="checkbox"
					class="toggle toggle-primary"
					checked={!!lockoutConfig.kLearningEnabled}
					disabled={!advancedUnlocked}
					onchange={(event) => {
						lockoutConfig.kLearningEnabled = event.currentTarget.checked;
						markDirty();
					}}
				/>
			</label>
			{#if !lockoutConfig.kLearningEnabled}
				<p class="copy-warning mt-1">⚠ K learning disabled — most false alerts will not be locked out</p>
			{/if}
		</div>

		<div class="form-control">
			<label class="label cursor-pointer">
				<div>
					<span class="label-text font-medium">X Band Learning</span>
					<p class="copy-caption-soft">Learn and lock out X-band false alerts — less common but still present in some areas</p>
				</div>
				<input
					type="checkbox"
					class="toggle toggle-primary"
					checked={!!lockoutConfig.xLearningEnabled}
					disabled={!advancedUnlocked}
					onchange={(event) => {
						lockoutConfig.xLearningEnabled = event.currentTarget.checked;
						markDirty();
					}}
				/>
			</label>
		</div>

		<div class="form-control">
			<label class="label cursor-pointer">
				<div>
					<span class="label-text font-medium">Ka Band Learning</span>
					<p class="copy-caption-soft">Allow the learner to lock out Ka-band signals — where real radar threats live</p>
				</div>
				<input
					type="checkbox"
					class="toggle toggle-warning"
					checked={!!lockoutConfig.kaLearningEnabled}
					disabled={!advancedUnlocked}
					onchange={(event) => {
						requestKaLearningToggle(event.currentTarget.checked);
					}}
				/>
			</label>
			{#if lockoutConfig.kaLearningEnabled}
				<p class="copy-warning mt-1">⚠ Ka learning active — lockouts can suppress real radar threats</p>
			{/if}
		</div>

		<div class="divider text-xs my-1">GPS Quality Gates</div>

		<div class="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-3 gap-3">
			<label class="form-control">
				<span class="label-text font-medium">GPS Accuracy Limit (HDOP)</span>
				<input
					type="number"
					min={GPS_MAX_HDOP_X10_MIN / 10}
					max={GPS_MAX_HDOP_X10_MAX / 10}
					step="0.1"
					class="input input-bordered input-sm"
					value={(lockoutConfig.maxHdopX10 / 10).toFixed(1)}
					disabled={!advancedUnlocked}
					onchange={(event) => {
						lockoutConfig.maxHdopX10 = clampHdopX10(Math.round(parseFloat(event.currentTarget.value) * 10));
						markDirty();
					}}
				/>
			</label>
			<label class="form-control">
				<span class="label-text font-medium">Minimum Speed for Learning</span>
				<input
					type="number"
					min={GPS_MIN_LEARNER_SPEED_MPH_MIN}
					max={GPS_MIN_LEARNER_SPEED_MPH_MAX}
					class="input input-bordered input-sm"
					value={lockoutConfig.minLearnerSpeedMph}
					disabled={!advancedUnlocked}
					onchange={(event) => {
						lockoutConfig.minLearnerSpeedMph = clampMinLearnerSpeed(event.currentTarget.value);
						markDirty();
					}}
				/>
			</label>
			<div class="form-control">
				<span class="label-text font-medium">Minimum Satellites</span>
				<div class="input input-bordered input-sm input-disabled flex items-center">{GPS_MIN_SATELLITES}</div>
			</div>
		</div>
	</div>
</div>
