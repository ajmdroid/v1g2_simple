<script>
	import { onMount } from 'svelte';
	
	let settings = $state({
		voiceAlertMode: 3,  // 0=disabled, 1=band, 2=freq, 3=band+freq
		voiceDirectionEnabled: true,
		announceBogeyCount: true,
		muteVoiceIfVolZero: false,
		voiceVolume: 75,  // Speaker volume (0-100)
		// Secondary alert settings
		announceSecondaryAlerts: false,
		secondaryLaser: true,
		secondaryKa: true,
		secondaryK: false,
		secondaryX: false,
		// Volume fade settings
		alertVolumeFadeEnabled: false,
		alertVolumeFadeDelaySec: 2,
		alertVolumeFadeVolume: 1
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
				settings.announceBogeyCount = data.announceBogeyCount ?? true;
				settings.muteVoiceIfVolZero = data.muteVoiceIfVolZero ?? false;
				settings.voiceVolume = data.voiceVolume ?? 75;
				// Secondary alert settings
				settings.announceSecondaryAlerts = data.announceSecondaryAlerts ?? false;
				settings.secondaryLaser = data.secondaryLaser ?? true;
				settings.secondaryKa = data.secondaryKa ?? true;
				settings.secondaryK = data.secondaryK ?? false;
				settings.secondaryX = data.secondaryX ?? false;
				// Volume fade settings
				settings.alertVolumeFadeEnabled = data.alertVolumeFadeEnabled ?? false;
				settings.alertVolumeFadeDelaySec = data.alertVolumeFadeDelaySec ?? 2;
				settings.alertVolumeFadeVolume = data.alertVolumeFadeVolume ?? 1;
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
			params.append('announceBogeyCount', settings.announceBogeyCount);
			params.append('muteVoiceIfVolZero', settings.muteVoiceIfVolZero);
			params.append('voiceVolume', settings.voiceVolume);
			// Secondary alert settings
			params.append('announceSecondaryAlerts', settings.announceSecondaryAlerts);
			params.append('secondaryLaser', settings.secondaryLaser);
			params.append('secondaryKa', settings.secondaryKa);
			params.append('secondaryK', settings.secondaryK);
			params.append('secondaryX', settings.secondaryX);
			// Volume fade settings
			params.append('alertVolumeFadeEnabled', settings.alertVolumeFadeEnabled);
			params.append('alertVolumeFadeDelaySec', settings.alertVolumeFadeDelaySec);
			params.append('alertVolumeFadeVolume', settings.alertVolumeFadeVolume);
			
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
		if (settings.announceBogeyCount && settings.voiceAlertMode > 0) parts.push('2 bogeys');
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
					
					<!-- Bogey Count Toggle -->
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Announce Bogey Count</span>
								<p class="text-xs text-base-content/50">Append "2 bogeys", "3 bogeys", etc. when multiple alerts active</p>
							</div>
							<input 
								type="checkbox" 
								class="toggle toggle-primary" 
								bind:checked={settings.announceBogeyCount}
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
				</div>
			</div>
		</div>
		
		<!-- Secondary Alerts -->
		<div class="card bg-base-200">
			<div class="card-body p-4">
				<h2 class="card-title text-lg">📢 Secondary Alert Announcements</h2>
				<p class="text-xs text-base-content/50 mb-4">Optionally announce non-priority alerts (lower bars) after priority stabilizes</p>
				
				<div class="space-y-4">
					<!-- Master Toggle -->
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Announce Secondary Alerts</span>
								<p class="text-xs text-base-content/50">Speak non-priority alerts once after 1s priority stability + 1.5s gap</p>
							</div>
							<input 
								type="checkbox" 
								class="toggle toggle-primary" 
								bind:checked={settings.announceSecondaryAlerts}
								disabled={settings.voiceAlertMode === 0}
							/>
						</label>
					</div>
					
					<!-- Band Filters (nested, only shown when master enabled) -->
					{#if settings.announceSecondaryAlerts && settings.voiceAlertMode !== 0}
						<div class="ml-4 pl-4 border-l-2 border-base-300 space-y-2">
							<p class="text-xs text-base-content/50 mb-2">Which bands to announce:</p>
							
							<div class="form-control">
								<label class="label cursor-pointer py-1">
									<span class="label-text">Laser</span>
									<input 
										type="checkbox" 
										class="toggle toggle-sm toggle-primary" 
										bind:checked={settings.secondaryLaser}
									/>
								</label>
							</div>
							
							<div class="form-control">
								<label class="label cursor-pointer py-1">
									<span class="label-text">Ka Band</span>
									<input 
										type="checkbox" 
										class="toggle toggle-sm toggle-primary" 
										bind:checked={settings.secondaryKa}
									/>
								</label>
							</div>
							
							<div class="form-control">
								<label class="label cursor-pointer py-1">
									<span class="label-text">K Band</span>
									<input 
										type="checkbox" 
										class="toggle toggle-sm toggle-primary" 
										bind:checked={settings.secondaryK}
									/>
								</label>
							</div>
							
							<div class="form-control">
								<label class="label cursor-pointer py-1">
									<span class="label-text">X Band</span>
									<input 
										type="checkbox" 
										class="toggle toggle-sm toggle-primary" 
										bind:checked={settings.secondaryX}
									/>
								</label>
							</div>
						</div>
					{/if}
				</div>
			</div>
		</div>
		
		<!-- Volume Fade -->
		<div class="card bg-base-200">
			<div class="card-body p-4">
				<h2 class="card-title text-lg">📉 V1 Volume Fade</h2>
				<p class="text-xs text-base-content/50 mb-4">Reduce V1 volume after initial alert period (doesn't affect muted alerts)</p>
				
				<div class="space-y-4">
					<!-- Master Toggle -->
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Enable Volume Fade</span>
								<p class="text-xs text-base-content/50">Lower V1 volume after delay, restore when alert clears</p>
							</div>
							<input 
								type="checkbox" 
								class="toggle toggle-primary" 
								bind:checked={settings.alertVolumeFadeEnabled}
							/>
						</label>
					</div>
					
					{#if settings.alertVolumeFadeEnabled}
						<div class="ml-4 pl-4 border-l-2 border-base-300 space-y-4">
							<!-- Delay -->
							<div class="form-control">
								<label class="label" for="fade-delay">
									<span class="label-text">Delay (seconds)</span>
									<span class="label-text-alt">{settings.alertVolumeFadeDelaySec}s</span>
								</label>
								<input 
									id="fade-delay"
									type="range" 
									min="1" 
									max="10" 
									bind:value={settings.alertVolumeFadeDelaySec}
									class="range range-primary range-sm" 
								/>
								<p class="text-xs text-base-content/50 mt-1">Time at full volume before reducing</p>
							</div>
							
							<!-- Reduced Volume -->
							<div class="form-control">
								<label class="label" for="fade-volume">
									<span class="label-text">Reduced Volume</span>
									<span class="label-text-alt">Level {settings.alertVolumeFadeVolume}</span>
								</label>
								<input 
									id="fade-volume"
									type="range" 
									min="0" 
									max="9" 
									bind:value={settings.alertVolumeFadeVolume}
									class="range range-primary range-sm" 
								/>
								<p class="text-xs text-base-content/50 mt-1">V1 volume to fade to (0-9)</p>
							</div>
							
							<!-- Preview -->
							<div class="bg-base-300 rounded-lg p-3 text-sm">
								<p class="text-base-content/70">
									Alert starts → <strong>full volume</strong> for {settings.alertVolumeFadeDelaySec}s → 
									fade to <strong>level {settings.alertVolumeFadeVolume}</strong> → 
									alert clears → <strong>restore volume</strong>
								</p>
							</div>
						</div>
					{/if}
				</div>
			</div>
		</div>
		
		<!-- Speaker Volume -->
		<div class="card bg-base-200">
			<div class="card-body p-4">
				<h2 class="card-title text-lg">🔈 Speaker Volume</h2>
				
				<div class="form-control">
					<label class="label" for="voice-volume-slider">
						<span class="label-text font-medium">Volume Level</span>
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
