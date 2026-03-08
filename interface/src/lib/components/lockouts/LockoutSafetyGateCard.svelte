<script>
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';

	let { advancedUnlocked = $bindable() } = $props();
</script>

<div class="surface-card">
	<div class="card-body gap-3">
		<CardSectionHead
			title="Safety Gate"
			subtitle="Advanced lockout writes stay disabled until explicitly unlocked for this session."
		>
			<label class="label cursor-pointer justify-start gap-3 py-0">
				<span class="label-text">Unlock advanced writes</span>
				<input type="checkbox" class="toggle toggle-warning" bind:checked={advancedUnlocked} />
			</label>
		</CardSectionHead>
		<div class="copy-caption-soft">
			Changing lockout settings can affect threat detection. Use caution in Enforce mode.
		</div>
	</div>
</div>
