<script>
	import { onMount } from 'svelte';
	import { fetchWithTimeout } from '$lib/utils/poll';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	
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
		alertVolumeFadeVolume: 1,
		// Speed-based volume settings
		speedVolumeEnabled: false,
		speedVolumeThresholdMph: 45,
		speedVolumeBoost: 2,
		// Low-speed quiet settings
		lowSpeedMuteEnabled: false,
		lowSpeedMuteThresholdMph: 5,
		lowSpeedVolume: 0
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
			const res = await fetchWithTimeout('/api/displaycolors');
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
				// Speed-based volume settings
				settings.speedVolumeEnabled = data.speedVolumeEnabled ?? false;
				settings.speedVolumeThresholdMph = data.speedVolumeThresholdMph ?? 45;
				settings.speedVolumeBoost = data.speedVolumeBoost ?? 2;
				// Low-speed quiet settings
				settings.lowSpeedMuteEnabled = data.lowSpeedMuteEnabled ?? false;
				settings.lowSpeedMuteThresholdMph = data.lowSpeedMuteThresholdMph ?? 5;
				settings.lowSpeedVolume = data.lowSpeedVolume ?? 0;
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
			// Speed-based volume settings
			params.append('speedVolumeEnabled', settings.speedVolumeEnabled);
			params.append('speedVolumeThresholdMph', settings.speedVolumeThresholdMph);
			params.append('speedVolumeBoost', settings.speedVolumeBoost);
			// Low-speed quiet settings
			params.append('lowSpeedMuteEnabled', settings.lowSpeedMuteEnabled);
			params.append('lowSpeedMuteThresholdMph', settings.lowSpeedMuteThresholdMph);
			params.append('lowSpeedVolume', settings.lowSpeedVolume);
			
			const res = await fetchWithTimeout('/api/displaycolors', {
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

<div class="page-stack">
	<PageHeader title="Audio Settings" subtitle="Voice alerts and speaker options" />
	
	<StatusAlert {message} fallbackType="success" />
	
	{#if loading}
		<div class="state-loading">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<!-- Voice Alerts -->
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Voice Alerts"
					subtitle="Speak alert information through the built-in speaker when no phone app is connected."
				/>
				
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
								<p class="copy-caption-soft">Append "ahead", "side", or "behind" to announcement</p>
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
								<p class="copy-caption-soft">Append "2 bogeys", "3 bogeys", etc. when multiple alerts active</p>
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
					<div class="surface-panel">
						<p class="copy-caption-soft mb-1">Preview:</p>
						<p class="text-lg font-mono">{getPreviewText()}</p>
					</div>
					
					<div class="divider my-2"></div>
					
					<!-- Mute at Vol 0 -->
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Mute Voice at Volume 0</span>
								<p class="copy-caption-soft">Silence alert announcements when V1 volume is 0</p>
								<p class="copy-warning mt-1">Note: "Warning Volume Zero" will still play</p>
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
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Secondary Alert Announcements"
					subtitle="Optionally announce non-priority alerts (lower bars) after priority stabilizes."
				/>
				
				<div class="space-y-4">
					<!-- Master Toggle -->
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Announce Secondary Alerts</span>
								<p class="copy-caption-soft">Speak non-priority alerts once after 1s priority stability + 1.5s gap</p>
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
						<div class="surface-subsection tight">
							<p class="copy-caption-soft mb-2">Which bands to announce:</p>
							
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
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="V1 Volume Fade"
					subtitle="Reduce V1 volume after initial alert period (doesn't affect muted alerts)."
				/>
				
				<div class="space-y-4">
					<!-- Master Toggle -->
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Enable Volume Fade</span>
								<p class="copy-caption-soft">Lower V1 volume after delay, restore when alert clears</p>
							</div>
							<input 
								type="checkbox" 
								class="toggle toggle-primary" 
								bind:checked={settings.alertVolumeFadeEnabled}
							/>
						</label>
					</div>
					
					{#if settings.alertVolumeFadeEnabled}
						<div class="surface-subsection">
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
								<p class="copy-caption-soft mt-1">Time at full volume before reducing</p>
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
								<p class="copy-caption-soft mt-1">V1 volume to fade to (0-9)</p>
							</div>
							
							<!-- Preview -->
							<div class="surface-panel text-sm">
								<p class="copy-subtle">
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
		
		<!-- Speed Volume -->
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Speed Volume"
					subtitle="Adjust V1 and speaker volume based on driving speed (requires a speed source)."
				/>
				
				<div class="space-y-4">
					<!-- Master Toggle -->
					<div class="form-control">
						<label class="label cursor-pointer">
							<div>
								<span class="label-text font-medium">Enable Speed Volume</span>
								<p class="copy-caption-soft">Automatically adjust volume based on speed</p>
							</div>
							<input 
								type="checkbox" 
								class="toggle toggle-primary" 
								bind:checked={settings.speedVolumeEnabled}
							/>
						</label>
					</div>
					
					{#if settings.speedVolumeEnabled}
						<!-- High-Speed Boost -->
						<div class="surface-subsection">
							<p class="text-sm font-semibold mb-2 copy-heading">High-Speed Boost</p>
							
							<!-- Speed Threshold -->
							<div class="form-control">
								<label class="label" for="speed-threshold">
									<span class="label-text">Speed Threshold</span>
									<span class="label-text-alt">{settings.speedVolumeThresholdMph} mph</span>
								</label>
								<input 
									id="speed-threshold"
									type="range" 
									min="20" 
									max="80" 
									step="5"
									bind:value={settings.speedVolumeThresholdMph}
									class="range range-primary range-sm" 
								/>
								<p class="copy-caption-soft mt-1">Boost volume when above this speed</p>
							</div>
							
							<!-- Volume Boost -->
							<div class="form-control">
								<label class="label" for="speed-boost">
									<span class="label-text">Volume Boost</span>
									<span class="label-text-alt">+{settings.speedVolumeBoost} levels</span>
								</label>
								<input 
									id="speed-boost"
									type="range" 
									min="1" 
									max="5" 
									bind:value={settings.speedVolumeBoost}
									class="range range-primary range-sm" 
								/>
								<p class="copy-caption-soft mt-1">V1 volume levels to add (1-5)</p>
							</div>
							
							<!-- Preview -->
							<div class="surface-panel text-sm">
								<p class="copy-subtle">
									Speed &gt; {settings.speedVolumeThresholdMph} mph → 
									<strong>+{settings.speedVolumeBoost} volume</strong> (max 9)
								</p>
							</div>
						</div>
						
						<!-- Low-Speed Quiet -->
						<div class="surface-subsection">
							<div class="form-control">
								<label class="label cursor-pointer">
									<div>
										<span class="label-text font-semibold">Low-Speed Quiet</span>
										<p class="copy-caption-soft">Reduce V1 and speaker volume at low speeds (parking lots, drive-thrus)</p>
									</div>
									<input 
										type="checkbox" 
										class="toggle toggle-primary toggle-sm" 
										bind:checked={settings.lowSpeedMuteEnabled}
									/>
								</label>
							</div>
							
							{#if settings.lowSpeedMuteEnabled}
								<!-- Speed Threshold -->
								<div class="form-control mt-2">
									<label class="label" for="low-speed-threshold">
										<span class="label-text">Speed Threshold</span>
										<span class="label-text-alt">{settings.lowSpeedMuteThresholdMph} mph</span>
									</label>
									<input 
										id="low-speed-threshold"
										type="range" 
										min="1" 
										max="20" 
										step="1"
										bind:value={settings.lowSpeedMuteThresholdMph}
										class="range range-primary range-sm" 
									/>
									<p class="copy-caption-soft mt-1">Reduce volume when below this speed</p>
								</div>
								
								<!-- Reduced Volume Level -->
								<div class="form-control">
									<label class="label" for="low-speed-volume">
										<span class="label-text">Reduced Volume</span>
										<span class="label-text-alt">{settings.lowSpeedVolume === 0 ? 'Mute' : settings.lowSpeedVolume}</span>
									</label>
									<input 
										id="low-speed-volume"
										type="range" 
										min="0" 
										max="9" 
										step="1"
										bind:value={settings.lowSpeedVolume}
										class="range range-primary range-sm" 
									/>
									<p class="copy-caption-soft mt-1">V1 and speaker volume level (0 = mute)</p>
								</div>
								
								<!-- Preview -->
								<div class="surface-panel text-sm">
									<p class="copy-subtle">
										Speed &lt; {settings.lowSpeedMuteThresholdMph} mph → 
										{#if settings.lowSpeedVolume === 0}
											<strong>muted</strong> (V1 + speaker silent)
										{:else}
											<strong>V1 vol = {settings.lowSpeedVolume}</strong>, speaker scaled
										{/if}
									</p>
								</div>
								
								<div class="copy-micro">
									Note: Requires a speed source. If unavailable, volume stays unchanged.
								</div>
							{/if}
						</div>
					{/if}
				</div>
			</div>
		</div>
		
		<!-- Speaker Volume -->
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead
					title="Speaker Volume"
					subtitle="Tune Waveshare ES8311 output level for local voice playback."
				/>
				
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
					<p class="copy-caption-soft mt-1">Controls the Waveshare ES8311 DAC output level</p>
				</div>
			</div>
		</div>
		
		<!-- Info Card -->
		<div class="surface-card">
			<div class="card-body">
				<CardSectionHead title="How It Works" />
				<ul class="copy-subtle space-y-2 list-disc list-inside">
					<li>Voice alerts only play when <strong>no phone app</strong> is connected via BLE proxy</li>
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
