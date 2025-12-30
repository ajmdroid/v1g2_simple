<script>
	import { onMount } from 'svelte';
	
	let profiles = $state([]);
	let currentProfile = $state(null);
	let loading = $state(true);
	let v1Connected = $state(false);
	let message = $state(null);
	
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
				currentProfile = data;
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
			} else {
				message = { type: 'error', text: 'Failed to pull settings' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	async function pushToV1(profileName) {
		message = { type: 'info', text: `Pushing ${profileName} to V1...` };
		try {
			const formData = new FormData();
			formData.append('name', profileName);
			const res = await fetch('/api/v1/push', { 
				method: 'POST',
				body: formData
			});
			if (res.ok) {
				message = { type: 'success', text: `${profileName} pushed to V1` };
			} else {
				message = { type: 'error', text: 'Failed to push profile' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	async function deleteProfile(name) {
		if (!confirm(`Delete profile "${name}"?`)) return;
		
		try {
			const formData = new FormData();
			formData.append('name', name);
			const res = await fetch('/api/v1/profile/delete', { 
				method: 'POST',
				body: formData 
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
	
	<!-- Current V1 Settings -->
	<div class="card bg-base-200">
		<div class="card-body">
			<h2 class="card-title">📡 Current V1 Settings</h2>
			{#if !v1Connected}
				<p class="text-warning">Connect to V1 to view/edit settings</p>
			{:else if currentProfile}
				<div class="grid grid-cols-2 gap-2 text-sm">
					<div>Ka Enabled: {currentProfile.ka ? '✅' : '❌'}</div>
					<div>K Enabled: {currentProfile.k ? '✅' : '❌'}</div>
					<div>X Enabled: {currentProfile.x ? '✅' : '❌'}</div>
					<div>Laser Enabled: {currentProfile.laser ? '✅' : '❌'}</div>
				</div>
			{/if}
			<div class="card-actions justify-end">
				<button class="btn btn-primary btn-sm" onclick={pullFromV1} disabled={!v1Connected}>
					⬇️ Pull from V1
				</button>
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
		<p><strong>Pull:</strong> Read current V1 settings and save as a profile</p>
		<p><strong>Push:</strong> Send saved profile settings to your V1</p>
	</div>
</div>
