<script>
	let {
		title,
		description = '',
		checked = false,
		disabled = false,
		onChange = null
	} = $props();

	function handleChange(event) {
		onChange?.(event.currentTarget.checked);
	}
</script>

<div class="form-control">
	<label class="label cursor-pointer">
		<div>
			<span class="label-text">{title}</span>
			{#if description}
				<p class="copy-caption-soft">{description}</p>
			{/if}
		</div>
		<input
			type="checkbox"
			class="toggle toggle-primary"
			{checked}
			{disabled}
			onchange={handleChange}
		/>
	</label>
</div>
