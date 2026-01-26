<script>
	import { onMount } from 'svelte';
	import { goto } from '$app/navigation';
	
	onMount(() => {
		// Redirect to consolidated Hardware Integrations page
		goto('/integrations', { replaceState: true });
	});
</script>

<div class="flex justify-center items-center p-8">
	<span class="loading loading-spinner loading-lg"></span>
	<span class="ml-4">Redirecting to Hardware Integrations...</span>
</div>
