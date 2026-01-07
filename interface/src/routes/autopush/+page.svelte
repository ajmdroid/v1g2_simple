<script>
	import { onMount } from 'svelte';
	
	let data = $state({
		enabled: false,
		activeSlot: 0,
		slots: []
	});
	
	let profiles = $state([]);
	let loading = $state(true);
	let message = $state(null);
	let editingSlot = $state(null);
	
	const modeNames = {
		0: 'Unknown',
		1: 'All Bogeys',
		2: 'Logic',
		3: 'Adv Logic'
	};
	
	const defaultSlotNames = ['Default', 'Highway', 'Comfort'];
	const slotIcons = ['🏠', '🏎️', '👥'];
	
	onMount(async () => {
		await Promise.all([fetchSlots(), fetchProfiles()]);
		loading = false;
	});
	
	async function fetchSlots() {
		try {
			const res = await fetch('/api/autopush/slots');
			if (res.ok) {
				const loaded = await res.json();
				// Normalize defaults for new fields
				loaded.slots = (loaded.slots || []).map((s) => ({
					...s,
					alertPersist: s.alertPersist ?? 0
				}));
				data = loaded;
			}
		} catch (e) {
			message = { type: 'error', text: 'Failed to load slots' };
		}
	}
	
	async function fetchProfiles() {
		try {
			const res = await fetch('/api/v1/profiles');
			if (res.ok) {
				const d = await res.json();
				profiles = d.profiles || [];
			}
		} catch (e) {
			console.error('Failed to fetch profiles');
		}
	}
	
	async function activateSlot(slot) {
		message = { type: 'info', text: `Activating slot ${slot}...` };
		try {
			const formData = new FormData();
			formData.append('slot', slot);
			formData.append('enable', 'true');
			
			const res = await fetch('/api/autopush/activate', {
				method: 'POST',
				body: formData
			});
			
			if (res.ok) {
				data.activeSlot = slot;
				data.enabled = true;
				message = { type: 'success', text: `Slot ${slot} activated` };
			} else {
				message = { type: 'error', text: 'Failed to activate' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	async function pushNow(slot) {
		message = { type: 'info', text: 'Pushing settings to V1...' };
		try {
			const formData = new FormData();
			formData.append('slot', slot);
			
			const res = await fetch('/api/autopush/push', {
				method: 'POST',
				body: formData
			});
			
			if (res.ok) {
				message = { type: 'success', text: 'Settings pushed to V1!' };
			} else {
				const err = await res.json();
				message = { type: 'error', text: err.error || 'Push failed' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	async function saveSlot(slot) {
		const s = data.slots[slot];
		message = { type: 'info', text: 'Saving slot...' };
		const persist = Math.max(0, Math.min(5, Number(s.alertPersist ?? 0)));
		s.alertPersist = persist;
		
		try {
			const formData = new FormData();
			formData.append('slot', slot);
			formData.append('name', s.name);
			formData.append('profile', s.profile);
			formData.append('mode', s.mode);
			formData.append('volume', s.volume);
			formData.append('muteVol', s.muteVolume);
			formData.append('darkMode', s.darkMode ? 'true' : 'false');
			formData.append('muteToZero', s.muteToZero ? 'true' : 'false');
			formData.append('alertPersist', persist);
			
			const res = await fetch('/api/autopush/slot', {
				method: 'POST',
				body: formData
			});
			
			if (res.ok) {
				message = { type: 'success', text: 'Slot saved!' };
				editingSlot = null;
			} else {
				message = { type: 'error', text: 'Failed to save' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	function getSlotColor(slot) {
		const colors = ['bg-primary', 'bg-secondary', 'bg-accent'];
		return colors[slot] || 'bg-base-300';
	}
</script>

<div class="space-y-6">
	<div class="flex justify-between items-center">
		<h1 class="text-2xl font-bold">Auto-Push Profiles</h1>
		<div class="badge {data.enabled ? 'badge-success' : 'badge-ghost'}">
			{data.enabled ? 'Enabled' : 'Disabled'}
		</div>
	</div>
	
	{#if message}
		<div class="alert alert-{message.type === 'error' ? 'error' : message.type === 'success' ? 'success' : 'info'}" role="status" aria-live="polite">
			<span>{message.text}</span>
		</div>
	{/if}
	
	<div class="text-sm text-base-content/60 bg-base-200 p-3 rounded-lg">
		<p>Auto-Push automatically sends V1 settings when you connect. Configure 3 slots for different driving scenarios and switch between them with a touch.</p>
	</div>
	
	{#if loading}
		<div class="flex justify-center p-8">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<!-- Slot Cards -->
		<div class="grid gap-4">
			{#each data.slots as slot, i}
				<div class="card bg-base-200 {data.activeSlot === i ? 'ring-2 ring-primary' : ''}">
					<div class="card-body p-4">
						<div class="flex justify-between items-start">
							<div class="flex items-center gap-3">
								<div class="text-3xl">{slotIcons[i]}</div>
								<div>
									{#if editingSlot === i}
										<input 
											type="text" 
											class="input input-bordered input-sm w-40"
											bind:value={slot.name}
											placeholder={defaultSlotNames[i]}
										/>
									{:else}
										<h3 class="font-bold text-lg">
											{slot.name || defaultSlotNames[i]}
										</h3>
									{/if}
									{#if data.activeSlot === i}
										<span class="badge badge-primary badge-sm">Active</span>
									{/if}
								</div>
							</div>
							<div class="flex gap-1">
								{#if editingSlot === i}
									<button class="btn btn-success btn-sm" onclick={() => saveSlot(i)}>
										💾 Save
									</button>
									<button class="btn btn-ghost btn-sm" onclick={() => editingSlot = null}>
										Cancel
									</button>
								{:else}
									<button class="btn btn-ghost btn-sm" onclick={() => editingSlot = i}>
										✏️
									</button>
								{/if}
							</div>
						</div>
						
						{#if editingSlot === i}
							<!-- Edit Mode -->
							<div class="grid grid-cols-2 gap-3 mt-3">
								<div class="form-control">
									<!-- provide stable ids for accessibility -->
									<label class="label py-1" for={`slot-${i}-profile`}>
										<span class="label-text text-xs">Profile</span>
									</label>
									<select id={`slot-${i}-profile`} class="select select-bordered select-sm" bind:value={slot.profile}>
										<option value="">-- None --</option>
										{#each profiles as p}
											<option value={p.name}>{p.name}</option>
										{/each}
									</select>
								</div>
								<div class="form-control">
									<label class="label py-1" for={`slot-${i}-mode`}>
										<span class="label-text text-xs">V1 Mode</span>
									</label>
									<select id={`slot-${i}-mode`} class="select select-bordered select-sm" bind:value={slot.mode}>
										<option value={0}>Don't Change</option>
										<option value={1}>All Bogeys</option>
										<option value={2}>Logic</option>
										<option value={3}>Adv Logic</option>
									</select>
								</div>
								<div class="form-control">
									<label class="label py-1" for={`slot-${i}-volume`}>
										<span class="label-text text-xs">Volume (0-9)</span>
									</label>
									<input 
										id={`slot-${i}-volume`}
										type="number" 
										class="input input-bordered input-sm" 
										min="0" max="9"
										bind:value={slot.volume}
									/>
								</div>
								<div class="form-control">
									<label class="label py-1" for={`slot-${i}-mute`}>
										<span class="label-text text-xs">Mute Volume (0-9)</span>
									</label>
									<input 
										id={`slot-${i}-mute`}
										type="number" 
										class="input input-bordered input-sm" 
										min="0" max="9"
										bind:value={slot.muteVolume}
									/>
								</div>
								<div class="form-control">
									<label class="label cursor-pointer justify-start gap-3 py-1">
										<input type="checkbox" class="toggle toggle-sm toggle-primary" bind:checked={slot.darkMode} />
										<span class="label-text text-xs">Dark Mode (V1 display off)</span>
									</label>
								</div>
								<div class="form-control">
									<label class="label cursor-pointer justify-start gap-3 py-1">
										<input type="checkbox" class="toggle toggle-sm toggle-primary" bind:checked={slot.muteToZero} />
										<span class="label-text text-xs">Mute to Zero</span>
									</label>
								</div>
								<div class="form-control">
									<label class="label py-1" for={`slot-${i}-persist`}>
										<span class="label-text text-xs">Alert persistence (seconds)</span>
										<span class="label-text-alt text-[10px] text-base-content/60">0 = off, max 5s</span>
									</label>
									<div class="flex items-center gap-2">
										<input
											id={`slot-${i}-persist`}
											type="range"
											min="0"
											max="5"
											step="1"
											class="range range-primary range-xs flex-1"
											bind:value={slot.alertPersist}
										/>
										<input
											type="number"
											min="0"
											max="5"
											class="input input-bordered input-xs w-16"
											bind:value={slot.alertPersist}
										/>
										<span class="text-xs text-base-content/60">s</span>
									</div>
								</div>
							</div>
						{:else}
							<!-- View Mode -->
							<div class="grid grid-cols-2 gap-x-4 gap-y-1 text-sm mt-2">
								<div class="text-base-content/60">Profile:</div>
								<div class="font-medium">{slot.profile || '—'}</div>
								<div class="text-base-content/60">Mode:</div>
								<div class="font-medium">{modeNames[slot.mode] || '—'}</div>
								<div class="text-base-content/60">Volume:</div>
								<div class="font-medium">{slot.volume} / Mute: {slot.muteVolume}</div>
								<div class="text-base-content/60">Options:</div>
								<div class="font-medium">
									{#if slot.darkMode}🌙 Dark{/if}
									{#if slot.darkMode && slot.muteToZero} · {/if}
									{#if slot.muteToZero}🔇 MZ{/if}
									{#if !slot.darkMode && !slot.muteToZero}—{/if}
								</div>
								<div class="text-base-content/60">Alert persistence:</div>
								<div class="font-medium">{slot.alertPersist || 0}s ghost</div>
							</div>
						{/if}
						
						{#if editingSlot !== i}
							<div class="card-actions justify-end mt-3">
								{#if data.activeSlot !== i}
									<button class="btn btn-outline btn-sm" onclick={() => activateSlot(i)}>
										Activate
									</button>
								{/if}
								<button 
									class="btn btn-primary btn-sm" 
									onclick={() => pushNow(i)}
									disabled={!slot.profile}
								>
									⬆️ Push Now
								</button>
							</div>
						{/if}
					</div>
				</div>
			{/each}
		</div>
		
		<!-- Info -->
		{#if profiles.length === 0}
			<div class="alert alert-warning">
				<span>No saved profiles. Go to V1 Profiles to pull settings from your V1 first.</span>
			</div>
		{/if}
	{/if}
</div>
