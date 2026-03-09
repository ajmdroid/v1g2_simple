<script>
	let {
		open = false,
		label = '',
		r = $bindable(0),
		g = $bindable(0),
		b = $bindable(0),
		oncancel,
		onapply
	} = $props();

	function getPickerHex() {
		return `#${[r, g, b].map((value) => Math.min(255, value).toString(16).padStart(2, '0')).join('')}`;
	}
</script>

{#if open}
	<div class="modal modal-open">
		<div class="modal-box surface-modal">
			<h3 class="font-bold text-lg mb-4">{label}</h3>

			<div class="surface-color-preview" style="background-color: {getPickerHex()}"></div>

			<div class="space-y-4">
				<div class="form-control">
					<label class="label" for="camera-picker-red">
						<span class="label-text font-semibold text-error">Red</span>
						<span class="label-text-alt font-mono">{r}</span>
					</label>
					<input
						id="camera-picker-red"
						type="range"
						min="0"
						max="248"
						step="8"
						bind:value={r}
						class="range range-error"
					/>
				</div>

				<div class="form-control">
					<label class="label" for="camera-picker-green">
						<span class="label-text font-semibold text-success">Green</span>
						<span class="label-text-alt font-mono">{g}</span>
					</label>
					<input
						id="camera-picker-green"
						type="range"
						min="0"
						max="252"
						step="4"
						bind:value={g}
						class="range range-success"
					/>
				</div>

				<div class="form-control">
					<label class="label" for="camera-picker-blue">
						<span class="label-text font-semibold text-info">Blue</span>
						<span class="label-text-alt font-mono">{b}</span>
					</label>
					<input
						id="camera-picker-blue"
						type="range"
						min="0"
						max="248"
						step="8"
						bind:value={b}
						class="range range-info"
					/>
				</div>
			</div>

			<div class="mt-4">
				<span class="copy-muted">Quick colors:</span>
				<div class="flex gap-2 mt-2 flex-wrap">
					<button class="btn btn-sm" style="background-color: #f80000" onclick={() => { r = 248; g = 0; b = 0; }}>Red</button>
					<button class="btn btn-sm" style="background-color: #00fc00" onclick={() => { r = 0; g = 252; b = 0; }}>Green</button>
					<button class="btn btn-sm" style="background-color: #0000f8" onclick={() => { r = 0; g = 0; b = 248; }}>Blue</button>
					<button class="btn btn-sm" style="background-color: #f8fc00" onclick={() => { r = 248; g = 252; b = 0; }}>Yellow</button>
					<button class="btn btn-sm" style="background-color: #00fcf8" onclick={() => { r = 0; g = 252; b = 248; }}>Cyan</button>
					<button class="btn btn-sm" style="background-color: #f800f8" onclick={() => { r = 248; g = 0; b = 248; }}>Magenta</button>
					<button class="btn btn-sm" style="background-color: #f8a000" onclick={() => { r = 248; g = 160; b = 0; }}>Orange</button>
					<button class="btn btn-sm bg-white text-black" onclick={() => { r = 248; g = 252; b = 248; }}>White</button>
				</div>
			</div>

			<div class="modal-action">
				<button class="btn btn-ghost" onclick={oncancel}>Cancel</button>
				<button class="btn btn-primary" onclick={onapply}>Apply</button>
			</div>
		</div>
		<div
			class="modal-backdrop"
			onclick={oncancel}
			onkeydown={(event) => {
				if (event.key === 'Enter' || event.key === ' ') {
					event.preventDefault();
					oncancel();
				}
			}}
			role="button"
			tabindex="0"
			aria-label="Close color picker"
		></div>
	</div>
{/if}
