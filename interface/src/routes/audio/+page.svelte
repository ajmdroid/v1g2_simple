<script>
	import { onMount } from 'svelte';
	
	let settings = $state({
		voiceAlertsEnabled: true,
		muteVoiceIfVolZero: false,
		voiceVolume: 75  // Speaker volume (0-100)
	});
	
	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);
	
	onMount(async () => {
		await fetchSettings();
	});
	
	async function fetchSettings() {
		loading = true;
		try {
			const res = await fetch('/api/displaycolors');
			if (res.ok) {
				const data = await res.json();
				settings.voiceAlertsEnabled = data.voiceAlertsEnabled ?? true;
				settings.muteVoiceIfVolZero = data.muteVoiceIfVolZero ?? false;
				settings.voiceVolume = data.voiceVolume ?? 75;
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
			const params = new URLSearchParams();
			params.append('voiceAlertsEnabled', settings.voiceAlertsEnabled);
			params.append('muteVoiceIfVolZero', settings.muteVoiceIfVolZero);
			params.append('voiceVolume', settings.voiceVolume);
			
			const res = await fetch('/api/displaycolors', {
				method: 'POST',
				headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
				body: params
			});
			
			if (res.ok) {
				message = { type: 'success', text: 'Audio settings saved!' };
			} else {
				message = { type: 'error', text: 'Failed to save settings' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		} finally {
			saving = false;
		}
	}
</script>

<div class="space-y-6">
	<div class="flex justify-between items-center">
		<div>
			<h1 class="text-2xl font-bold">Audio Settings</h1>
			<p class="text-sm text-base-content/60">Voice alerts and speaker options</p>
		</div>
	</div>
	
	{#if message}
		<div class="alert alert-{message.type === 'error' ? 'error' : 'success'}">
			<span>{message.text}</span>
		</div>
	{/if}
	
	{#if loading}
		<div class="flex justify-center p-8">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<!-- Voice Alerts -->
		<div class="card bg-base-200">
			<div class="card-body p-4">
				<h2 class="card-title text-lg">🔊 Voice Alerts</h2>
				<p class="text-xs text-base-content/50 mb-4">Speak alert band and direction through the built-in speaker when no phone app is connected</p>
				
				<div class="space-y-4">
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Enable Voice Alerts</span>
								<p class="text-xs text-base-content/50">Announces "Ka ahead", "Laser behind", etc.</p>
							</div>
							<input 
								type="checkbox" 
								class="toggle toggle-primary" 
								bind:checked={settings.voiceAlertsEnabled}
							/>
						</label>
					</div>
					
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Mute Voice at Volume 0</span>
								<p class="text-xs text-base-content/50">Silence alert announcements when V1 volume is 0</p>
								<p class="text-xs text-warning/70 mt-1">⚠️ "Warning Volume Zero" will still play</p>
							</div>
							<input 
								type="checkbox" 
								class="toggle toggle-primary" 
								bind:checked={settings.muteVoiceIfVolZero}
								disabled={!settings.voiceAlertsEnabled}
							/>
						</label>
					</div>
					
					<div class="divider my-2"></div>
					
					<div class="form-control">
						<label class="label" for="voice-volume-slider">
							<span class="label-text font-medium">Speaker Volume</span>
							<span class="label-text-alt">{settings.voiceVolume}%</span>
						</label>
						<div class="flex items-center gap-3">
							<span class="text-lg">🔈</span>
							<input 
								id="voice-volume-slider"
								type="range" 
								min="0" 
								max="100" 
								bind:value={settings.voiceVolume}
								class="range range-primary flex-1" 
							/>
							<span class="text-lg">🔊</span>
						</div>
						<p class="text-xs text-base-content/50 mt-1">Controls the Waveshare ES8311 DAC output level</p>
					</div>
				</div>
			</div>
		</div>
		
		<!-- Info Card -->
		<div class="card bg-base-200">
			<div class="card-body p-4">
				<h2 class="card-title text-lg">ℹ️ How It Works</h2>
				<ul class="text-sm text-base-content/70 space-y-2 list-disc list-inside">
					<li>Voice alerts only play when <strong>no phone app</strong> (JBV1) is connected</li>
					<li>Announces priority alert: band (Laser, Ka, K, X) + direction (ahead, behind, side)</li>
					<li>2-second cooldown between announcements to prevent spam</li>
					<li>Re-announces when band or direction changes</li>
				</ul>
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
			Save Audio Settings
		</button>
	{/if}
</div>
