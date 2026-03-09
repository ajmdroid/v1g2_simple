<script>
	let {
		open = false,
		saveName = $bindable(''),
		saveDescription = $bindable(''),
		oncancel,
		onsave
	} = $props();
</script>

{#if open}
	<div class="modal modal-open">
		<div class="modal-box surface-modal">
			<h3 class="font-bold text-lg">Save Profile</h3>
			<div class="py-4 space-y-4">
				<div class="form-control">
					<label class="label" for="profile-name">
						<span class="label-text">Profile Name</span>
					</label>
					<input
						id="profile-name"
						type="text"
						placeholder="e.g., Highway, City, Custom"
						class="input input-bordered w-full"
						bind:value={saveName}
					/>
				</div>
				<div class="form-control">
					<label class="label" for="profile-description">
						<span class="label-text">Description (optional)</span>
					</label>
					<input
						id="profile-description"
						type="text"
						placeholder="e.g., Max sensitivity for open roads"
						class="input input-bordered w-full"
						bind:value={saveDescription}
					/>
				</div>
			</div>
			<div class="modal-action">
				<button class="btn btn-ghost" onclick={oncancel}>Cancel</button>
				<button class="btn btn-primary" onclick={onsave}>Save</button>
			</div>
		</div>
	</div>
{/if}
