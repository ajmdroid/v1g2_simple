<script>
	import { onMount } from 'svelte';
	import { goto } from '$app/navigation';
	import PageHeader from '$lib/components/PageHeader.svelte';
	
	onMount(() => {
		// Redirect to consolidated Hardware Integrations page.
		goto('/integrations', { replaceState: true });
	});
</script>

<div class="page-stack">
	<PageHeader title="GPS" subtitle="This page now lives under Integrations." />
		<div class="surface-card">
			<div class="card-body py-6 copy-subtle">
				<div class="flex items-center gap-3">
				<span class="loading loading-spinner loading-md"></span>
				<span>Redirecting to Hardware Integrations...</span>
			</div>
		</div>
	</div>
</div>
