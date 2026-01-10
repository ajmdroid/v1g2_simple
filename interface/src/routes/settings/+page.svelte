<script>
	import { onMount } from 'svelte';
	
    let settings = $state({
        ap_ssid: '',
        ap_password: '',
        proxy_ble: true,
        proxy_name: 'V1C-LE-S3'
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
			formData.append('ap_ssid', settings.ap_ssid);
			formData.append('ap_password', settings.ap_password);
			formData.append('proxy_ble', settings.proxy_ble);
            formData.append('proxy_name', settings.proxy_name);
			
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
	
	// AP-only UI; STA/scan removed by design
</script>

<div class="space-y-6">
	<h1 class="text-2xl font-bold">Settings</h1>
	
	{#if message}
		<div class="alert alert-{message.type === 'error' ? 'error' : message.type === 'success' ? 'success' : 'info'}" role="status" aria-live="polite">
			<span>{message.text}</span>
		</div>
	{/if}
	
	{#if loading}
		<div class="flex justify-center p-8">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<!-- AP Settings -->
		<div class="card bg-base-200">
			<div class="card-body">
				<h2 class="card-title">📡 Access Point (AP-only)</h2>
				<p class="text-sm text-base-content/60">Device always hosts its own hotspot; station mode is intentionally disabled.</p>
				
				<div class="form-control">
					<label class="label" for="ap-ssid">
						<span class="label-text">AP Name</span>
					</label>
					<input 
						id="ap-ssid"
						type="text" 
						class="input input-bordered" 
						bind:value={settings.ap_ssid}
						placeholder="V1G2-Display"
					/>
				</div>

				<div class="form-control">
					<label class="label" for="ap-password">
						<span class="label-text">AP Password</span>
					</label>
					<input 
						id="ap-password"
						type="password" 
						class="input input-bordered" 
						bind:value={settings.ap_password}
						placeholder="At least 8 characters"
					/>
				</div>
			</div>
		</div>
		
		<!-- BLE Proxy -->
		<div class="card bg-base-200">
			<div class="card-body space-y-4">
				<h2 class="card-title">🟦 Bluetooth Proxy</h2>
				<p class="text-sm text-base-content/60">Relay V1 data to apps like JBV1.</p>
				<label class="label cursor-pointer">
					<span class="label-text">Enable Proxy</span>
					<input type="checkbox" class="toggle" bind:checked={settings.proxy_ble} />
				</label>
				<div class="form-control">
					<label class="label" for="proxy-name">
						<span class="label-text">Proxy Name</span>
					</label>
					<input
						id="proxy-name"
						type="text"
						class="input input-bordered"
						bind:value={settings.proxy_name}
						placeholder="V1C-LE-S3"
						disabled={!settings.proxy_ble}
					/>
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
