<script>
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	import SettingsObdCard from '$lib/features/settings/SettingsObdCard.svelte';

	let message = $state(null);
</script>

<div class="page-stack">
	<PageHeader title="Integrations" subtitle="OBD connectivity and speed source status." />

	<StatusAlert {message} />

	<SettingsObdCard />
</div>
