<script>
	import { onMount } from 'svelte';
	import { fetchWithTimeout } from '$lib/utils/poll';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	import ProfileSaveDialog from '$lib/features/profiles/ProfileSaveDialog.svelte';
	import ProfileSavedListCard from '$lib/features/profiles/ProfileSavedListCard.svelte';
	import ProfileSettingsPanel from '$lib/features/profiles/ProfileSettingsPanel.svelte';
	import { fromApiSettings, toApiSettings } from '$lib/features/profiles/profileSettingsAdapter';

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

	onMount(async () => {
		await fetchProfiles();
		await fetchCurrentSettings();
	});

	async function fetchProfiles() {
		try {
			const res = await fetchWithTimeout('/api/v1/profiles');
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
			const res = await fetchWithTimeout('/api/v1/current');
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
			const res = await fetchWithTimeout('/api/v1/pull', { method: 'POST' });
			if (res.ok) {
				let attempts = 0;
				const maxAttempts = 5;
				const delay = 300;

				while (attempts < maxAttempts) {
					await new Promise((resolve) => setTimeout(resolve, delay));
					await fetchCurrentSettings();
					attempts++;
					if (currentProfile?.settings) break;
				}

				message = {
					type: 'success',
					text: 'Settings pulled from V1. Review below, then click Save to store as a profile.'
				};
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

			const res = await fetchWithTimeout('/api/v1/profile', {
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

	function openSaveDialog() {
		showSaveDialog = true;
	}

	function closeSaveDialog() {
		showSaveDialog = false;
	}

	async function editProfile(name) {
		message = { type: 'info', text: `Loading ${name}...` };
		try {
			const res = await fetchWithTimeout(`/api/v1/profile?name=${encodeURIComponent(name)}`);
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

			const res = await fetchWithTimeout('/api/v1/profile', {
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
			const payload = {
				settings: toApiSettings(editedSettings)
			};

			const res = await fetchWithTimeout('/api/v1/push', {
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
			const res = await fetchWithTimeout('/api/v1/push', {
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
			const res = await fetchWithTimeout('/api/v1/profile/delete', {
				method: 'POST',
				headers: {
					'Content-Type': 'application/json'
				},
				body: JSON.stringify({ name })
			});
			if (res.ok) {
				profiles = profiles.filter((profile) => profile.name !== name);
				message = { type: 'success', text: 'Profile deleted' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Failed to delete' };
		}
	}
</script>

<div class="page-stack">
	<PageHeader title="V1 Profiles" subtitle="Pull, edit, save, and push complete V1 settings profiles.">
		<div class="badge {v1Connected ? 'badge-success' : 'badge-warning'}">
			{v1Connected ? 'V1 Connected' : 'V1 Disconnected'}
		</div>
	</PageHeader>

	<StatusAlert {message} />

	<ProfileSaveDialog
		open={showSaveDialog}
		bind:saveName
		bind:saveDescription
		oncancel={closeSaveDialog}
		onsave={saveCurrentProfile}
	/>

	<ProfileSettingsPanel
		{v1Connected}
		{editingSettings}
		{currentProfile}
		bind:editedSettings
		bind:editDescription
		oncancelEditing={cancelEditing}
		onsaveEdits={saveEdits}
		onsaveEditedProfile={saveEditedProfile}
		onpullFromV1={pullFromV1}
		onstartEditing={startEditing}
		onshowSaveDialog={openSaveDialog}
	/>

	<ProfileSavedListCard
		{loading}
		{profiles}
		{v1Connected}
		oneditProfile={editProfile}
		onpushToV1={pushToV1}
		ondeleteProfile={deleteProfile}
	/>

	<div class="surface-note copy-muted space-y-1">
		<p><strong>Pull:</strong> Read current V1 settings (shows in Current V1 Settings)</p>
		<p><strong>Edit:</strong> Modify settings and push directly to V1</p>
		<p><strong>Save:</strong> Store current V1 settings as a named profile</p>
		<p><strong>Push:</strong> Send a saved profile to your V1</p>
	</div>
</div>
