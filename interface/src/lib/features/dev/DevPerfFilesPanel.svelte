<script>
	import { formatBytes } from '$lib/utils/format';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';

	let {
		acknowledged,
		perfFiles = [],
		perfFilesLoading = false,
		perfFileActionBusy = '',
		perfFilesInfo = {
			storageReady: false,
			onSdCard: false,
			path: '/perf'
		},
		onrefresh,
		ondownload,
		ondelete
	} = $props();
</script>

<div class="surface-card" class:opacity-50={!acknowledged}>
	<div class="card-body">
		<CardSectionHead title="Perf CSV Files">
			<button
				class="btn btn-sm btn-outline"
				onclick={onrefresh}
				disabled={perfFilesLoading}
			>
				{#if perfFilesLoading}
					<span class="loading loading-spinner loading-xs"></span>
				{:else}
					Refresh
				{/if}
			</button>
		</CardSectionHead>

		<p class="copy-caption-soft">
			Files under <span class="font-mono">{perfFilesInfo.path}</span>.
			Download or delete without opening contents.
		</p>

		{#if perfFilesLoading}
			<div class="state-loading inline">
				<span class="loading loading-spinner loading-sm"></span>
			</div>
		{:else if !perfFilesInfo.storageReady || !perfFilesInfo.onSdCard}
			<StatusAlert message="SD storage not ready. Perf CSV files are unavailable." fallbackType="warning" />
		{:else if perfFiles.length === 0}
			<div class="state-empty py-2">
				No perf CSV files found.
			</div>
		{:else}
			<div class="surface-table-wrap">
				<table class="table table-sm">
					<thead>
						<tr>
							<th>File</th>
							<th class="text-right">Size</th>
							<th class="text-right">Actions</th>
						</tr>
					</thead>
					<tbody>
						{#each perfFiles as file}
							<tr>
								<td class="font-mono text-xs">
									{file.name}
									{#if file.active}
										<span class="badge badge-xs badge-primary ml-2">active</span>
									{/if}
								</td>
								<td class="text-right text-xs">{formatBytes(file.sizeBytes || 0)}</td>
								<td class="text-right">
									<div class="flex justify-end gap-2">
										<button
											class="btn btn-xs btn-outline"
											onclick={() => ondownload(file.name)}
											disabled={!acknowledged || perfFileActionBusy === file.name}
										>
											Download
										</button>
										<button
											class="btn btn-xs btn-outline btn-error"
											onclick={() => ondelete(file.name)}
											disabled={!acknowledged || perfFileActionBusy === file.name}
										>
											{#if perfFileActionBusy === file.name}
												<span class="loading loading-spinner loading-xs"></span>
											{:else}
												Delete
											{/if}
										</button>
									</div>
								</td>
							</tr>
						{/each}
					</tbody>
				</table>
			</div>
		{/if}
	</div>
</div>
