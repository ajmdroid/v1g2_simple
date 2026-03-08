<script>
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import { clampU16 } from '$lib/utils/lockout';

	let {
		advancedUnlocked,
		lockoutConfig = $bindable(),
		lockoutConfigDirty,
		backendSynced,
		savingLockoutConfig,
		reloadStatus,
		saveConfig,
		markDirty
	} = $props();
</script>

<div class="surface-card">
	<div class="card-body gap-3">
		<CardSectionHead title="Lockout Mode" subtitle="How the system handles known false signals.">
			<div class="flex gap-2">
				{#if lockoutConfigDirty}
					<div class="badge badge-warning badge-sm">unsaved</div>
				{:else if backendSynced}
					<div class="badge badge-success badge-sm">synced</div>
				{:else}
					<div class="badge badge-ghost badge-sm">syncing</div>
				{/if}
				<button class="btn btn-outline btn-sm" onclick={reloadStatus}>Reload</button>
				<button
					class="btn btn-primary btn-sm"
					onclick={saveConfig}
					disabled={!advancedUnlocked || savingLockoutConfig || !lockoutConfigDirty}
				>
					{#if savingLockoutConfig}
						<span class="loading loading-spinner loading-xs"></span>
					{/if}
					Save
				</button>
			</div>
		</CardSectionHead>

		<div class="space-y-4">
			<div class="form-control">
				<label class="label" for="lockout-mode">
					<span class="label-text font-medium">Mode</span>
				</label>
				<select
					id="lockout-mode"
					class="select select-bordered w-full"
					bind:value={lockoutConfig.modeRaw}
					onchange={markDirty}
					disabled={!advancedUnlocked}
				>
					<option value={0}>Off — lockouts disabled</option>
					<option value={1}>Log Only — evaluate and log matches, no muting</option>
					<option value={3}>Enforce — mute locked-out signals</option>
				</select>
				{#if lockoutConfig.modeRaw === 3}
					<p class="copy-warning mt-1">⚠ Enforce will mute alerts matching lockout zones</p>
				{/if}
			</div>

			<div class="form-control">
				<label class="label cursor-pointer">
					<div>
						<span class="label-text font-medium">Safety Circuit Breaker</span>
						<p class="copy-caption-soft">Automatically disable enforcement if system health degrades</p>
					</div>
					<input
						type="checkbox"
						class="toggle toggle-primary"
						checked={!!lockoutConfig.coreGuardEnabled}
						disabled={!advancedUnlocked}
						onchange={(event) => {
							lockoutConfig.coreGuardEnabled = event.currentTarget.checked;
							markDirty();
						}}
					/>
				</label>
				{#if !lockoutConfig.coreGuardEnabled}
					<p class="copy-warning mt-1">⚠ Enforcement continues even during system issues</p>
				{/if}
			</div>

			{#if lockoutConfig.modeRaw === 3}
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text font-medium">Lower Volume in Lockout Zones</span>
							<p class="copy-caption-soft">Reduce V1 volume when approaching a zone, restore instantly on real alerts</p>
						</div>
						<input
							type="checkbox"
							class="toggle toggle-primary"
							checked={!!lockoutConfig.preQuiet}
							disabled={!advancedUnlocked}
							onchange={(event) => {
								lockoutConfig.preQuiet = event.currentTarget.checked;
								markDirty();
							}}
						/>
					</label>
				</div>

				{#if lockoutConfig.preQuiet}
					<div class="form-control">
						<label class="label" for="pre-quiet-buffer">
							<span class="label-text font-medium">Pre-Quiet Approach Distance</span>
						</label>
						<select
							id="pre-quiet-buffer"
							class="select select-bordered w-full"
							bind:value={lockoutConfig.preQuietBufferE5}
							onchange={markDirty}
							disabled={!advancedUnlocked}
						>
							<option value={0}>Same as zone — drop volume at zone edge</option>
							<option value={45}>+50m (~150 ft) — drop volume slightly before zone</option>
							<option value={90}>+100m (~300 ft) — drop volume well before zone</option>
							<option value={135}>+150m (~500 ft) — drop volume early, no surprise beeps</option>
						</select>
					</div>
				{/if}
			{/if}

			{#if lockoutConfig.coreGuardEnabled}
				<div class="surface-subsection">
					<p class="copy-caption-soft mb-2">Circuit breaker trip thresholds (0 = trip on first drop):</p>
					<div class="grid grid-cols-1 md:grid-cols-3 gap-3">
						<label class="form-control">
							<span class="label-text">Alert Queue Drops</span>
							<input
								type="number"
								min="0"
								max="65535"
								class="input input-bordered input-sm"
								value={lockoutConfig.maxQueueDrops}
								disabled={!advancedUnlocked}
								onchange={(event) => {
									lockoutConfig.maxQueueDrops = clampU16(event.currentTarget.value);
									markDirty();
								}}
							/>
						</label>
						<label class="form-control">
							<span class="label-text">Performance Drops</span>
							<input
								type="number"
								min="0"
								max="65535"
								class="input input-bordered input-sm"
								value={lockoutConfig.maxPerfDrops}
								disabled={!advancedUnlocked}
								onchange={(event) => {
									lockoutConfig.maxPerfDrops = clampU16(event.currentTarget.value);
									markDirty();
								}}
							/>
						</label>
						<label class="form-control">
							<span class="label-text">Event Bus Drops</span>
							<input
								type="number"
								min="0"
								max="65535"
								class="input input-bordered input-sm"
								value={lockoutConfig.maxEventBusDrops}
								disabled={!advancedUnlocked}
								onchange={(event) => {
									lockoutConfig.maxEventBusDrops = clampU16(event.currentTarget.value);
									markDirty();
								}}
							/>
						</label>
					</div>
				</div>
			{/if}
		</div>
	</div>
</div>
