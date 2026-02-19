<script>
	let { title, subtitle = '', children } = $props();
</script>

<div class="page-header">
	<div class="page-header-copy">
		<h1 class="page-title">{title}</h1>
		{#if subtitle}
			<p class="page-subtitle">{subtitle}</p>
		{/if}
	</div>
	{#if children}
		<div class="page-header-actions">
			{@render children()}
		</div>
	{/if}
</div>
