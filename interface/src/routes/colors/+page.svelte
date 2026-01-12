<script>
	import { onMount } from 'svelte';
	
	// RGB565 color values (stored on ESP32)
	let colors = $state({
		bogey: 0xF800,   // Red
		freq: 0xF800,    // Red
		freqUseBandColor: false,  // Use band color for frequency
		arrowFront: 0xF800,  // Red (front)
		arrowSide: 0xF800,   // Red (side)
		arrowRear: 0xF800,   // Red (rear)
		bandL: 0x001F,   // Blue
		bandKa: 0xF800,  // Red
		bandK: 0x001F,   // Blue
		bandX: 0x07E0,   // Green
		wifiIcon: 0x07FF, // Cyan
		bleConnected: 0x07E0,    // Green
		bleDisconnected: 0x001F, // Blue
		bar1: 0x07E0,    // Green (weakest)
		bar2: 0x07E0,    // Green
		bar3: 0xFFE0,    // Yellow
		bar4: 0xFFE0,    // Yellow
		bar5: 0xF800,    // Red
		bar6: 0xF800,    // Red (strongest)
		muted: 0x3186,   // Dark grey (muted alerts)
		persisted: 0x18C3, // Darker grey (persisted alerts)
		volumeMain: 0x001F, // Blue (main volume)
		volumeMute: 0xFFE0, // Yellow (mute volume)
		hideWifiIcon: false,
		hideProfileIndicator: false,
		hideBatteryIcon: false,
		hideBleIcon: false,
		hideVolumeIndicator: false,
		voiceAlertsEnabled: true
	});
	
	let displayStyle = $state(0);  // 0 = Classic, 1 = Modern
	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);
	
	onMount(async () => {
		await Promise.all([fetchColors(), fetchDisplayStyle()]);
	});
	
	async function fetchColors() {
		loading = true;
		try {
			const res = await fetch('/api/displaycolors');
			if (res.ok) {
				const data = await res.json();
				colors = data;
			}
		} catch (e) {
			message = { type: 'error', text: 'Failed to load colors' };
		} finally {
			loading = false;
		}
	}
	
	async function fetchDisplayStyle() {
		try {
			const res = await fetch('/api/settings');
			if (res.ok) {
				const data = await res.json();
				displayStyle = data.displayStyle || 0;
			}
		} catch (e) {
			// Ignore - will use default
		}
	}
	
	async function saveDisplayStyle(event) {
		const newStyle = parseInt(event.target.value);
		try {
			const formData = new FormData();
			formData.append('displayStyle', newStyle);
			const res = await fetch('/settings', {
				method: 'POST',
				body: formData
			});
			if (res.ok) {
				displayStyle = newStyle;  // Update local state after successful save
				message = { type: 'success', text: 'Display style updated!' };
			} else {
				message = { type: 'error', text: 'Failed to save display style' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Failed to save display style' };
		}
	}
	
	// Convert RGB565 to hex color for input
	function rgb565ToHex(rgb565) {
		const r = ((rgb565 >> 11) & 0x1F) << 3;
		const g = ((rgb565 >> 5) & 0x3F) << 2;
		const b = (rgb565 & 0x1F) << 3;
		return '#' + [r, g, b].map(x => x.toString(16).padStart(2, '0')).join('');
	}
	
	// Convert hex color to RGB565
	function hexToRgb565(hex) {
		const r = parseInt(hex.slice(1, 3), 16) >> 3;
		const g = parseInt(hex.slice(3, 5), 16) >> 2;
		const b = parseInt(hex.slice(5, 7), 16) >> 3;
		return (r << 11) | (g << 5) | b;
	}
	
	function updateColor(key, hexValue) {
		colors[key] = hexToRgb565(hexValue);
	}
	
	async function saveColors() {
		saving = true;
		message = null;
		
		try {
			const params = new URLSearchParams();
			params.append('bogey', colors.bogey);
			params.append('freq', colors.freq);
			params.append('freqUseBandColor', colors.freqUseBandColor);
			params.append('arrowFront', colors.arrowFront);
			params.append('arrowSide', colors.arrowSide);
			params.append('arrowRear', colors.arrowRear);
			params.append('bandL', colors.bandL);
			params.append('bandKa', colors.bandKa);
			params.append('bandK', colors.bandK);
			params.append('bandX', colors.bandX);
			params.append('wifiIcon', colors.wifiIcon);
			params.append('bleConnected', colors.bleConnected);
			params.append('bleDisconnected', colors.bleDisconnected);
			params.append('bar1', colors.bar1);
			params.append('bar2', colors.bar2);
			params.append('bar3', colors.bar3);
			params.append('bar4', colors.bar4);
			params.append('bar5', colors.bar5);
			params.append('bar6', colors.bar6);
			params.append('muted', colors.muted);
			params.append('persisted', colors.persisted);
			params.append('volumeMain', colors.volumeMain);
			params.append('volumeMute', colors.volumeMute);
			params.append('hideWifiIcon', colors.hideWifiIcon);
			params.append('hideProfileIndicator', colors.hideProfileIndicator);
			params.append('hideBatteryIcon', colors.hideBatteryIcon);
			params.append('hideBleIcon', colors.hideBleIcon);
			params.append('hideVolumeIndicator', colors.hideVolumeIndicator);
			params.append('voiceAlertsEnabled', colors.voiceAlertsEnabled);
			
			const res = await fetch('/api/displaycolors', {
				method: 'POST',
				headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
				body: params
			});
			
			if (res.ok) {
				message = { type: 'success', text: 'Colors saved! Previewing on display...' };
				// Clear preview after 3 seconds
				setTimeout(() => {
					fetch('/api/displaycolors/clear', { method: 'POST' })
						.catch(() => {}); // Ignore errors
				}, 3000);
			} else {
				message = { type: 'error', text: 'Failed to save colors' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Connection error' };
		} finally {
			saving = false;
		}
	}
	
	async function testColors() {
		try {
			await fetch('/api/displaycolors/preview', { method: 'POST' });
		} catch {
			// Silent fail for test
		}
	}
	
	async function resetDefaults() {
		if (!confirm('Reset all colors to defaults?')) return;
		
		try {
			const res = await fetch('/api/displaycolors/reset', { method: 'POST' });
			if (res.ok) {
				// Set default values
				colors = {
					bogey: 0xF800,
					freq: 0xF800,
					freqUseBandColor: false,
					arrowFront: 0xF800,
					arrowSide: 0xF800,
					arrowRear: 0xF800,
					bandL: 0x001F,
					bandKa: 0xF800,
					bandK: 0x001F,
					bandX: 0x07E0,
					wifiIcon: 0x07FF,
					bleConnected: 0x07E0,
					bleDisconnected: 0x001F,
					bar1: 0x07E0,
					bar2: 0x07E0,
					bar3: 0xFFE0,
					bar4: 0xFFE0,
					bar5: 0xF800,
					bar6: 0xF800,
					muted: 0x3186,
					persisted: 0x18C3,
					volumeMain: 0x001F,
					volumeMute: 0xFFE0,
					hideWifiIcon: false,
					hideProfileIndicator: false,
					hideBatteryIcon: false,
					hideBleIcon: false,
					hideVolumeIndicator: false,
					voiceAlertsEnabled: true
				};
				message = { type: 'success', text: 'Colors reset to defaults!' };
			}
		} catch (e) {
			message = { type: 'error', text: 'Failed to reset' };
		}
	}
</script>

<div class="space-y-6">
	<div class="flex justify-between items-center">
		<div>
			<h1 class="text-2xl font-bold">Display Colors</h1>
			<p class="text-sm text-base-content/60">Customize alert colors</p>
		</div>
	</div>
	
	{#if message}
		<div class="alert alert-{message.type === 'error' ? 'error' : 'success'}" role="status" aria-live="polite">
			<span>{message.text}</span>
			<button class="btn btn-ghost btn-xs" onclick={() => message = null} aria-label="Dismiss message">✕</button>
		</div>
	{/if}
	
	{#if loading}
		<div class="flex justify-center p-8">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<!-- Display Style -->
		<div class="card bg-base-200">
			<div class="card-body p-4">
				<h2 class="card-title text-lg">🖥️ Display Style</h2>
				<p class="text-sm text-base-content/60">Choose font style for frequency and counter</p>
				<div class="form-control">
					<select 
						id="display-style"
						class="select select-bordered"
						value={displayStyle}
						onchange={saveDisplayStyle}
					>
						<option value={0}>Classic (7-Segment)</option>
						<option value={1}>Modern</option>
					</select>
				</div>
			</div>
		</div>

		<!-- Counter & Frequency -->
		<div class="card bg-base-200">
			<div class="card-body p-4">
				<h2 class="card-title text-lg">Counter & Frequency</h2>
				<div class="grid grid-cols-2 gap-4">
					<div class="form-control">
						<label class="label" for="bogey-color">
							<span class="label-text">Bogey Counter</span>
						</label>
						<div class="flex items-center gap-3">
							<input 
								id="bogey-color"
								type="color" 
								aria-label="Bogey counter color"
								class="w-12 h-10 cursor-pointer rounded border-0"
								value={rgb565ToHex(colors.bogey)}
								onchange={(e) => updateColor('bogey', e.target.value)}
							/>
							<span 
								class="text-2xl font-bold font-mono"
								style="color: {rgb565ToHex(colors.bogey)}"
							>1.</span>
						</div>
					</div>
					<div class="form-control">
						<label class="label" for="freq-color">
							<span class="label-text">Frequency Display</span>
						</label>
						<div class="flex items-center gap-3">
							<input 
								id="freq-color"
								type="color" 
								aria-label="Frequency display color"
								class="w-12 h-10 cursor-pointer rounded border-0"
								value={rgb565ToHex(colors.freq)}
								onchange={(e) => updateColor('freq', e.target.value)}
								disabled={colors.freqUseBandColor}
							/>
							<span 
								class="text-2xl font-bold font-mono"
								style="color: {rgb565ToHex(colors.freq)}"
								class:opacity-50={colors.freqUseBandColor}
							>35.5</span>
						</div>
						<label class="label cursor-pointer justify-start gap-2 mt-1">
							<input 
								type="checkbox"
								class="toggle toggle-sm toggle-primary"
								bind:checked={colors.freqUseBandColor}
							/>
							<span class="label-text text-sm">Use band color for frequency</span>
						</label>
					</div>
				</div>
				<div class="divider my-2"></div>
				<div class="form-control">
					<label class="label" for="muted-color">
						<span class="label-text">Muted Alert Color</span>
						<span class="label-text-alt text-base-content/50">When alert is muted</span>
					</label>
					<div class="flex items-center gap-3">
						<input 
							id="muted-color"
							type="color" 
							aria-label="Muted alert color"
							class="w-12 h-10 cursor-pointer rounded border-0"
							value={rgb565ToHex(colors.muted)}
							onchange={(e) => updateColor('muted', e.target.value)}
						/>
						<span 
							class="text-2xl font-bold font-mono"
							style="color: {rgb565ToHex(colors.muted)}"
						>35.5</span>
						<span class="text-sm text-base-content/60">(muted)</span>
					</div>
				</div>
				<div class="divider my-2"></div>
				<div class="form-control">
					<label class="label" for="persisted-color">
						<span class="label-text">Persisted Alert Color</span>
						<span class="label-text-alt text-base-content/50">Ghost alert after V1 clears</span>
					</label>
					<div class="flex items-center gap-3">
						<input 
							id="persisted-color"
							type="color" 
							aria-label="Persisted alert color"
							class="w-12 h-10 cursor-pointer rounded border-0"
							value={rgb565ToHex(colors.persisted)}
							onchange={(e) => updateColor('persisted', e.target.value)}
						/>
						<span 
							class="text-2xl font-bold font-mono"
							style="color: {rgb565ToHex(colors.persisted)}"
						>35.5</span>
						<span class="text-sm text-base-content/60">(persisted)</span>
					</div>
				</div>
				<div class="divider my-2"></div>
				<h3 class="font-semibold text-sm mt-2">Volume Indicator</h3>
				<div class="grid grid-cols-2 gap-4">
					<div class="form-control">
						<label class="label" for="volumeMain-color">
							<span class="label-text">Main Volume</span>
						</label>
						<div class="flex items-center gap-2">
							<input 
								id="volumeMain-color"
								type="color" 
								aria-label="Main volume color"
								class="w-10 h-8 cursor-pointer rounded border-0"
								value={rgb565ToHex(colors.volumeMain)}
								onchange={(e) => updateColor('volumeMain', e.target.value)}
							/>
							<span 
								class="text-lg font-bold font-mono"
								style="color: {rgb565ToHex(colors.volumeMain)}"
							>5V</span>
						</div>
					</div>
					<div class="form-control">
						<label class="label" for="volumeMute-color">
							<span class="label-text">Mute Volume</span>
						</label>
						<div class="flex items-center gap-2">
							<input 
								id="volumeMute-color"
								type="color" 
								aria-label="Mute volume color"
								class="w-10 h-8 cursor-pointer rounded border-0"
								value={rgb565ToHex(colors.volumeMute)}
								onchange={(e) => updateColor('volumeMute', e.target.value)}
							/>
							<span 
								class="text-lg font-bold font-mono"
								style="color: {rgb565ToHex(colors.volumeMute)}"
							>0M</span>
						</div>
					</div>
				</div>
			</div>
		</div>
		
		<!-- Band Indicators -->
		<div class="card bg-base-200">
			<div class="card-body p-4">
				<h2 class="card-title text-lg">Band Indicators</h2>
				<div class="grid grid-cols-2 md:grid-cols-4 gap-4">
					<div class="form-control">
						<label class="label" for="bandL-color">
							<span class="label-text">Laser (L)</span>
						</label>
						<div class="flex items-center gap-3">
							<input 
								id="bandL-color"
								type="color" 
								aria-label="Laser band color"
								class="w-12 h-10 cursor-pointer rounded border-0"
								value={rgb565ToHex(colors.bandL)}
								onchange={(e) => updateColor('bandL', e.target.value)}
							/>
							<span 
								class="text-2xl font-bold"
								style="color: {rgb565ToHex(colors.bandL)}"
							>L</span>
						</div>
					</div>
					<div class="form-control">
						<label class="label" for="bandKa-color">
							<span class="label-text">Ka Band</span>
						</label>
						<div class="flex items-center gap-3">
							<input 
								id="bandKa-color"
								type="color" 
								aria-label="Ka band color"
								class="w-12 h-10 cursor-pointer rounded border-0"
								value={rgb565ToHex(colors.bandKa)}
								onchange={(e) => updateColor('bandKa', e.target.value)}
							/>
							<span 
								class="text-2xl font-bold"
								style="color: {rgb565ToHex(colors.bandKa)}"
							>Ka</span>
						</div>
					</div>
					<div class="form-control">
						<label class="label" for="bandK-color">
							<span class="label-text">K Band</span>
						</label>
						<div class="flex items-center gap-3">
							<input 
								id="bandK-color"
								type="color" 
								aria-label="K band color"
								class="w-12 h-10 cursor-pointer rounded border-0"
								value={rgb565ToHex(colors.bandK)}
								onchange={(e) => updateColor('bandK', e.target.value)}
							/>
							<span 
								class="text-2xl font-bold"
								style="color: {rgb565ToHex(colors.bandK)}"
							>K</span>
						</div>
					</div>
					<div class="form-control">
						<label class="label" for="bandX-color">
							<span class="label-text">X Band</span>
						</label>
						<div class="flex items-center gap-3">
							<input 
								id="bandX-color"
								type="color" 
								aria-label="X band color"
								class="w-12 h-10 cursor-pointer rounded border-0"
								value={rgb565ToHex(colors.bandX)}
								onchange={(e) => updateColor('bandX', e.target.value)}
							/>
							<span 
								class="text-2xl font-bold"
								style="color: {rgb565ToHex(colors.bandX)}"
							>X</span>
						</div>
					</div>
				</div>
			</div>
		</div>
		
		<!-- Direction Arrows -->
		<div class="card bg-base-200">
			<div class="card-body p-4">
				<h2 class="card-title text-lg">Direction Arrows</h2>
				<div class="grid grid-cols-3 gap-4">
					<div class="form-control">
						<label class="label" for="arrow-front-color">
							<span class="label-text">Front</span>
						</label>
						<div class="flex items-center gap-2">
							<input 
								id="arrow-front-color"
								type="color" 
								aria-label="Front arrow color"
								class="w-10 h-10 cursor-pointer rounded border-0"
								value={rgb565ToHex(colors.arrowFront)}
								onchange={(e) => updateColor('arrowFront', e.target.value)}
							/>
							<span 
								class="text-2xl font-bold"
								style="color: {rgb565ToHex(colors.arrowFront)}"
							>▲</span>
						</div>
					</div>
					<div class="form-control">
						<label class="label" for="arrow-side-color">
							<span class="label-text">Side</span>
						</label>
						<div class="flex items-center gap-2">
							<input 
								id="arrow-side-color"
								type="color" 
								aria-label="Side arrow color"
								class="w-10 h-10 cursor-pointer rounded border-0"
								value={rgb565ToHex(colors.arrowSide)}
								onchange={(e) => updateColor('arrowSide', e.target.value)}
							/>
							<span 
								class="text-2xl font-bold"
								style="color: {rgb565ToHex(colors.arrowSide)}"
							>◀▶</span>
						</div>
					</div>
					<div class="form-control">
						<label class="label" for="arrow-rear-color">
							<span class="label-text">Rear</span>
						</label>
						<div class="flex items-center gap-2">
							<input 
								id="arrow-rear-color"
								type="color" 
								aria-label="Rear arrow color"
								class="w-10 h-10 cursor-pointer rounded border-0"
								value={rgb565ToHex(colors.arrowRear)}
								onchange={(e) => updateColor('arrowRear', e.target.value)}
							/>
							<span 
								class="text-2xl font-bold"
								style="color: {rgb565ToHex(colors.arrowRear)}"
							>▼</span>
						</div>
					</div>
				</div>
			</div>
		</div>
		
		<!-- Status Indicators -->
		<div class="card bg-base-200">
			<div class="card-body p-4">
				<h2 class="card-title text-lg">Status Indicators</h2>
				<div class="form-control">
					<label class="label" for="wifiIcon-color">
						<span class="label-text">WiFi Icon</span>
					</label>
					<div class="flex items-center gap-3">
						<input 
							id="wifiIcon-color"
							type="color" 
							aria-label="WiFi icon color"
							class="w-12 h-10 cursor-pointer rounded border-0"
							value={rgb565ToHex(colors.wifiIcon)}
							onchange={(e) => updateColor('wifiIcon', e.target.value)}
						/>
						<span 
							class="text-2xl font-bold"
							style="color: {rgb565ToHex(colors.wifiIcon)}"
						>📶</span>
					</div>
				</div>
				
				<div class="grid grid-cols-2 gap-4">
					<div class="form-control">
						<label class="label" for="bleConnected-color">
							<span class="label-text">Proxy Connected</span>
						</label>
						<div class="flex items-center gap-3">
							<input 
								id="bleConnected-color"
								type="color" 
								aria-label="Bluetooth connected color"
								class="w-12 h-10 cursor-pointer rounded border-0"
								value={rgb565ToHex(colors.bleConnected)}
								onchange={(e) => updateColor('bleConnected', e.target.value)}
							/>
							<span 
								class="text-2xl font-bold"
								style="color: {rgb565ToHex(colors.bleConnected)}"
							>🔗</span>
						</div>
					</div>
					<div class="form-control">
						<label class="label" for="bleDisconnected-color">
							<span class="label-text">Proxy Ready</span>
						</label>
						<div class="flex items-center gap-3">
							<input 
								id="bleDisconnected-color"
								type="color" 
								aria-label="BLE proxy ready color"
								class="w-12 h-10 cursor-pointer rounded border-0"
								value={rgb565ToHex(colors.bleDisconnected)}
								onchange={(e) => updateColor('bleDisconnected', e.target.value)}
							/>
							<span 
								class="text-2xl font-bold"
								style="color: {rgb565ToHex(colors.bleDisconnected)}"
							>⛓️</span>
						</div>
					</div>
				</div>
				
				<div class="divider my-2"></div>
				
				<!-- Visibility Toggles -->
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text">Hide WiFi Icon</span>
							<p class="text-xs text-base-content/50">Show briefly on connect, then hide</p>
						</div>
						<input 
							type="checkbox" 
							class="toggle toggle-primary" 
							checked={colors.hideWifiIcon}
							onchange={(e) => colors.hideWifiIcon = e.target.checked}
						/>
					</label>
				</div>
				
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text">Hide Profile Indicator</span>
							<p class="text-xs text-base-content/50">Show on profile change, then hide</p>
						</div>
						<input 
							type="checkbox" 
							class="toggle toggle-primary" 
							checked={colors.hideProfileIndicator}
							onchange={(e) => colors.hideProfileIndicator = e.target.checked}
						/>
					</label>
				</div>
				
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text">Hide Battery Icon</span>
							<p class="text-xs text-base-content/50">Hide the battery indicator</p>
						</div>
						<input 
							type="checkbox" 
							class="toggle toggle-primary" 
							checked={colors.hideBatteryIcon}
							onchange={(e) => colors.hideBatteryIcon = e.target.checked}
						/>
					</label>
				</div>
				
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text">Hide BLE Proxy Icon</span>
							<p class="text-xs text-base-content/50">Hide the JBV1 proxy status indicator</p>
						</div>
						<input 
							type="checkbox" 
							class="toggle toggle-primary" 
							checked={colors.hideBleIcon}
							onchange={(e) => colors.hideBleIcon = e.target.checked}
						/>
					</label>
				</div>
				
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text">Hide Volume Indicator</span>
							<p class="text-xs text-base-content/50">Hide the V1 volume display (requires V1 firmware 4.1028+)</p>
						</div>
						<input 
							type="checkbox" 
							class="toggle toggle-primary" 
							checked={colors.hideVolumeIndicator}
							onchange={(e) => colors.hideVolumeIndicator = e.target.checked}
						/>
					</label>
				</div>
				
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text">🔊 Voice Alerts</span>
							<p class="text-xs text-base-content/50">Announce priority alerts when no phone app is connected</p>
						</div>
						<input 
							type="checkbox" 
							class="toggle toggle-primary" 
							checked={colors.voiceAlertsEnabled}
							onchange={(e) => colors.voiceAlertsEnabled = e.target.checked}
						/>
					</label>
				</div>
			</div>
		</div>
		
		<!-- Signal Bars -->
		<div class="card bg-base-200">
			<div class="card-body p-4">
				<h2 class="card-title text-lg">Signal Bars</h2>
				<p class="text-xs text-base-content/50 mb-2">Bar 1 = weakest, Bar 6 = strongest</p>
				<div class="grid grid-cols-3 md:grid-cols-6 gap-2">
					{#each [1, 2, 3, 4, 5, 6] as barNum}
						{@const barId = `bar-${barNum}-color`}
						<div class="form-control">
							<label class="label py-1" for={barId}>
								<span class="label-text text-xs">Bar {barNum}</span>
							</label>
							<div class="flex flex-col items-center gap-1">
								<input 
									id={barId}
									type="color" 
									aria-label="Signal bar {barNum} color"
									class="w-10 h-8 cursor-pointer rounded border-0"
									value={rgb565ToHex(colors[`bar${barNum}`])}
									onchange={(e) => updateColor(`bar${barNum}`, e.target.value)}
								/>
								<div 
									class="w-8 h-3 rounded"
									style="background-color: {rgb565ToHex(colors[`bar${barNum}`])}"
								></div>
							</div>
						</div>
					{/each}
				</div>
				<!-- Preview stack -->
				<div class="flex justify-center mt-3">
					<div class="flex flex-col-reverse gap-1">
						{#each [1, 2, 3, 4, 5, 6] as barNum}
							<div 
								class="w-12 h-2 rounded"
								style="background-color: {rgb565ToHex(colors[`bar${barNum}`])}"
							></div>
						{/each}
					</div>
				</div>
			</div>
		</div>
		
		<!-- Action Buttons -->
		<div class="flex gap-3">
			<button 
				class="btn btn-primary flex-1" 
				onclick={saveColors}
				disabled={saving}
			>
				{#if saving}
					<span class="loading loading-spinner loading-sm"></span>
				{:else}
					💾 Save Colors
				{/if}
			</button>
			<button class="btn btn-secondary" onclick={testColors}>
				🧪 Test
			</button>
			<button class="btn btn-outline" onclick={resetDefaults}>
				🔄 Reset
			</button>
		</div>
		
		<div class="text-xs text-base-content/40 text-center">
			Colors use RGB565 format. Save triggers a preview on the display.
		</div>
	{/if}
</div>
