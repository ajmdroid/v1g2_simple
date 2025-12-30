<script>
	import { onMount } from 'svelte';
	
	let devices = $state([]);
	let autoPushSlots = $state([]);
	let loading = $state(true);
	let message = $state(null);
	let editingDevice = $state(null);
	let editName = $state('');
	
	onMount(async () => {
		await Promise.all([fetchDevices(), fetchAutoPushSlots()]);
	});
	
	async function fetchDevices() {
		loading = true;
		try {
			const res = await fetch('/api/v1/devices');
			if (res.ok) {
				const data = await res.json();
				devices = data.devices || [];
			} else {
				message = { type: 'error', text: 'Failed to load devices' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		} finally {
			loading = false;
		}
	}
	
	async function fetchAutoPushSlots() {
		try {
			const res = await fetch('/api/autopush/slots');
			if (res.ok) {
				const data = await res.json();
				autoPushSlots = data.slots || [];
			}
		} catch (e) {
			// Silent fail - just won't show slot names
		}
	}
	
	function getSlotName(slotNum) {
		if (slotNum === 0) return 'None (manual)';
		// slotNum is 1-3, array is 0-indexed
		const slot = autoPushSlots[slotNum - 1];
		return slot?.name || `Slot ${slotNum}`;
	}
	
	function startEdit(device) {
		editingDevice = device.address;
		editName = device.name || '';
	}
	
	function cancelEdit() {
		editingDevice = null;
		editName = '';
	}
	
	async function saveName(address) {
		try {
			const formData = new FormData();
			formData.append('address', address);
			formData.append('name', editName);
			
			const res = await fetch('/api/v1/devices/name', {
				method: 'POST',
				body: formData
			});
			
			if (res.ok) {
				// Update local state
				const device = devices.find(d => d.address === address);
				if (device) {
					device.name = editName;
				}
				message = { type: 'success', text: 'Name saved!' };
				editingDevice = null;
				editName = '';
			} else {
				message = { type: 'error', text: 'Failed to save' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	async function saveProfile(address, profile) {
		try {
			const formData = new FormData();
			formData.append('address', address);
			formData.append('profile', profile);
			
			const res = await fetch('/api/v1/devices/profile', {
				method: 'POST',
				body: formData
			});
			
			if (res.ok) {
				// Update local state
				const device = devices.find(d => d.address === address);
				if (device) {
					device.defaultProfile = parseInt(profile);
				}
				message = { type: 'success', text: 'Default profile saved!' };
			} else {
				message = { type: 'error', text: 'Failed to save profile' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	async function deleteDevice(address) {
		const device = devices.find(d => d.address === address);
		const displayName = device?.name || address;
		
		if (!confirm(`Delete "${displayName}" from fast reconnect cache?\n\nThis will remove the device from automatic reconnection.`)) {
			return;
		}
		
		try {
			const formData = new FormData();
			formData.append('address', address);
			
			const res = await fetch('/api/v1/devices/delete', {
				method: 'POST',
				body: formData
			});
			
			if (res.ok) {
				devices = devices.filter(d => d.address !== address);
				message = { type: 'success', text: 'Device removed' };
			} else {
				message = { type: 'error', text: 'Failed to delete' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	function formatAddress(addr) {
		// Make MAC address more readable
		return addr.toUpperCase();
	}
</script>

<div class="space-y-6">
	<div class="flex justify-between items-center">
		<div>
			<h1 class="text-2xl font-bold">Saved V1 Devices</h1>
			<p class="text-sm text-base-content/60">Fast reconnect & auto-push profiles</p>
		</div>
		<button class="btn btn-sm btn-outline" onclick={fetchDevices}>
			🔄 Refresh
		</button>
	</div>
	
	{#if message}
		<div class="alert alert-{message.type === 'error' ? 'error' : message.type === 'success' ? 'success' : 'info'}">
			<span>{message.text}</span>
			<button class="btn btn-ghost btn-xs" onclick={() => message = null}>✕</button>
		</div>
	{/if}
	
	<div class="text-sm text-base-content/60 bg-base-200 p-3 rounded-lg">
		<p>📡 Saved V1 devices for fast reconnect. Set a default auto-push profile for each device - perfect for different vehicles!</p>
	</div>
	
	{#if loading}
		<div class="flex justify-center p-8">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else if devices.length === 0}
		<div class="card bg-base-200">
			<div class="card-body text-center">
				<div class="text-4xl mb-2">📭</div>
				<p class="text-base-content/60">No saved V1 devices yet</p>
				<p class="text-sm text-base-content/40">Connect to a V1 and it will be saved automatically</p>
			</div>
		</div>
	{:else}
		<div class="space-y-3">
			{#each devices as device}
				<div class="card bg-base-200">
					<div class="card-body p-4">
						<div class="flex justify-between items-start gap-2">
							<div class="flex items-center gap-3 flex-1">
								<div class="text-3xl">📡</div>
								<div class="flex-1">
									{#if editingDevice === device.address}
										<input 
											type="text" 
											class="input input-bordered input-sm w-full max-w-48"
											bind:value={editName}
											placeholder="Enter a name..."
											onkeydown={(e) => e.key === 'Enter' && saveName(device.address)}
										/>
									{:else}
										<h3 class="font-bold">
											{device.name || 'Unnamed V1'}
										</h3>
									{/if}
									<div class="text-xs font-mono text-base-content/50">
										{formatAddress(device.address)}
									</div>
								</div>
							</div>
							<div class="flex gap-1">
								{#if editingDevice === device.address}
									<button class="btn btn-success btn-sm" onclick={() => saveName(device.address)}>
										💾
									</button>
									<button class="btn btn-ghost btn-sm" onclick={cancelEdit}>
										✕
									</button>
								{:else}
									<button class="btn btn-ghost btn-sm" onclick={() => startEdit(device)} title="Edit name">
										✏️
									</button>
									<button class="btn btn-ghost btn-sm text-error" onclick={() => deleteDevice(device.address)} title="Delete">
										🗑️
									</button>
								{/if}
							</div>
						</div>
						
						<!-- Default Auto-Push Profile Selector -->
						<div class="mt-3 pt-3 border-t border-base-300">
							<div class="flex items-center gap-2">
								<span class="text-sm text-base-content/70">🚗 Default Profile:</span>
								<select 
									class="select select-bordered select-sm flex-1 max-w-xs"
									value={device.defaultProfile || 0}
									onchange={(e) => saveProfile(device.address, e.target.value)}
								>
									<option value="0">None (manual)</option>
									{#each [1, 2, 3] as slot}
										<option value={slot}>{getSlotName(slot)}</option>
									{/each}
								</select>
							</div>
							{#if device.defaultProfile > 0}
								<p class="text-xs text-base-content/50 mt-1">
									Will auto-push "{getSlotName(device.defaultProfile)}" on connect
								</p>
							{/if}
						</div>
					</div>
				</div>
			{/each}
		</div>
		
		<div class="text-xs text-base-content/40 text-center">
			{devices.length} device{devices.length === 1 ? '' : 's'} saved
		</div>
	{/if}
</div>
