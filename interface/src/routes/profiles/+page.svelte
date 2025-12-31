<script>
	import { onMount } from 'svelte';
	
	let profiles = $state([]);
	let currentProfile = $state(null);
	let loading = $state(true);
	let v1Connected = $state(false);
	let message = $state(null);
	let showSaveDialog = $state(false);
	let saveName = $state('');
	let saveDescription = $state('');
	let editingSettings = $state(false);
	let editedSettings = $state(null);
	let editDescription = $state('');

	function fromApiSettings(api = {}) {
		return {
			ka: api.kaBand ?? api.ka ?? false,
			k: api.kBand ?? api.k ?? false,
			x: api.xBand ?? api.x ?? false,
			ku: api.kuBand ?? api.ku ?? false,
			laser: api.laser ?? false,
			euroMode: api.euro ?? api.euroMode ?? false,
			kVerifier: api.kVerifier ?? false,
			fastLaserDetect: api.fastLaserDetect ?? false,
			laserRear: api.laserRear ?? false,
			customFreqs: api.customFreqs ?? false,
			kaAlwaysPriority: api.kaAlwaysPriority ?? false,
			kaSensitivity: Number(api.kaSensitivity ?? 0),
			kSensitivity: Number(api.kSensitivity ?? 0),
			xSensitivity: Number(api.xSensitivity ?? 0),
			autoMute: Number(api.autoMute ?? 0),
			muteToMuteVolume: api.muteToMuteVolume ?? false,
			bogeyLockLoud: api.bogeyLockLoud ?? false,
			muteXKRear: api.muteXKRear ?? false,
			startupSequence: api.startupSequence ?? false,
			restingDisplay: api.restingDisplay ?? false,
			bsmPlus: api.bsmPlus ?? false,
			mrct: api.mrct ?? false,
			driveSafe3D: api.driveSafe3D ?? false,
			driveSafe3DHD: api.driveSafe3DHD ?? false,
			redflexHalo: api.redflexHalo ?? false,
			redflexNK7: api.redflexNK7 ?? false,
			ekin: api.ekin ?? false,
			photoVerifier: api.photoVerifier ?? false,
		};
	}

	function toApiSettings(ui = {}) {
		return {
			xBand: ui.x ?? ui.xBand ?? false,
			kBand: ui.k ?? ui.kBand ?? false,
			kaBand: ui.ka ?? ui.kaBand ?? false,
			laser: ui.laser ?? false,
			kuBand: ui.ku ?? ui.kuBand ?? false,
			euro: ui.euroMode ?? ui.euro ?? false,
			kVerifier: ui.kVerifier ?? false,
			fastLaserDetect: ui.fastLaserDetect ?? false,
			laserRear: ui.laserRear ?? false,
			customFreqs: ui.customFreqs ?? false,
			kaAlwaysPriority: ui.kaAlwaysPriority ?? false,
			kaSensitivity: Number(ui.kaSensitivity ?? 0),
			kSensitivity: Number(ui.kSensitivity ?? 0),
			xSensitivity: Number(ui.xSensitivity ?? 0),
			autoMute: Number(ui.autoMute ?? 0),
			muteToMuteVolume: ui.muteToMuteVolume ?? false,
			bogeyLockLoud: ui.bogeyLockLoud ?? false,
			muteXKRear: ui.muteXKRear ?? false,
			startupSequence: ui.startupSequence ?? false,
			restingDisplay: ui.restingDisplay ?? false,
			bsmPlus: ui.bsmPlus ?? false,
			mrct: ui.mrct ?? false,
			driveSafe3D: ui.driveSafe3D ?? false,
			driveSafe3DHD: ui.driveSafe3DHD ?? false,
			redflexHalo: ui.redflexHalo ?? false,
			redflexNK7: ui.redflexNK7 ?? false,
			ekin: ui.ekin ?? false,
			photoVerifier: ui.photoVerifier ?? false,
		};
	}
	
	onMount(async () => {
		await fetchProfiles();
		await fetchCurrentSettings();
	});
	
	async function fetchProfiles() {
		try {
			const res = await fetch('/api/v1/profiles');
			if (res.ok) {
				const data = await res.json();
				profiles = data.profiles || [];
			}
		} catch (e) {
			console.error('Failed to fetch profiles');
		} finally {
			loading = false;
		}
	}
	
	async function fetchCurrentSettings() {
		try {
			const res = await fetch('/api/v1/current');
			if (res.ok) {
				const data = await res.json();
				currentProfile = {
					...data,
					settings: fromApiSettings(data.settings || {})
				};
				v1Connected = data.connected || false;
			}
		} catch (e) {
			v1Connected = false;
		}
	}
	
	async function pullFromV1() {
		message = { type: 'info', text: 'Pulling settings from V1...' };
		try {
			const res = await fetch('/api/v1/pull', { method: 'POST' });
			if (res.ok) {
				message = { type: 'success', text: 'Settings pulled from V1' };
				await fetchCurrentSettings();
				// Show save dialog after successful pull
				showSaveDialog = true;
				saveName = '';
				saveDescription = '';
			} else {
				message = { type: 'error', text: 'Failed to pull settings' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	async function saveCurrentProfile() {
		if (!saveName.trim()) {
			message = { type: 'error', text: 'Profile name required' };
			return;
		}
		
		if (!currentProfile || !currentProfile.settings) {
			message = { type: 'error', text: 'No settings to save' };
			return;
		}
		
		try {
			const payload = {
				name: saveName.trim(),
				description: saveDescription.trim(),
				settings: toApiSettings(currentProfile.settings)
			};
			
			const res = await fetch('/api/v1/profile', {
				method: 'POST',
				headers: {
					'Content-Type': 'application/json'
				},
				body: JSON.stringify(payload)
			});
			
			if (res.ok) {
				message = { type: 'success', text: `Profile "${saveName}" saved` };
				showSaveDialog = false;
				await fetchProfiles();
			} else {
				const error = await res.text();
				message = { type: 'error', text: `Failed to save: ${error}` };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	function startEditing() {
		if (currentProfile && currentProfile.settings) {
			editedSettings = { ...currentProfile.settings };
			editDescription = currentProfile.description || '';
			editingSettings = true;
		}
	}
	
	function cancelEditing() {
		editedSettings = null;
		editDescription = '';
		editingSettings = false;
	}

	async function editProfile(name) {
		message = { type: 'info', text: `Loading ${name}...` };
		try {
			const res = await fetch(`/api/v1/profile?name=${encodeURIComponent(name)}`);
			if (res.ok) {
				const data = await res.json();
				currentProfile = {
					...data,
					settings: fromApiSettings(data.settings || {})
				};
				editedSettings = { ...currentProfile.settings };
				editDescription = data.description || '';
				editingSettings = true;
				message = { type: 'info', text: `Editing ${name}` };
			} else {
				const error = await res.text();
				message = { type: 'error', text: `Failed to load: ${error}` };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}

	async function saveEditedProfile() {
		if (!editedSettings || !currentProfile || !currentProfile.name) {
			message = { type: 'error', text: 'No profile loaded to save' };
			return;
		}

		message = { type: 'info', text: `Saving ${currentProfile.name}...` };
		try {
			const payload = {
				name: currentProfile.name,
				description: editDescription.trim(),
				settings: toApiSettings(editedSettings)
			};

			const res = await fetch('/api/v1/profile', {
				method: 'POST',
				headers: {
					'Content-Type': 'application/json'
				},
				body: JSON.stringify(payload)
			});

			if (res.ok) {
				message = { type: 'success', text: `Profile "${currentProfile.name}" saved` };
				editingSettings = false;
				editedSettings = null;
				await fetchProfiles();
				await fetchCurrentSettings();
			} else {
				const error = await res.text();
				message = { type: 'error', text: `Failed to save: ${error}` };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	async function saveEdits() {
		if (!editedSettings) return;
		
		message = { type: 'info', text: 'Pushing edited settings to V1...' };
		
		try {
			// Push edited settings directly to V1
			const payload = {
				settings: toApiSettings(editedSettings)
			};
			
			const res = await fetch('/api/v1/push', {
				method: 'POST',
				headers: {
					'Content-Type': 'application/json'
				},
				body: JSON.stringify(payload)
			});
			
			if (res.ok) {
				message = { type: 'success', text: 'Settings pushed to V1' };
				currentProfile.settings = editedSettings;
				editingSettings = false;
				editedSettings = null;
				await fetchCurrentSettings();
			} else {
				const error = await res.text();
				message = { type: 'error', text: `Failed to push: ${error}` };
			}
		} catch (e) {
			message = { type: 'error', text: `Connection error: ${e.message}` };
		}
	}
	
	async function pushToV1(profileName) {
		message = { type: 'info', text: `Pushing ${profileName} to V1...` };
		try {
			const res = await fetch('/api/v1/push', { 
				method: 'POST',
				headers: {
					'Content-Type': 'application/json'
				},
				body: JSON.stringify({ name: profileName })
			});
			if (res.ok) {
				message = { type: 'success', text: `${profileName} pushed to V1` };
				await fetchCurrentSettings();
			} else {
				const error = await res.text();
				message = { type: 'error', text: `Failed to push: ${error}` };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	async function deleteProfile(name) {
		if (!confirm(`Delete profile "${name}"?`)) return;
		
		try {
			const res = await fetch('/api/v1/profile/delete', { 
				method: 'POST',
				headers: {
					'Content-Type': 'application/json'
				},
				body: JSON.stringify({ name })
			});
			if (res.ok) {
				profiles = profiles.filter(p => p.name !== name);
				message = { type: 'success', text: 'Profile deleted' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Failed to delete' };
		}
	}
</script>

<div class="space-y-6">
	<div class="flex justify-between items-center">
		<h1 class="text-2xl font-bold">V1 Profiles</h1>
		<div class="badge {v1Connected ? 'badge-success' : 'badge-warning'}">
			{v1Connected ? 'V1 Connected' : 'V1 Disconnected'}
		</div>
	</div>
	
	{#if message}
		<div class="alert alert-{message.type === 'error' ? 'error' : message.type === 'success' ? 'success' : 'info'}" role="status" aria-live="polite">
			<span>{message.text}</span>
		</div>
	{/if}
	
	<!-- Save Profile Dialog -->
	{#if showSaveDialog}
		<div class="modal modal-open">
			<div class="modal-box">
				<h3 class="font-bold text-lg">💾 Save Profile</h3>
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
					<button class="btn btn-ghost" onclick={() => showSaveDialog = false}>Cancel</button>
					<button class="btn btn-primary" onclick={saveCurrentProfile}>Save</button>
				</div>
			</div>
		</div>
	{/if}

	<!-- Current V1 Settings -->
	<div class="card bg-base-200">
		<div class="card-body">
			<h2 class="card-title">📡 Current V1 Settings</h2>
			{#if editingSettings && currentProfile?.name}
				<div class="alert alert-info text-sm">
					Editing profile: <span class="font-semibold">{currentProfile.name}</span>
				</div>
				<div class="form-control max-w-md">
					<label class="label" for="edit-description">
						<span class="label-text">Description</span>
					</label>
					<input
						id="edit-description"
						type="text"
						placeholder="Update description"
						class="input input-bordered input-sm"
						bind:value={editDescription}
					/>
				</div>
			{/if}
			{#if (!v1Connected) && !editingSettings}
				<p class="text-warning">Connect to V1 to view/edit settings</p>
			{:else if currentProfile && currentProfile.settings}
				{@const settings = editingSettings ? editedSettings : currentProfile.settings}
				<div class="grid grid-cols-2 md:grid-cols-3 gap-3 text-sm">
					<!-- Band Enables -->
					<div class="col-span-2 md:col-span-3 font-bold text-base">Band Detection</div>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.ka} disabled={!editingSettings} />
						<span>Ka Band</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.k} disabled={!editingSettings} />
						<span>K Band</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.x} disabled={!editingSettings} />
						<span>X Band</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.laser} disabled={!editingSettings} />
						<span>Laser</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.ku} disabled={!editingSettings} />
						<span>Ku Band</span>
					</label>
					
					<!-- Sensitivities -->
					<div class="col-span-2 md:col-span-3 font-bold text-base mt-2">Sensitivity</div>
					<div class="flex items-center gap-2">
						<span class="min-w-12">Ka:</span>
						{#if editingSettings}
							<select class="select select-bordered select-xs" bind:value={settings.kaSensitivity}>
								<option value={1}>Relaxed</option>
								<option value={2}>Original</option>
								<option value={3}>Full</option>
							</select>
						{:else}
							<span>{settings.kaSensitivity === 3 ? 'Full' : settings.kaSensitivity === 2 ? 'Original' : 'Relaxed'}</span>
						{/if}
					</div>
					<div class="flex items-center gap-2">
						<span class="min-w-12">K:</span>
						{#if editingSettings}
							<select class="select select-bordered select-xs" bind:value={settings.kSensitivity}>
								<option value={1}>Relaxed</option>
								<option value={2}>Full</option>
								<option value={3}>Original</option>
							</select>
						{:else}
							<span>{settings.kSensitivity === 3 ? 'Original' : settings.kSensitivity === 2 ? 'Full' : 'Relaxed'}</span>
						{/if}
					</div>
					<div class="flex items-center gap-2">
						<span class="min-w-12">X:</span>
						{#if editingSettings}
							<select class="select select-bordered select-xs" bind:value={settings.xSensitivity}>
								<option value={1}>Relaxed</option>
								<option value={2}>Full</option>
								<option value={3}>Original</option>
							</select>
						{:else}
							<span>{settings.xSensitivity === 3 ? 'Original' : settings.xSensitivity === 2 ? 'Full' : 'Relaxed'}</span>
						{/if}
					</div>
					
					<!-- Features -->
					<div class="col-span-2 md:col-span-3 font-bold text-base mt-2">Features</div>
					<div class="flex items-center gap-2">
						<span class="min-w-24">Auto Mute:</span>
						{#if editingSettings}
							<select class="select select-bordered select-xs" bind:value={settings.autoMute}>
								<option value={1}>On</option>
								<option value={2}>Advanced</option>
								<option value={3}>Off</option>
							</select>
						{:else}
							<span>{settings.autoMute === 3 ? 'Off' : settings.autoMute === 2 ? 'Advanced' : 'On'}</span>
						{/if}
					</div>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.kVerifier} disabled={!editingSettings} />
						<span>K Verifier (TMF)</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.fastLaserDetect} disabled={!editingSettings} />
						<span>Fast Laser</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.laserRear} disabled={!editingSettings} />
						<span>Laser Rear</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.euroMode} disabled={!editingSettings} />
						<span>Euro Mode</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.mrct} disabled={!editingSettings} />
						<span>MRCT</span>
					</label>
					
					<!-- Display -->
					<div class="col-span-2 md:col-span-3 font-bold text-base mt-2">Display</div>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.startupSequence} disabled={!editingSettings} />
						<span>Startup Sequence</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.restingDisplay} disabled={!editingSettings} />
						<span>Resting Display</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.bogeyLockLoud} disabled={!editingSettings} />
						<span>Bogey Lock Loud</span>
					</label>
					
					<!-- Advanced -->
					<div class="col-span-2 md:col-span-3 font-bold text-base mt-2">Advanced</div>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.muteXKRear} disabled={!editingSettings} />
						<span>Mute X/K Rear</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.kaAlwaysPriority} disabled={!editingSettings} />
						<span>Ka Always Priority</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.muteToMuteVolume} disabled={!editingSettings} />
						<span>Mute to Mute Vol</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.customFreqs} disabled={!editingSettings} />
						<span>Custom Freqs</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.bsmPlus} disabled={!editingSettings} />
						<span>BSM Plus</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.driveSafe3D} disabled={!editingSettings} />
						<span>Drive Safe 3D</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.driveSafe3DHD} disabled={!editingSettings} />
						<span>Drive Safe 3D HD</span>
					</label>
					<label class="flex items-center gap-2">
						<input type="checkbox" class="checkbox checkbox-sm" bind:checked={settings.redflexHalo} disabled={!editingSettings} />
						<span>Redflex Halo</span>
					</label>
				</div>
			{:else}
				<p class="text-base-content/60">No settings available. Pull from V1 to view.</p>
			{/if}
			<div class="card-actions justify-end gap-2">
				{#if editingSettings}
					<button class="btn btn-ghost btn-sm" onclick={cancelEditing}>
						Cancel
					</button>
					<button class="btn btn-success btn-sm" onclick={saveEdits}>
						✅ Push to V1
					</button>
					{#if currentProfile?.name}
						<button class="btn btn-primary btn-sm" onclick={saveEditedProfile}>
							💾 Save Profile
						</button>
					{/if}
				{:else}
					<button class="btn btn-primary btn-sm" onclick={pullFromV1} disabled={!v1Connected}>
						⬇️ Pull from V1
					</button>
					{#if currentProfile && currentProfile.settings}
						<button class="btn btn-secondary btn-sm" onclick={startEditing} disabled={!v1Connected}>
							✏️ Edit
						</button>
						<button class="btn btn-success btn-sm" onclick={() => showSaveDialog = true}>
							💾 Save
						</button>
					{/if}
				{/if}
			</div>
		</div>
	</div>
	
	<!-- Saved Profiles -->
	<div class="card bg-base-200">
		<div class="card-body">
			<h2 class="card-title">💾 Saved Profiles</h2>
			
			{#if loading}
				<div class="flex justify-center p-4">
					<span class="loading loading-spinner"></span>
				</div>
			{:else if profiles.length === 0}
				<p class="text-base-content/60">No saved profiles. Pull settings from V1 to create one.</p>
			{:else}
				<div class="space-y-2">
					{#each profiles as profile}
						<div class="flex justify-between items-center p-3 bg-base-300 rounded-lg">
							<div>
								<div class="font-medium">{profile.name}</div>
								<div class="text-xs text-base-content/60">
									{profile.description || 'No description'}
								</div>
							</div>
							<div class="flex gap-2">
								<button 
									class="btn btn-secondary btn-xs"
									onclick={() => editProfile(profile.name)}
								>
									✏️ Edit
								</button>
								<button 
									class="btn btn-primary btn-xs" 
									onclick={() => pushToV1(profile.name)}
									disabled={!v1Connected}
								>
									⬆️ Push
								</button>
								<button 
									class="btn btn-error btn-xs btn-outline" 
									onclick={() => deleteProfile(profile.name)}
								>
									🗑️
								</button>
							</div>
						</div>
					{/each}
				</div>
			{/if}
		</div>
	</div>
	
	<!-- Info -->
	<div class="text-sm text-base-content/50">
		<p><strong>Pull:</strong> Read current V1 settings (shows in Current V1 Settings)</p>
		<p><strong>Edit:</strong> Modify settings and push directly to V1</p>
		<p><strong>Save:</strong> Store current V1 settings as a named profile</p>
		<p><strong>Push:</strong> Send a saved profile to your V1</p>
	</div>
</div>
