<script>
	import { onMount } from 'svelte';
	
	let settings = $state({
		ssid: '',
		password: '',
		ap_ssid: '',
		ap_password: '',
		wifi_mode: 2
	});
	
	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);
	
	onMount(async () => {
		await fetchSettings();
	});
	
	async function fetchSettings() {
		try {
			const res = await fetch('/api/settings');
			if (res.ok) {
				const data = await res.json();
				settings = { ...settings, ...data };
			}
		} catch (e) {
			message = { type: 'error', text: 'Failed to load settings' };
		} finally {
			loading = false;
		}
	}
	
	async function saveSettings() {
		saving = true;
		message = null;
		
		try {
			const formData = new FormData();
			formData.append('ssid', settings.ssid);
			formData.append('password', settings.password);
			formData.append('ap_ssid', settings.ap_ssid);
			formData.append('ap_password', settings.ap_password);
			formData.append('wifi_mode', settings.wifi_mode);
			
			const res = await fetch('/settings', {
				method: 'POST',
				body: formData
			});
			
			if (res.ok) {
				message = { type: 'success', text: 'Settings saved! WiFi will restart.' };
			} else {
				message = { type: 'error', text: 'Failed to save settings' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		} finally {
			saving = false;
		}
	}
	
	async function scanNetworks() {
		message = { type: 'info', text: 'Scanning for networks...' };
		try {
			const res = await fetch('/api/scan');
			if (res.ok) {
				const data = await res.json();
				// Could populate a dropdown here
				message = { type: 'success', text: `Found ${data.networks?.length || 0} networks` };
			}
		} catch (e) {
			message = { type: 'error', text: 'Scan failed' };
		}
	}
</script>

<div class="space-y-6">
	<h1 class="text-2xl font-bold">Settings</h1>
	
	{#if message}
		<div class="alert alert-{message.type === 'error' ? 'error' : message.type === 'success' ? 'success' : 'info'}">
			<span>{message.text}</span>
		</div>
	{/if}
	
	{#if loading}
		<div class="flex justify-center p-8">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<!-- WiFi Station Settings -->
		<div class="card bg-base-200">
			<div class="card-body">
				<h2 class="card-title">📶 WiFi Network</h2>
				<p class="text-sm text-base-content/60">Connect to your home/car WiFi network</p>
				
				<div class="form-control">
					<label class="label">
						<span class="label-text">Network Name (SSID)</span>
					</label>
					<input 
						type="text" 
						class="input input-bordered" 
						bind:value={settings.ssid}
						placeholder="Your WiFi network"
					/>
				</div>
				
				<div class="form-control">
					<label class="label">
						<span class="label-text">Password</span>
					</label>
					<input 
						type="password" 
						class="input input-bordered" 
						bind:value={settings.password}
						placeholder="WiFi password"
					/>
				</div>
				
				<button class="btn btn-outline btn-sm mt-2" onclick={scanNetworks}>
					🔍 Scan Networks
				</button>
			</div>
		</div>
		
		<!-- AP Settings -->
		<div class="card bg-base-200">
			<div class="card-body">
				<h2 class="card-title">📡 Access Point</h2>
				<p class="text-sm text-base-content/60">The WiFi network this device broadcasts</p>
				
				<div class="form-control">
					<label class="label">
						<span class="label-text">AP Name</span>
					</label>
					<input 
						type="text" 
						class="input input-bordered" 
						bind:value={settings.ap_ssid}
						placeholder="V1G2-Display"
					/>
				</div>
				
				<div class="form-control">
					<label class="label">
						<span class="label-text">AP Password</span>
					</label>
					<input 
						type="password" 
						class="input input-bordered" 
						bind:value={settings.ap_password}
						placeholder="At least 8 characters"
					/>
				</div>
			</div>
		</div>
		
		<!-- WiFi Mode -->
		<div class="card bg-base-200">
			<div class="card-body">
				<h2 class="card-title">🔧 WiFi Mode</h2>
				
				<div class="form-control">
					<label class="label cursor-pointer">
						<span class="label-text">AP Only (creates hotspot)</span>
						<input type="radio" name="wifi_mode" class="radio" value={0} bind:group={settings.wifi_mode} />
					</label>
				</div>
				<div class="form-control">
					<label class="label cursor-pointer">
						<span class="label-text">Station Only (connects to network)</span>
						<input type="radio" name="wifi_mode" class="radio" value={1} bind:group={settings.wifi_mode} />
					</label>
				</div>
				<div class="form-control">
					<label class="label cursor-pointer">
						<span class="label-text">AP + Station (recommended)</span>
						<input type="radio" name="wifi_mode" class="radio" value={2} bind:group={settings.wifi_mode} />
					</label>
				</div>
			</div>
		</div>
		
		<!-- Save Button -->
		<button 
			class="btn btn-primary btn-block" 
			onclick={saveSettings}
			disabled={saving}
		>
			{#if saving}
				<span class="loading loading-spinner loading-sm"></span>
			{/if}
			Save Settings
		</button>
	{/if}
</div>
