<script>
	import { onMount, onDestroy } from 'svelte';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	import {
		otaStatus,
		otaError,
		checkForUpdate,
		startUpdate,
		cancelUpdate,
		refreshOtaStatus,
		stopProgressPoll
	} from '$lib/stores/otaStatus.svelte.js';

	let checking = $state(false);
	let updating = $state(false);
	let cancelling = $state(false);
	let confirmBreaking = $state(false);
	let message = $state(null);

	onMount(async () => {
		await refreshOtaStatus();
	});

	onDestroy(() => {
		stopProgressPoll();
	});

	async function handleCheck() {
		checking = true;
		message = null;
		const status = await checkForUpdate();
		checking = false;

		if (!status) {
			message = { type: 'error', text: $otaError || 'Check failed' };
			return;
		}

		if (status.state === 'update_available') {
			message = { type: 'info', text: `Update available: v${status.available_version}` };
		} else if (status.state === 'up_to_date') {
			message = { type: 'success', text: 'Already up to date.' };
		} else if (status.state === 'check_failed') {
			message = { type: 'error', text: status.error || 'Version check failed' };
		}
	}

	async function handleUpdate() {
		if ($otaStatus.breaking && !confirmBreaking) {
			confirmBreaking = true;
			return;
		}

		updating = true;
		message = null;
		confirmBreaking = false;

		const ok = await startUpdate('both');
		if (!ok) {
			updating = false;
			message = { type: 'error', text: $otaError || 'Failed to start update' };
		}
	}

	function cancelConfirm() {
		confirmBreaking = false;
	}

	async function handleCancel() {
		cancelling = true;
		const ok = await cancelUpdate();
		cancelling = false;

		if (ok) {
			updating = false;
			message = { type: 'info', text: 'Update cancelled. Device will restart to restore BLE.' };
		} else {
			message = { type: 'error', text: $otaError || 'Failed to cancel update' };
		}
	}

	$effect(() => {
		const status = $otaStatus;
		if (status.state === 'error') {
			updating = false;
			message = { type: 'error', text: status.error || 'Update failed' };
		} else if (status.state === 'cancelled') {
			updating = false;
			message = { type: 'info', text: 'Update cancelled. Device will restart to restore BLE.' };
		} else if (status.state === 'restarting') {
			message = { type: 'success', text: 'Update complete. Device is restarting...' };
		}
	});
</script>

<div class="surface-card">
	<div class="card-body space-y-4">
		<CardSectionHead
			title="Firmware Update"
			subtitle="Check for and install firmware updates from GitHub."
		/>

		<div class="flex flex-col gap-3">
			<!-- Current version -->
			<div class="flex justify-between items-center">
				<span class="copy-caption">Current version</span>
				<span class="font-mono text-sm">v{$otaStatus.current_version || '...'}</span>
			</div>

			<!-- Available version (if checked) -->
			{#if $otaStatus.check_done && $otaStatus.available_version}
				<div class="flex justify-between items-center">
					<span class="copy-caption">Available version</span>
					<span class="font-mono text-sm">v{$otaStatus.available_version}</span>
				</div>

				{#if $otaStatus.changelog}
					<div class="surface-note text-sm p-2 rounded">
						{$otaStatus.changelog}
					</div>
				{/if}
			{/if}

			<!-- Status message -->
			<StatusAlert {message} />

			<!-- STA not connected warning -->
			{#if !$otaStatus.sta_connected}
				<div class="surface-alert alert-warning text-sm" role="status">
					WiFi client not connected. Connect to a network in WiFi Client settings above to check for updates.
				</div>
			{/if}

			<!-- Version gate block -->
			{#if $otaStatus.blocked_reason === 'version_too_old'}
				<div class="surface-alert alert-error text-sm" role="status">
					Your firmware is too old to update directly. Minimum required: v{$otaStatus.min_from_version}. Use the web installer to update.
				</div>
			{/if}

			<!-- Breaking change confirmation -->
			{#if confirmBreaking}
				<div class="surface-alert alert-warning text-sm" role="alert">
					<div>
						<strong>Breaking change warning</strong>
						{#if $otaStatus.notes}
							<p class="mt-1">{$otaStatus.notes}</p>
						{/if}
						<p class="mt-1">Are you sure you want to proceed?</p>
					</div>
					<div class="flex gap-2 mt-2">
						<button class="btn btn-warning btn-xs" onclick={handleUpdate}>
							Yes, update
						</button>
						<button class="btn btn-ghost btn-xs" onclick={cancelConfirm}>
							Cancel
						</button>
					</div>
				</div>
			{/if}

			<!-- Progress bar during update -->
			{#if updating || $otaStatus.state === 'downloading_firmware' || $otaStatus.state === 'downloading_filesystem' || $otaStatus.state === 'preparing'}
				<div class="flex flex-col gap-2">
					<div class="flex justify-between items-center text-sm">
						<span class="copy-caption">
							{#if $otaStatus.state === 'preparing'}
								Disconnecting BLE...
							{:else if $otaStatus.state === 'downloading_firmware'}
								Downloading firmware...
							{:else if $otaStatus.state === 'downloading_filesystem'}
								Downloading filesystem...
							{:else}
								Preparing...
							{/if}
						</span>
						<span class="font-mono">{$otaStatus.progress}%</span>
					</div>
					<progress class="progress progress-primary w-full" value={$otaStatus.progress} max="100"></progress>
					<div class="flex justify-between items-center">
						<p class="copy-caption text-xs">V1 connection will reconnect after restart.</p>
						{#if $otaStatus.state === 'downloading_firmware' || $otaStatus.state === 'downloading_filesystem'}
							<button
								class="btn btn-ghost btn-xs text-error"
								onclick={handleCancel}
								disabled={cancelling}
							>
								{#if cancelling}
									<span class="loading loading-spinner loading-xs"></span>
								{/if}
								Cancel
							</button>
						{/if}
					</div>
				</div>
			{/if}

			<!-- Restarting state -->
			{#if $otaStatus.state === 'restarting'}
				<div class="flex items-center gap-2 text-sm">
					<span class="loading loading-spinner loading-sm"></span>
					<span>Restarting device...</span>
				</div>
			{/if}

			<!-- Action buttons -->
			{#if $otaStatus.state !== 'restarting' && !updating}
				<div class="flex gap-2">
					<button
						class="btn btn-outline btn-sm"
						onclick={handleCheck}
						disabled={checking || !$otaStatus.sta_connected}
					>
						{#if checking}
							<span class="loading loading-spinner loading-sm"></span>
						{/if}
						Check for Updates
					</button>

					{#if $otaStatus.can_update && !confirmBreaking}
						<button
							class="btn btn-primary btn-sm"
							onclick={handleUpdate}
						>
							Install Update
						</button>
					{/if}
				</div>
			{/if}
		</div>
	</div>
</div>
