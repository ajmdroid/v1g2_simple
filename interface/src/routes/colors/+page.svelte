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
		wifiConnected: 0x07E0, // Green (client connected)
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
		rssiV1: 0x07E0,  // Green (V1 RSSI label)
		rssiProxy: 0x001F, // Blue (Proxy RSSI label)
		hideWifiIcon: false,
		hideProfileIndicator: false,
		hideBatteryIcon: false,
		hideBleIcon: false,
		hideVolumeIndicator: false,
		hideRssiIndicator: false,
		brightness: 200  // Display brightness (0-255)
	});
	
	let displayStyle = $state(0);  // 0 = Classic, 1 = Modern, 2 = Hemi
	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);
	
	// Custom color picker state
	let pickerOpen = $state(false);
	let pickerKey = $state(null);
	let pickerLabel = $state('');
	let pickerR = $state(0);
	let pickerG = $state(0);
	let pickerB = $state(0);
	
	// Open the custom color picker
	function openPicker(key, label) {
		pickerKey = key;
		pickerLabel = label;
		// Convert RGB565 to RGB888 for sliders
		const rgb565 = colors[key] || 0;
		pickerR = ((rgb565 >> 11) & 0x1F) << 3;
		pickerG = ((rgb565 >> 5) & 0x3F) << 2;
		pickerB = (rgb565 & 0x1F) << 3;
		pickerOpen = true;
	}
	
	// Apply color from picker
	function applyPickerColor() {
		if (pickerKey) {
			const r = pickerR >> 3;
			const g = pickerG >> 2;
			const b = pickerB >> 3;
			colors[pickerKey] = (r << 11) | (g << 5) | b;
		}
		pickerOpen = false;
	}
	
	// Cancel picker
	function cancelPicker() {
		pickerOpen = false;
	}
	
	// Get hex color from picker RGB values
	function getPickerHex() {
		return '#' + [pickerR, pickerG, pickerB].map(x => Math.min(255, x).toString(16).padStart(2, '0')).join('');
	}
	
	onMount(async () => {
		await Promise.all([fetchColors(), fetchDisplayStyle()]);
	});
	
	async function fetchColors() {
		loading = true;
		try {
			const res = await fetch('/api/displaycolors');
			if (res.ok) {
				const data = await res.json();
				// Ensure all color values are parsed as integers (API might return strings)
				for (const key of Object.keys(data)) {
					if (typeof data[key] === 'string' && !['freqUseBandColor', 'hideWifiIcon', 'hideProfileIndicator', 'hideBatteryIcon', 'hideBleIcon', 'hideVolumeIndicator', 'hideRssiIndicator'].includes(key)) {
						data[key] = parseInt(data[key], 10);
					}
				}
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
		// Ensure we have a valid number, default to black if not
		const val = typeof rgb565 === 'number' ? rgb565 : 0;
		const r = ((val >> 11) & 0x1F) << 3;
		const g = ((val >> 5) & 0x3F) << 2;
		const b = (val & 0x1F) << 3;
		return '#' + [r, g, b].map(x => x.toString(16).padStart(2, '0')).join('');
	}
	
	// Convert hex color to RGB565
	function hexToRgb565(hex) {
		if (!hex || hex.length < 7) return 0;
		const r = parseInt(hex.slice(1, 3), 16) >> 3;
		const g = parseInt(hex.slice(3, 5), 16) >> 2;
		const b = parseInt(hex.slice(5, 7), 16) >> 3;
		return (r << 11) | (g << 5) | b;
	}
	
	// Format RGB565 as hex string for display (e.g., "F800")
	function rgb565ToHexStr(rgb565) {
		const val = typeof rgb565 === 'number' ? rgb565 : 0;
		return val.toString(16).toUpperCase().padStart(4, '0');
	}
	
	// Parse user input - accepts RGB565 (F800, 0xF800) or RGB888 (#FF0000, FF0000)
	function parseColorInput(input) {
		let clean = input.trim().toUpperCase();
		
		// Remove common prefixes
		if (clean.startsWith('0X')) clean = clean.slice(2);
		if (clean.startsWith('#')) clean = clean.slice(1);
		
		// Validate hex characters
		if (!/^[0-9A-F]+$/.test(clean)) return null;
		
		if (clean.length <= 4) {
			// RGB565 format (1-4 hex digits)
			const value = parseInt(clean, 16);
			if (value > 0xFFFF) return null;
			return value;
		} else if (clean.length === 5) {
			// 5 digits - treat as RGB565 with leading digit
			const value = parseInt(clean, 16);
			if (value > 0xFFFF) return null;
			return value;
		} else if (clean.length === 6) {
			// RGB888 format - convert to RGB565
			const r = parseInt(clean.slice(0, 2), 16) >> 3;
			const g = parseInt(clean.slice(2, 4), 16) >> 2;
			const b = parseInt(clean.slice(4, 6), 16) >> 3;
			return (r << 11) | (g << 5) | b;
		}
		
		return null;
	}
	
	// Handle hex input change
	function handleHexInput(key, value) {
		const parsed = parseColorInput(value);
		if (parsed !== null) {
			colors[key] = parsed;
		}
	}
	
	// Force the native input value before picker opens (fixes macOS color picker issue)
	function syncColorInput(event, key) {
		const hex = rgb565ToHex(colors[key]);
		event.target.value = hex;
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
			params.append('wifiConnected', colors.wifiConnected);
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
			params.append('rssiV1', colors.rssiV1);
			params.append('rssiProxy', colors.rssiProxy);
			params.append('hideWifiIcon', colors.hideWifiIcon);
			params.append('hideProfileIndicator', colors.hideProfileIndicator);
			params.append('hideBatteryIcon', colors.hideBatteryIcon);
			params.append('hideBleIcon', colors.hideBleIcon);
			params.append('hideVolumeIndicator', colors.hideVolumeIndicator);
			params.append('hideRssiIndicator', colors.hideRssiIndicator);
			params.append('brightness', colors.brightness);
			
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
					wifiConnected: 0x07E0,
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
					rssiV1: 0x07E0,
					rssiProxy: 0x001F,
					hideWifiIcon: false,
					hideProfileIndicator: false,
					hideBatteryIcon: false,
					hideBleIcon: false,
					hideVolumeIndicator: false,
					hideRssiIndicator: false
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
						<option value={0}>Classic (V1 tech)</option>
						<option value={1}>Modern</option>
						<option value={2}>Hemi</option>
						<option value={3}>Serpentine</option>
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
						<div class="flex items-center gap-2">
							<button 
								id="bogey-color"
								type="button"
								aria-label="Bogey counter color"
								class="w-12 h-10 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.bogey)}"
								onclick={() => openPicker('bogey', 'Bogey Counter')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-sm w-20 font-mono text-xs"
								value={rgb565ToHexStr(colors.bogey)}
								onchange={(e) => handleHexInput('bogey', e.target.value)}
								title="RGB565 hex (or RGB888)"
								placeholder="F800"
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
						<div class="flex items-center gap-2">
							<button 
								id="freq-color"
								type="button"
								aria-label="Frequency display color"
								class="w-12 h-10 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.freq)}"
								onclick={() => openPicker('freq', 'Frequency Display')}
								disabled={colors.freqUseBandColor}
							></button>
							<input 
								type="text"
								class="input input-bordered input-sm w-20 font-mono text-xs"
								value={rgb565ToHexStr(colors.freq)}
								onchange={(e) => handleHexInput('freq', e.target.value)}
								title="RGB565 hex (or RGB888)"
								placeholder="F800"
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
					<div class="flex items-center gap-2">
						<button 
							id="muted-color"
							type="button"
							aria-label="Muted alert color"
							class="w-12 h-10 cursor-pointer rounded border-2 border-base-300"
							style="background-color: {rgb565ToHex(colors.muted)}"
							onclick={() => openPicker('muted', 'Muted Alert')}
						></button>
						<input 
							type="text"
							class="input input-bordered input-sm w-20 font-mono text-xs"
							value={rgb565ToHexStr(colors.muted)}
							onchange={(e) => handleHexInput('muted', e.target.value)}
							title="RGB565 hex (or RGB888)"
							placeholder="3186"
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
					<div class="flex items-center gap-2">
						<button 
							id="persisted-color"
							type="button"
							aria-label="Persisted alert color"
							class="w-12 h-10 cursor-pointer rounded border-2 border-base-300"
							style="background-color: {rgb565ToHex(colors.persisted)}"
							onclick={() => openPicker('persisted', 'Persisted Alert')}
						></button>
						<input 
							type="text"
							class="input input-bordered input-sm w-20 font-mono text-xs"
							value={rgb565ToHexStr(colors.persisted)}
							onchange={(e) => handleHexInput('persisted', e.target.value)}
							title="RGB565 hex (or RGB888)"
							placeholder="18C3"
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
							<button 
								id="volumeMain-color"
								type="button"
								aria-label="Main volume color"
								class="w-10 h-8 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.volumeMain)}"
								onclick={() => openPicker('volumeMain', 'Main Volume')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-xs w-16 font-mono text-xs"
								value={rgb565ToHexStr(colors.volumeMain)}
								onchange={(e) => handleHexInput('volumeMain', e.target.value)}
								title="RGB565 hex (or RGB888)"
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
							<button 
								id="volumeMute-color"
								type="button"
								aria-label="Mute volume color"
								class="w-10 h-8 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.volumeMute)}"
								onclick={() => openPicker('volumeMute', 'Mute Volume')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-xs w-16 font-mono text-xs"
								value={rgb565ToHexStr(colors.volumeMute)}
								onchange={(e) => handleHexInput('volumeMute', e.target.value)}
								title="RGB565 hex (or RGB888)"
							/>
							<span 
								class="text-lg font-bold font-mono"
								style="color: {rgb565ToHex(colors.volumeMute)}"
							>0M</span>
						</div>
					</div>
				</div>
				<div class="divider my-2"></div>
				<h3 class="font-semibold text-sm mt-2">RSSI Labels</h3>
				<p class="text-sm text-base-content/50 mb-2">Colors for V1 and Proxy connection strength labels</p>
				<div class="grid grid-cols-2 gap-4">
					<div class="form-control">
						<label class="label" for="rssiV1-color">
							<span class="label-text">V1 RSSI (V)</span>
						</label>
						<div class="flex items-center gap-2">
							<button 
								id="rssiV1-color"
								type="button"
								aria-label="V1 RSSI label color"
								class="w-10 h-8 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.rssiV1)}"
								onclick={() => openPicker('rssiV1', 'V1 RSSI Label')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-xs w-16 font-mono text-xs"
								value={rgb565ToHexStr(colors.rssiV1)}
								onchange={(e) => handleHexInput('rssiV1', e.target.value)}
								title="RGB565 hex (or RGB888)"
							/>
							<span 
								class="text-lg font-bold font-mono"
								style="color: {rgb565ToHex(colors.rssiV1)}"
							>V</span>
							<span class="text-lg font-mono text-success">-55</span>
						</div>
					</div>
					<div class="form-control">
						<label class="label" for="rssiProxy-color">
							<span class="label-text">Proxy RSSI (P)</span>
						</label>
						<div class="flex items-center gap-2">
							<button 
								id="rssiProxy-color"
								type="button"
								aria-label="Proxy RSSI label color"
								class="w-10 h-8 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.rssiProxy)}"
								onclick={() => openPicker('rssiProxy', 'Proxy RSSI Label')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-xs w-16 font-mono text-xs"
								value={rgb565ToHexStr(colors.rssiProxy)}
								onchange={(e) => handleHexInput('rssiProxy', e.target.value)}
								title="RGB565 hex (or RGB888)"
							/>
							<span 
								class="text-lg font-bold font-mono"
								style="color: {rgb565ToHex(colors.rssiProxy)}"
							>P</span>
							<span class="text-lg font-mono text-success">-62</span>
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
						<div class="flex items-center gap-2">
							<button 
								id="bandL-color"
								type="button"
								aria-label="Laser band color"
								class="w-12 h-10 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.bandL)}"
								onclick={() => openPicker('bandL', 'Laser Band')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-xs w-16 font-mono text-xs"
								value={rgb565ToHexStr(colors.bandL)}
								onchange={(e) => handleHexInput('bandL', e.target.value)}
								title="RGB565 hex (or RGB888)"
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
						<div class="flex items-center gap-2">
							<button 
								id="bandKa-color"
								type="button"
								aria-label="Ka band color"
								class="w-12 h-10 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.bandKa)}"
								onclick={() => openPicker('bandKa', 'Ka Band')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-xs w-16 font-mono text-xs"
								value={rgb565ToHexStr(colors.bandKa)}
								onchange={(e) => handleHexInput('bandKa', e.target.value)}
								title="RGB565 hex (or RGB888)"
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
						<div class="flex items-center gap-2">
							<button 
								id="bandK-color"
								type="button"
								aria-label="K band color"
								class="w-12 h-10 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.bandK)}"
								onclick={() => openPicker('bandK', 'K Band')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-xs w-16 font-mono text-xs"
								value={rgb565ToHexStr(colors.bandK)}
								onchange={(e) => handleHexInput('bandK', e.target.value)}
								title="RGB565 hex (or RGB888)"
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
						<div class="flex items-center gap-2">
							<button 
								id="bandX-color"
								type="button"
								aria-label="X band color"
								class="w-12 h-10 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.bandX)}"
								onclick={() => openPicker('bandX', 'X Band')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-xs w-16 font-mono text-xs"
								value={rgb565ToHexStr(colors.bandX)}
								onchange={(e) => handleHexInput('bandX', e.target.value)}
								title="RGB565 hex (or RGB888)"
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
							<button 
								id="arrow-front-color"
								type="button"
								aria-label="Front arrow color"
								class="w-10 h-10 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.arrowFront)}"
								onclick={() => openPicker('arrowFront', 'Front Arrow')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-xs w-16 font-mono text-xs"
								value={rgb565ToHexStr(colors.arrowFront)}
								onchange={(e) => handleHexInput('arrowFront', e.target.value)}
								title="RGB565 hex (or RGB888)"
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
							<button 
								id="arrow-side-color"
								type="button"
								aria-label="Side arrow color"
								class="w-10 h-10 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.arrowSide)}"
								onclick={() => openPicker('arrowSide', 'Side Arrows')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-xs w-16 font-mono text-xs"
								value={rgb565ToHexStr(colors.arrowSide)}
								onchange={(e) => handleHexInput('arrowSide', e.target.value)}
								title="RGB565 hex (or RGB888)"
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
							<button 
								id="arrow-rear-color"
								type="button"
								aria-label="Rear arrow color"
								class="w-10 h-10 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.arrowRear)}"
								onclick={() => openPicker('arrowRear', 'Rear Arrow')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-xs w-16 font-mono text-xs"
								value={rgb565ToHexStr(colors.arrowRear)}
								onchange={(e) => handleHexInput('arrowRear', e.target.value)}
								title="RGB565 hex (or RGB888)"
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
				
				<div class="grid grid-cols-2 gap-4">
					<div class="form-control">
						<label class="label" for="wifiConnected-color">
							<span class="label-text">WiFi Connected</span>
						</label>
						<div class="flex items-center gap-2">
							<button 
								id="wifiConnected-color"
								type="button"
								aria-label="WiFi connected color"
								class="w-12 h-10 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.wifiConnected)}"
								onclick={() => openPicker('wifiConnected', 'WiFi Connected')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-xs w-16 font-mono text-xs"
								value={rgb565ToHexStr(colors.wifiConnected)}
								onchange={(e) => handleHexInput('wifiConnected', e.target.value)}
								title="RGB565 hex (or RGB888)"
							/>
							<span 
								class="text-2xl font-bold"
								style="color: {rgb565ToHex(colors.wifiConnected)}"
							>📶</span>
						</div>
					</div>
					<div class="form-control">
						<label class="label" for="wifiIcon-color">
							<span class="label-text">WiFi (No Client)</span>
						</label>
						<div class="flex items-center gap-2">
							<button 
								id="wifiIcon-color"
								type="button"
								aria-label="WiFi icon color"
								class="w-12 h-10 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.wifiIcon)}"
								onclick={() => openPicker('wifiIcon', 'WiFi (No Client)')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-xs w-16 font-mono text-xs"
								value={rgb565ToHexStr(colors.wifiIcon)}
								onchange={(e) => handleHexInput('wifiIcon', e.target.value)}
								title="RGB565 hex (or RGB888)"
							/>
							<span 
								class="text-2xl font-bold"
								style="color: {rgb565ToHex(colors.wifiIcon)}"
							>📶</span>
						</div>
					</div>
				</div>
				
				<div class="grid grid-cols-2 gap-4">
					<div class="form-control">
						<label class="label" for="bleConnected-color">
							<span class="label-text">Proxy Connected</span>
						</label>
						<div class="flex items-center gap-2">
							<button 
								id="bleConnected-color"
								type="button"
								aria-label="Bluetooth connected color"
								class="w-12 h-10 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.bleConnected)}"
								onclick={() => openPicker('bleConnected', 'Proxy Connected')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-xs w-16 font-mono text-xs"
								value={rgb565ToHexStr(colors.bleConnected)}
								onchange={(e) => handleHexInput('bleConnected', e.target.value)}
								title="RGB565 hex (or RGB888)"
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
						<div class="flex items-center gap-2">
							<button 
								id="bleDisconnected-color"
								type="button"
								aria-label="BLE proxy ready color"
								class="w-12 h-10 cursor-pointer rounded border-2 border-base-300"
								style="background-color: {rgb565ToHex(colors.bleDisconnected)}"
								onclick={() => openPicker('bleDisconnected', 'Proxy Ready')}
							></button>
							<input 
								type="text"
								class="input input-bordered input-xs w-16 font-mono text-xs"
								value={rgb565ToHexStr(colors.bleDisconnected)}
								onchange={(e) => handleHexInput('bleDisconnected', e.target.value)}
								title="RGB565 hex (or RGB888)"
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
							<span class="label-text">Hide RSSI Indicator</span>
							<p class="text-xs text-base-content/50">Hide the BLE signal strength display</p>
						</div>
						<input 
							type="checkbox" 
							class="toggle toggle-primary" 
							checked={colors.hideRssiIndicator}
							onchange={(e) => colors.hideRssiIndicator = e.target.checked}
						/>
					</label>
				</div>
			</div>
		</div>
		
		<!-- Display Brightness -->
		<div class="card bg-base-200">
			<div class="card-body p-4">
				<h2 class="card-title text-lg">☀️ Display Brightness</h2>
				<p class="text-xs text-base-content/50 mb-2">Adjust the AMOLED screen brightness (0-255)</p>
				<div class="form-control">
					<div class="flex items-center gap-4">
						<label for="brightness-slider" class="text-sm">🌑</label>
						<input 
							id="brightness-slider"
							type="range" 
							min="0" 
							max="255" 
							bind:value={colors.brightness}
							class="range range-primary flex-1" 
						/>
						<span class="text-sm">☀️</span>
						<span class="text-sm font-mono w-12 text-right">{colors.brightness}</span>
					</div>
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
						{@const barKey = `bar${barNum}`}
						<div class="form-control">
							<label class="label py-1" for={barId}>
								<span class="label-text text-xs">Bar {barNum}</span>
							</label>
							<div class="flex flex-col items-center gap-1">
								<button 
									id={barId}
									type="button"
									aria-label="Signal bar {barNum} color"
									class="w-10 h-8 cursor-pointer rounded border-2 border-base-300"
									style="background-color: {rgb565ToHex(colors[barKey])}"
									onclick={() => openPicker(barKey, `Signal Bar ${barNum}`)}
								></button>
								<input 
									type="text"
									class="input input-bordered input-xs w-14 font-mono text-xs text-center"
									value={rgb565ToHexStr(colors[barKey])}
									onchange={(e) => handleHexInput(barKey, e.target.value)}
									title="RGB565 hex (or RGB888)"
								/>
								<div 
									class="w-8 h-3 rounded"
									style="background-color: {rgb565ToHex(colors[barKey])}"
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

<!-- Custom Color Picker Modal -->
{#if pickerOpen}
<div class="modal modal-open">
	<div class="modal-box">
		<h3 class="font-bold text-lg mb-4">{pickerLabel}</h3>
		
		<!-- Color preview -->
		<div 
			class="w-full h-20 rounded-lg mb-4 border-2 border-base-300"
			style="background-color: {getPickerHex()}"
		></div>
		
		<!-- RGB Sliders -->
		<div class="space-y-4">
			<div class="form-control">
				<label class="label" for="picker-red">
					<span class="label-text font-semibold text-error">Red</span>
					<span class="label-text-alt font-mono">{pickerR}</span>
				</label>
				<input 
					id="picker-red"
					type="range" 
					min="0" 
					max="248" 
					step="8"
					bind:value={pickerR}
					class="range range-error"
				/>
			</div>
			
			<div class="form-control">
				<label class="label" for="picker-green">
					<span class="label-text font-semibold text-success">Green</span>
					<span class="label-text-alt font-mono">{pickerG}</span>
				</label>
				<input 
					id="picker-green"
					type="range" 
					min="0" 
					max="252" 
					step="4"
					bind:value={pickerG}
					class="range range-success"
				/>
			</div>
			
			<div class="form-control">
				<label class="label" for="picker-blue">
					<span class="label-text font-semibold text-info">Blue</span>
					<span class="label-text-alt font-mono">{pickerB}</span>
				</label>
				<input 
					id="picker-blue"
					type="range" 
					min="0" 
					max="248" 
					step="8"
					bind:value={pickerB}
					class="range range-info"
				/>
			</div>
		</div>
		
		<!-- Quick presets -->
		<div class="mt-4">
			<span class="text-sm text-base-content/60">Quick colors:</span>
			<div class="flex gap-2 mt-2 flex-wrap">
				<button class="btn btn-sm" style="background-color: #f80000" onclick={() => { pickerR = 248; pickerG = 0; pickerB = 0; }}>Red</button>
				<button class="btn btn-sm" style="background-color: #00fc00" onclick={() => { pickerR = 0; pickerG = 252; pickerB = 0; }}>Green</button>
				<button class="btn btn-sm" style="background-color: #0000f8" onclick={() => { pickerR = 0; pickerG = 0; pickerB = 248; }}>Blue</button>
				<button class="btn btn-sm" style="background-color: #f8fc00" onclick={() => { pickerR = 248; pickerG = 252; pickerB = 0; }}>Yellow</button>
				<button class="btn btn-sm" style="background-color: #00fcf8" onclick={() => { pickerR = 0; pickerG = 252; pickerB = 248; }}>Cyan</button>
				<button class="btn btn-sm" style="background-color: #f800f8" onclick={() => { pickerR = 248; pickerG = 0; pickerB = 248; }}>Magenta</button>
				<button class="btn btn-sm" style="background-color: #f8a000" onclick={() => { pickerR = 248; pickerG = 160; pickerB = 0; }}>Orange</button>
				<button class="btn btn-sm bg-white text-black" onclick={() => { pickerR = 248; pickerG = 252; pickerB = 248; }}>White</button>
			</div>
		</div>
		
		<div class="modal-action">
			<button class="btn btn-ghost" onclick={cancelPicker}>Cancel</button>
			<button class="btn btn-primary" onclick={applyPickerColor}>Apply</button>
		</div>
	</div>
	<div
		class="modal-backdrop"
		onclick={cancelPicker}
		onkeydown={(event) => {
			if (event.key === 'Enter' || event.key === ' ') {
				event.preventDefault();
				cancelPicker();
			}
		}}
		role="button"
		tabindex="0"
		aria-label="Close color picker"
	></div>
</div>
{/if}