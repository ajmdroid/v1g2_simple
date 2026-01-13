<script>
	import { onMount } from 'svelte';
	
	let settings = $state({
		voiceAlertMode: 3,  // 0=disabled, 1=band, 2=freq, 3=band+freq
		voiceDirectionEnabled: true,
		muteVoiceIfVolZero: false,
		voiceVolume: 75  // Speaker volume (0-100)
	});
	
	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);
	
	// Voice mode options for dropdown
	const voiceModes = [
		{ value: 0, label: 'Disabled', desc: 'No voice announcements' },
		{ value: 1, label: 'Band Only', desc: '"Ka", "K", "Laser"' },
		{ value: 2, label: 'Frequency Only', desc: '"34.712"' },
		{ value: 3, label: 'Band + Frequency', desc: '"Ka 34.712"' }
	];
	
	onMount(async () => {
		await fetchSettings();
	});
	
	async function fetchSettings() {
		loading = true;
		try {
			const res = await fetch('/api/displaycolors');
			if (res.ok) {
				const data = await res.json();
				// Support both old and new API format
				if (data.voiceAlertMode !== undefined) {
					settings.voiceAlertMode = data.voiceAlertMode;
				} else if (data.voiceAlertsEnabled !== undefined) {
					// Migrate old setting
					settings.voiceAlertMode = data.voiceAlertsEnabled ? 3 : 0;
				}
				settings.voiceDirectionEnabled = data.voiceDirectionEnabled ?? true;
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
			params.append('voiceAlertMode', settings.voiceAlertMode);
			params.append('voiceDirectionEnabled', settings.voiceDirectionEnabled);
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
	
	// Build preview text based on current settings
	function getPreviewText() {
		if (settings.voiceAlertMode === 0) return '(silent)';
		let parts = [];
		if (settings.voiceAlertMode === 1) parts.push('Ka');
		else if (settings.voiceAlertMode === 2) parts.push('34.712');
		else if (settings.voiceAlertMode === 3) parts.push('Ka 34.712');
		if (settings.voiceDirectionEnabled && settings.voiceAlertMode > 0) parts.push('ahead');
		return `"${parts.join(' ')}"`;
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
				<p class="text-xs text-base-content/50 mb-4">Speak alert information through the built-in speaker when no phone app is connected</p>
				
				<div class="space-y-4">
					<!-- Voice Content Mode Dropdown -->
					<div class="form-control">
						<label class="label" for="voice-mode">
							<span class="label-text font-medium">Voice Content</span>
						</label>
						<select 
							id="voice-mode"
							class="select select-bordered w-full"
							bind:value={settings.voiceAlertMode}
						>
							{#each voiceModes as mode}
								<option value={mode.value}>{mode.label} - {mode.desc}</option>
							{/each}
						</select>
					</div>
					
					<!-- Direction Toggle -->
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Include Direction</span>
								<p class="text-xs text-base-content/50">Append "ahead", "side", or "behind" to announcement</p>
							</div>
							<input 
								type="checkbox" 
								class="toggle toggle-primary" 
								bind:checked={settings.voiceDirectionEnabled}
								disabled={settings.voiceAlertMode === 0}
							/>
						</label>
					</div>
					
					<!-- Preview -->
					<div class="bg-base-300 rounded-lg p-3">
						<p class="text-xs text-base-content/50 mb-1">Preview:</p>
						<p class="text-lg font-mono">{getPreviewText()}</p>
					</div>
					
					<div class="divider my-2"></div>
					
					<!-- Mute at Vol 0 -->
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
								disabled={settings.voiceAlertMode === 0}
							/>
						</label>
					</div>
					
					<div class="divider my-2"></div>
					
					<!-- Speaker Volume -->
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
					<li>New alert: full announcement based on your content settings</li>
					<li>Direction change: direction-only announcement (e.g., "behind") if direction is enabled</li>
					<li>5-second cooldown between announcements to prevent spam</li>
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
