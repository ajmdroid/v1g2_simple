<script>
	import { onMount } from 'svelte';
	
	// RGB565 color values (stored on ESP32)
	let colors = $state({
		bogey: 0xF800,   // Red
		freq: 0xF800,    // Red
		arrow: 0xF800,   // Red
		bandL: 0x001F,   // Blue
		bandKa: 0xF800,  // Red
		bandK: 0x001F,   // Blue
		bandX: 0x07E0,   // Green
		wifiIcon: 0x07FF, // Cyan
		bar1: 0x07E0,    // Green (weakest)
		bar2: 0x07E0,    // Green
		bar3: 0xFFE0,    // Yellow
		bar4: 0xFFE0,    // Yellow
		bar5: 0xF800,    // Red
		bar6: 0xF800,    // Red (strongest)
		hideWifiIcon: false,
		hideProfileIndicator: false
	});
	
	let loading = $state(true);
	let saving = $state(false);
	let message = $state(null);
	
	onMount(async () => {
		await fetchColors();
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
			params.append('arrow', colors.arrow);
			params.append('bandL', colors.bandL);
			params.append('bandKa', colors.bandKa);
			params.append('bandK', colors.bandK);
			params.append('bandX', colors.bandX);
			params.append('wifiIcon', colors.wifiIcon);
			params.append('bar1', colors.bar1);
			params.append('bar2', colors.bar2);
			params.append('bar3', colors.bar3);
			params.append('bar4', colors.bar4);
			params.append('bar5', colors.bar5);
			params.append('bar6', colors.bar6);
			params.append('hideWifiIcon', colors.hideWifiIcon);
			params.append('hideProfileIndicator', colors.hideProfileIndicator);
			
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
	
	async function previewColors() {
		try {
			await fetch('/api/displaycolors/preview', { method: 'POST' });
		} catch (e) {
			// Silent fail for preview
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
					arrow: 0xF800,
					bandL: 0x001F,
					bandKa: 0xF800,
					bandK: 0x001F,
					bandX: 0x07E0,
					wifiIcon: 0x07FF,
					bar1: 0x07E0,
					bar2: 0x07E0,
					bar3: 0xFFE0,
					bar4: 0xFFE0,
					bar5: 0xF800,
					bar6: 0xF800,
					hideWifiIcon: false,
					hideProfileIndicator: false
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
		<div class="alert alert-{message.type === 'error' ? 'error' : 'success'}">
			<span>{message.text}</span>
			<button class="btn btn-ghost btn-xs" onclick={() => message = null}>✕</button>
		</div>
	{/if}
	
	{#if loading}
		<div class="flex justify-center p-8">
			<span class="loading loading-spinner loading-lg"></span>
		</div>
	{:else}
		<!-- Counter & Frequency -->
		<div class="card bg-base-200">
			<div class="card-body p-4">
				<h2 class="card-title text-lg">Counter & Frequency</h2>
				<div class="grid grid-cols-2 gap-4">
					<div class="form-control">
						<label class="label">
							<span class="label-text">Bogey Counter</span>
						</label>
						<div class="flex items-center gap-3">
							<input 
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
						<label class="label">
							<span class="label-text">Frequency Display</span>
						</label>
						<div class="flex items-center gap-3">
							<input 
								type="color" 
								aria-label="Frequency display color"
								class="w-12 h-10 cursor-pointer rounded border-0"
								value={rgb565ToHex(colors.freq)}
								onchange={(e) => updateColor('freq', e.target.value)}
							/>
							<span 
								class="text-2xl font-bold font-mono"
								style="color: {rgb565ToHex(colors.freq)}"
							>35.5</span>
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
						<label class="label">
							<span class="label-text">Laser (L)</span>
						</label>
						<div class="flex items-center gap-3">
							<input 
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
						<label class="label">
							<span class="label-text">Ka Band</span>
						</label>
						<div class="flex items-center gap-3">
							<input 
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
						<label class="label">
							<span class="label-text">K Band</span>
						</label>
						<div class="flex items-center gap-3">
							<input 
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
						<label class="label">
							<span class="label-text">X Band</span>
						</label>
						<div class="flex items-center gap-3">
							<input 
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
				<div class="form-control">
					<label class="label">
						<span class="label-text">Arrow Color</span>
					</label>
					<div class="flex items-center gap-3">
						<input 
							type="color" 
							aria-label="Direction arrow color"
							class="w-12 h-10 cursor-pointer rounded border-0"
							value={rgb565ToHex(colors.arrow)}
							onchange={(e) => updateColor('arrow', e.target.value)}
						/>
						<span 
							class="text-2xl font-bold"
							style="color: {rgb565ToHex(colors.arrow)}"
						>▲ ▼</span>
					</div>
				</div>
			</div>
		</div>
		
		<!-- Status Indicators -->
		<div class="card bg-base-200">
			<div class="card-body p-4">
				<h2 class="card-title text-lg">Status Indicators</h2>
				<div class="form-control">
					<label class="label">
						<span class="label-text">WiFi Icon</span>
					</label>
					<div class="flex items-center gap-3">
						<input 
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
			</div>
		</div>
		
		<!-- Signal Bars -->
		<div class="card bg-base-200">
			<div class="card-body p-4">
				<h2 class="card-title text-lg">Signal Bars</h2>
				<p class="text-xs text-base-content/50 mb-2">Bar 1 = weakest, Bar 6 = strongest</p>
				<div class="grid grid-cols-3 md:grid-cols-6 gap-2">
					{#each [1, 2, 3, 4, 5, 6] as barNum}
						<div class="form-control">
							<label class="label py-1">
								<span class="label-text text-xs">Bar {barNum}</span>
							</label>
							<div class="flex flex-col items-center gap-1">
								<input 
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
			<button class="btn btn-secondary" onclick={previewColors}>
				👁️ Preview
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
