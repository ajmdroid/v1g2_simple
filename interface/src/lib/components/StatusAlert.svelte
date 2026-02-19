<script>
	let { message = null, fallbackType = 'info', dismiss = null } = $props();

	function resolveType(type) {
		if (type === 'error') return 'error';
		if (type === 'success') return 'success';
		if (type === 'warning') return 'warning';
		if (type === 'info') return 'info';
		if (fallbackType === 'warning') return 'warning';
		return fallbackType === 'success' ? 'success' : 'info';
	}
</script>

{#if message}
	<div class="surface-alert alert-{resolveType(message?.type)}" role="status" aria-live="polite">
		<span>{message?.text ?? message}</span>
		{#if dismiss}
			<button class="btn btn-ghost btn-xs" onclick={dismiss} aria-label="Dismiss message">✕</button>
		{/if}
	</div>
{/if}
