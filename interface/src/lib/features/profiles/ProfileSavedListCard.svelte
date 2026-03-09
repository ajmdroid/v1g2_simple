<script>
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';

	let { loading, profiles = [], v1Connected, oneditProfile, onpushToV1, ondeleteProfile } = $props();
</script>

<div class="surface-card">
	<div class="card-body">
		<CardSectionHead
			title="Saved Profiles"
			subtitle="Named snapshots you can edit, push to V1, or delete."
		/>

		{#if loading}
			<div class="state-loading compact">
				<span class="loading loading-spinner"></span>
			</div>
		{:else if profiles.length === 0}
			<p class="state-empty">No saved profiles. Pull settings from V1 to create one.</p>
		{:else}
			<div class="space-y-2">
				{#each profiles as profile}
					<div class="surface-panel flex items-center justify-between">
						<div>
							<div class="font-medium">{profile.name}</div>
							<div class="copy-caption">
								{profile.description || 'No description'}
							</div>
						</div>
						<div class="flex gap-2">
							<button class="btn btn-secondary btn-xs" onclick={() => oneditProfile(profile.name)}>
								Edit
							</button>
							<button class="btn btn-primary btn-xs" onclick={() => onpushToV1(profile.name)} disabled={!v1Connected}>
								Push
							</button>
							<button class="btn btn-error btn-xs btn-outline" onclick={() => ondeleteProfile(profile.name)}>
								Delete
							</button>
						</div>
					</div>
				{/each}
			</div>
		{/if}
	</div>
</div>
