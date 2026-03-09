<script>
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';

	let { settings } = $props();
</script>

<div class="surface-card">
	<div class="card-body space-y-4">
		<CardSectionHead
			title="Auto Power Off"
			subtitle="Automatically power off when V1 disconnects (e.g., when you turn off your car)."
		/>
		<div class="form-control">
			<label class="label" for="auto-power-off">
				<span class="label-text">Minutes after disconnect (0 = disabled)</span>
			</label>
			<input
				id="auto-power-off"
				type="number"
				class="input input-bordered w-24"
				bind:value={settings.autoPowerOffMinutes}
				min="0"
				max="60"
				placeholder="0"
			/>
			<div class="label">
				<span class="label-text-alt">
					{#if settings.autoPowerOffMinutes > 0}
						Device will power off {settings.autoPowerOffMinutes} minute{settings.autoPowerOffMinutes !== 1 ? 's' : ''} after V1 disconnects
					{:else}
						Auto power-off is disabled
					{/if}
				</span>
			</div>
		</div>
	</div>
</div>
