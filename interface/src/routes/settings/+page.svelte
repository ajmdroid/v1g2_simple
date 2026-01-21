<script>
	import { onMount } from 'svelte';
	
    let settings = $state({
        ap_ssid: '',
        ap_password: '',
        proxy_ble: true,
        proxy_name: 'V1C-LE-S3',
        autoPowerOffMinutes: 0
    });
	
	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);
	let restoreFile = $state(null);
	let restoring = $state(false);
	
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
            formData.append('autoPowerOffMinutes', settings.autoPowerOffMinutes);
			
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
	
	async function downloadBackup() {
		try {
			const res = await fetch('/api/settings/backup');
			if (res.ok) {
				const blob = await res.blob();
				const url = window.URL.createObjectURL(blob);
				const a = document.createElement('a');
				a.href = url;
				a.download = 'v1simple_backup.json';
				document.body.appendChild(a);
				a.click();
				document.body.removeChild(a);
				window.URL.revokeObjectURL(url);
				message = { type: 'success', text: 'Backup downloaded!' };
			} else {
				message = { type: 'error', text: 'Failed to download backup' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		}
	}
	
	function handleFileSelect(e) {
		const file = e.target.files[0];
		if (file) {
			restoreFile = file;
		}
	}
	
	async function restoreBackup() {
		if (!restoreFile) {
			message = { type: 'error', text: 'Please select a backup file first' };
			return;
		}
		
		// Confirm before overwriting
		if (!confirm('⚠️ This will overwrite all your current settings and profiles.\n\nAre you sure you want to restore from this backup?')) {
			return;
		}
		
		restoring = true;
		message = null;
		
		try {
			const text = await restoreFile.text();
			const res = await fetch('/api/settings/restore', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: text
			});
			
			const data = await res.json();
			if (res.ok && data.success) {
				message = { type: 'success', text: 'Settings restored! Refresh to see changes.' };
				restoreFile = null;
				// Refresh settings
				await fetchSettings();
			} else {
				message = { type: 'error', text: data.error || 'Failed to restore backup' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Failed to read backup file' };
		} finally {
			restoring = false;
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
		
		<!-- Auto Power Off -->
		<div class="card bg-base-200">
			<div class="card-body space-y-4">
				<h2 class="card-title">🔌 Auto Power Off</h2>
				<p class="text-sm text-base-content/60">Automatically power off when V1 disconnects (e.g., when you turn off your car).</p>
				<div class="form-control">
					<label class="label" for="auto-power-off">
						<span class="label-text">Minutes after disconnect (0 = disabled)</span>
					</label>
					<input
						id="auto-power-off"
						type="number"
						class="input input-bordered w-24"
						bind:value={settings.autoPowerOffMinutes}
						min="0"
						max="60"
						placeholder="0"
					/>
					<div class="label">
						<span class="label-text-alt">
							{#if settings.autoPowerOffMinutes > 0}
								Device will power off {settings.autoPowerOffMinutes} minute{settings.autoPowerOffMinutes !== 1 ? 's' : ''} after V1 disconnects
							{:else}
								Auto power-off is disabled
							{/if}
						</span>
					</div>
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
		
		<!-- Backup & Restore -->
		<div class="card bg-base-200">
			<div class="card-body space-y-4">
				<h2 class="card-title">💾 Backup & Restore</h2>
				<p class="text-sm text-base-content/60">Download your settings or restore from a backup file.</p>
				
				<div class="flex flex-col gap-3">
					<button class="btn btn-outline btn-sm" onclick={downloadBackup}>
						⬇️ Download Backup
					</button>
					
					<div class="divider my-0">OR</div>
					
					<input 
						type="file" 
						accept=".json,application/json"
						class="file-input file-input-bordered file-input-sm w-full"
						onchange={handleFileSelect}
					/>
					
					<button 
						class="btn btn-warning btn-sm" 
						onclick={restoreBackup}
						disabled={!restoreFile || restoring}
					>
						{#if restoring}
							<span class="loading loading-spinner loading-sm"></span>
						{/if}
						⬆️ Restore from Backup
					</button>
				</div>
			</div>
		</div>
	{/if}
</div>
