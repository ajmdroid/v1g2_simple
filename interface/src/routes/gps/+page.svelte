<script>
	import { onMount } from 'svelte';
	
	let settings = $state({
		gpsEnabled: false,
		obdEnabled: false,
		// Auto-lockout settings (JBV1-style)
		lockoutEnabled: true,
		lockoutKaProtection: true,
		lockoutDirectionalUnlearn: true,
		lockoutFreqToleranceMHz: 8,
		lockoutLearnCount: 3,
		lockoutUnlearnCount: 5,
		lockoutManualDeleteCount: 25,
		lockoutLearnIntervalHours: 4,
		lockoutUnlearnIntervalHours: 4,
		lockoutMaxSignalStrength: 0,  // 0 = disabled/None
		lockoutMaxDistanceM: 600
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
			const params = new URLSearchParams();
			params.append('gpsEnabled', settings.gpsEnabled);
			params.append('obdEnabled', settings.obdEnabled);
			params.append('lockoutEnabled', settings.lockoutEnabled);
			params.append('lockoutKaProtection', settings.lockoutKaProtection);
			params.append('lockoutDirectionalUnlearn', settings.lockoutDirectionalUnlearn);
			params.append('lockoutFreqToleranceMHz', settings.lockoutFreqToleranceMHz);
			params.append('lockoutLearnCount', settings.lockoutLearnCount);
			params.append('lockoutUnlearnCount', settings.lockoutUnlearnCount);
			params.append('lockoutManualDeleteCount', settings.lockoutManualDeleteCount);
			params.append('lockoutLearnIntervalHours', settings.lockoutLearnIntervalHours);
			params.append('lockoutUnlearnIntervalHours', settings.lockoutUnlearnIntervalHours);
			params.append('lockoutMaxSignalStrength', settings.lockoutMaxSignalStrength);
			params.append('lockoutMaxDistanceM', settings.lockoutMaxDistanceM);
			
			const res = await fetch('/settings', {
				method: 'POST',
				headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
				body: params
			});
			
			if (res.ok) {
				message = { type: 'success', text: 'GPS & Lockout settings saved!' };
			} else {
				message = { type: 'error', text: 'Failed to save settings' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		} finally {
			saving = false;
		}
	}
	
	function resetToDefaults() {
		if (!confirm('Reset all lockout settings to JBV1 defaults?')) return;
		
		settings.lockoutEnabled = true;
		settings.lockoutKaProtection = true;
		settings.lockoutDirectionalUnlearn = true;
		settings.lockoutFreqToleranceMHz = 8;
		settings.lockoutLearnCount = 3;
		settings.lockoutUnlearnCount = 5;
		settings.lockoutManualDeleteCount = 25;
		settings.lockoutLearnIntervalHours = 4;
		settings.lockoutUnlearnIntervalHours = 4;
		settings.lockoutMaxSignalStrength = 0;
		settings.lockoutMaxDistanceM = 600;
	}
</script>

<div class="space-y-6">
	<h1 class="text-2xl font-bold">📍 GPS & Lockouts</h1>
	
	{#if message}
		<div class="alert alert-{message.type === 'error' ? 'error' : 'success'}" role="status">
			<span>{message.text}</span>
		</div>
	{/if}
	
	{#if loading}
		<div class="flex justify-center p-8">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<!-- Hardware Modules -->
		<div class="card bg-base-200">
			<div class="card-body space-y-4">
				<h2 class="card-title">🔧 Hardware Modules</h2>
				<p class="text-sm text-base-content/60">Enable optional GPS and OBD-II modules.</p>
				
				<label class="label cursor-pointer">
					<div class="flex flex-col">
						<span class="label-text">GPS Module</span>
						<span class="label-text-alt text-base-content/50">Required for lockouts and speed display</span>
					</div>
					<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.gpsEnabled} />
				</label>
				
				<label class="label cursor-pointer">
					<div class="flex flex-col">
						<span class="label-text">OBD-II Module</span>
						<span class="label-text-alt text-base-content/50">Vehicle speed from car's computer (Bluetooth ELM327)</span>
					</div>
					<input type="checkbox" class="toggle toggle-primary" bind:checked={settings.obdEnabled} />
				</label>
				
				<div class="text-xs text-base-content/40 mt-2">
					💡 Modules auto-disable after 60 seconds if not detected.
				</div>
			</div>
		</div>

		<!-- Auto-Lockout Master -->
		<div class="card bg-base-200">
			<div class="card-body space-y-4">
				<h2 class="card-title">🔒 Auto-Lockout System</h2>
				<p class="text-sm text-base-content/60">Automatically learn and mute false alerts at specific locations.</p>
				
				<label class="label cursor-pointer">
					<div class="flex flex-col">
						<span class="label-text font-semibold">Enable Auto-Lockouts</span>
						<span class="label-text-alt text-base-content/50">Learn false alerts and auto-mute in the future</span>
					</div>
					<input type="checkbox" class="toggle toggle-success" bind:checked={settings.lockoutEnabled} />
				</label>
				
				{#if !settings.gpsEnabled && settings.lockoutEnabled}
					<div class="alert alert-warning text-sm">
						<svg xmlns="http://www.w3.org/2000/svg" class="stroke-current shrink-0 h-5 w-5" fill="none" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z" /></svg>
						<span>GPS module must be enabled for lockouts to work!</span>
					</div>
				{/if}
			</div>
		</div>

		{#if settings.lockoutEnabled}
			<!-- Learning Settings -->
			<div class="card bg-base-200">
				<div class="card-body space-y-4">
					<h2 class="card-title">📚 Learning Settings</h2>
					<p class="text-sm text-base-content/60">How alerts are learned and promoted to lockouts.</p>
					
					<div class="form-control">
						<label class="label" for="learn-count">
							<span class="label-text">Learn Count</span>
							<span class="label-text-alt">Hits to create lockout</span>
						</label>
						<input 
							id="learn-count"
							type="number" 
							class="input input-bordered w-24"
							bind:value={settings.lockoutLearnCount}
							min="1"
							max="10"
						/>
						<label class="label">
							<span class="label-text-alt text-base-content/50">JBV1 default: 3</span>
						</label>
					</div>
					
					<div class="form-control">
						<label class="label" for="learn-interval">
							<span class="label-text">Learn Interval (hours)</span>
							<span class="label-text-alt">Time between counted hits</span>
						</label>
						<input 
							id="learn-interval"
							type="number" 
							class="input input-bordered w-24"
							bind:value={settings.lockoutLearnIntervalHours}
							min="0"
							max="24"
						/>
						<label class="label">
							<span class="label-text-alt text-base-content/50">JBV1 default: 4 hours (0 = no interval)</span>
						</label>
					</div>
					
					<div class="form-control">
						<label class="label" for="freq-tolerance">
							<span class="label-text">Frequency Tolerance (MHz)</span>
							<span class="label-text-alt">Match range for same signal</span>
						</label>
						<input 
							id="freq-tolerance"
							type="number" 
							class="input input-bordered w-24"
							bind:value={settings.lockoutFreqToleranceMHz}
							min="1"
							max="50"
						/>
						<label class="label">
							<span class="label-text-alt text-base-content/50">JBV1 default: 8 MHz (radar guns drift with temperature)</span>
						</label>
					</div>
					
					<div class="form-control">
						<label class="label" for="max-distance">
							<span class="label-text">Max Alert Distance (meters)</span>
							<span class="label-text-alt">Don't learn far signals</span>
						</label>
						<input 
							id="max-distance"
							type="number" 
							class="input input-bordered w-24"
							bind:value={settings.lockoutMaxDistanceM}
							min="100"
							max="2000"
							step="100"
						/>
						<label class="label">
							<span class="label-text-alt text-base-content/50">JBV1 default: 600m (~3/8 mi)</span>
						</label>
					</div>
					
					<div class="form-control">
						<label class="label" for="max-signal">
							<span class="label-text">Max Signal Strength</span>
							<span class="label-text-alt">Don't learn strong signals</span>
						</label>
						<select 
							id="max-signal"
							class="select select-bordered w-32"
							bind:value={settings.lockoutMaxSignalStrength}
						>
							<option value={0}>None (learn all)</option>
							<option value={3}>3 bars</option>
							<option value={4}>4 bars</option>
							<option value={5}>5 bars</option>
							<option value={6}>6 bars</option>
							<option value={7}>7 bars</option>
							<option value={8}>8 bars</option>
						</select>
						<label class="label">
							<span class="label-text-alt text-base-content/50">JBV1 default: None (strong signals may be real threats)</span>
						</label>
					</div>
				</div>
			</div>

			<!-- Unlearning Settings -->
			<div class="card bg-base-200">
				<div class="card-body space-y-4">
					<h2 class="card-title">🗑️ Unlearning Settings</h2>
					<p class="text-sm text-base-content/60">How lockouts are removed when no longer needed.</p>
					
					<div class="form-control">
						<label class="label" for="unlearn-count">
							<span class="label-text">Auto-Lockout Delete Count</span>
							<span class="label-text-alt">Misses to remove auto-learned</span>
						</label>
						<input 
							id="unlearn-count"
							type="number" 
							class="input input-bordered w-24"
							bind:value={settings.lockoutUnlearnCount}
							min="1"
							max="50"
						/>
						<label class="label">
							<span class="label-text-alt text-base-content/50">JBV1 default: 5</span>
						</label>
					</div>
					
					<div class="form-control">
						<label class="label" for="manual-delete-count">
							<span class="label-text">Manual Lockout Delete Count</span>
							<span class="label-text-alt">Misses to remove user-created</span>
						</label>
						<input 
							id="manual-delete-count"
							type="number" 
							class="input input-bordered w-24"
							bind:value={settings.lockoutManualDeleteCount}
							min="1"
							max="100"
						/>
						<label class="label">
							<span class="label-text-alt text-base-content/50">JBV1 default: 25 (user lockouts are more trusted)</span>
						</label>
					</div>
					
					<div class="form-control">
						<label class="label" for="unlearn-interval">
							<span class="label-text">Unlearn Interval (hours)</span>
							<span class="label-text-alt">Time between counted misses</span>
						</label>
						<input 
							id="unlearn-interval"
							type="number" 
							class="input input-bordered w-24"
							bind:value={settings.lockoutUnlearnIntervalHours}
							min="0"
							max="24"
						/>
						<label class="label">
							<span class="label-text-alt text-base-content/50">JBV1 default: 4 hours (prevents multiple misses per trip)</span>
						</label>
					</div>
					
					<label class="label cursor-pointer">
						<div class="flex flex-col">
							<span class="label-text">Directional Unlearn</span>
							<span class="label-text-alt text-base-content/50">Only unlearn when traveling same direction as when learned</span>
						</div>
						<input type="checkbox" class="toggle" bind:checked={settings.lockoutDirectionalUnlearn} />
					</label>
				</div>
			</div>

			<!-- Protection Settings -->
			<div class="card bg-base-200">
				<div class="card-body space-y-4">
					<h2 class="card-title">🛡️ Protection Settings</h2>
					<p class="text-sm text-base-content/60">Safeguards to prevent learning real threats.</p>
					
					<label class="label cursor-pointer">
						<div class="flex flex-col">
							<span class="label-text">Ka Band Protection</span>
							<span class="label-text-alt text-base-content/50">Never auto-learn Ka alerts (almost always real police radar)</span>
						</div>
						<input type="checkbox" class="toggle toggle-warning" bind:checked={settings.lockoutKaProtection} />
					</label>
					
					{#if !settings.lockoutKaProtection}
						<div class="alert alert-error text-sm">
							<svg xmlns="http://www.w3.org/2000/svg" class="stroke-current shrink-0 h-5 w-5" fill="none" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z" /></svg>
							<span><strong>Warning:</strong> Ka band is rarely used for false sources. Disabling this protection may cause you to miss real threats!</span>
						</div>
					{/if}
				</div>
			</div>
		{/if}

		<!-- Action Buttons -->
		<div class="flex gap-2">
			<button 
				class="btn btn-primary flex-1" 
				onclick={saveSettings}
				disabled={saving}
			>
				{#if saving}
					<span class="loading loading-spinner loading-sm"></span>
				{/if}
				Save Settings
			</button>
			
			{#if settings.lockoutEnabled}
				<button class="btn btn-outline btn-sm" onclick={resetToDefaults}>
					Reset to JBV1 Defaults
				</button>
			{/if}
		</div>

		<!-- Help Section -->
		<div class="collapse collapse-arrow bg-base-200">
			<input type="checkbox" />
			<div class="collapse-title font-medium">
				❓ How Auto-Lockouts Work
			</div>
			<div class="collapse-content text-sm space-y-2">
				<p><strong>Learning:</strong> When you pass a false alert multiple times (at the same location, same frequency), it gets promoted to a lockout. The V1 will be muted automatically at that location in the future.</p>
				<p><strong>Unlearning:</strong> If you pass a lockout location and the signal is no longer there, it counts as a "miss". After enough misses, the lockout is automatically removed.</p>
				<p><strong>Intervals:</strong> The learn/unlearn intervals prevent the same signal from being counted multiple times in one trip. With a 4-hour interval, you need to pass on separate occasions.</p>
				<p><strong>Directional:</strong> Lockouts are learned in a specific direction. If you approach from the opposite direction, misses won't count against unlearning.</p>
				<p><strong>Ka Protection:</strong> Ka band is almost exclusively used by police. False Ka sources are extremely rare, so auto-learning Ka is disabled by default.</p>
			</div>
		</div>
	{/if}
</div>
