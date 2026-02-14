<script>
	import { onDestroy, onMount } from 'svelte';

	const STATUS_POLL_INTERVAL_MS = 3000;
	const EVENTS_LIMIT = 24;

	let loading = $state(true);
	let refreshing = $state(false);
	let statusFetchInFlight = $state(false);
	let catalogFetchInFlight = $state(false);
	let eventsFetchInFlight = $state(false);
	let configFetchInFlight = $state(false);
	let configSaveInFlight = $state(false);
	let statusError = $state('');
	let catalogError = $state('');
	let eventsError = $state('');
	let configError = $state('');
	let pollHandle = null;

	let status = $state({
		enabled: false,
		indexLoaded: false,
		lastTickDurationUs: 0,
		maxTickDurationUs: 0,
		lastInternalFree: 0,
		lastInternalLargestBlock: 0,
		memoryGuardMinFree: 0,
		memoryGuardMinLargestBlock: 0,
		counters: {
			cameraTicks: 0,
			cameraTickSkipsOverload: 0,
			cameraTickSkipsNonCore: 0,
			cameraTickSkipsMemoryGuard: 0,
			cameraCandidatesChecked: 0,
			cameraMatches: 0,
			cameraAlertsStarted: 0,
			cameraBudgetExceeded: 0,
			cameraLoadFailures: 0,
			cameraLoadSkipsMemoryGuard: 0,
			cameraIndexSwapCount: 0
		},
		loader: {
			loadAttempts: 0,
			loadFailures: 0,
			loadSkipsMemoryGuard: 0,
			lastSuccessMs: 0,
			lastLoadDurationMs: 0,
			maxLoadDurationMs: 0,
			lastSortDurationMs: 0,
			lastSpanBuildDurationMs: 0,
			taskRunning: false,
			loadInProgress: false,
			reloadPending: false,
			readyVersion: 0
		},
		index: {
			cameraCount: 0,
			bucketCount: 0,
			version: 0
		}
	});

	let catalog = $state({
		success: false,
		storageReady: false,
		message: '',
		tsMs: 0,
		totalCount: 0,
		totalBytes: 0,
		datasets: {
			alpr: { present: false, valid: false, count: 0, bytes: 0 },
			speed: { present: false, valid: false, count: 0, bytes: 0 },
			redlight: { present: false, valid: false, count: 0, bytes: 0 }
		}
	});

	let recentEvents = $state([]);
	let eventStats = $state({
		count: 0,
		published: 0,
		drops: 0,
		size: 0,
		capacity: 0
	});

	let runtimeConfig = $state({
		gpsEnabled: false,
		cameraEnabled: true
	});

	onMount(async () => {
		await refreshAll();
		pollHandle = setInterval(() => {
			void fetchCameraStatus(true);
			void fetchCameraEvents(true);
		}, STATUS_POLL_INTERVAL_MS);
	});

	onDestroy(() => {
		if (pollHandle) {
			clearInterval(pollHandle);
			pollHandle = null;
		}
	});

	async function refreshAll() {
		refreshing = true;
		await Promise.all([
			fetchCameraStatus(false),
			fetchCameraCatalog(false),
			fetchCameraEvents(false),
			fetchRuntimeConfig(false)
		]);
		loading = false;
		refreshing = false;
	}

	function formatBytes(bytes) {
		if (typeof bytes !== 'number' || !Number.isFinite(bytes) || bytes <= 0) return '0 B';
		if (bytes < 1024) return `${bytes} B`;
		if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
		return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
	}

	function formatTimestamp(tsMs) {
		if (typeof tsMs !== 'number' || tsMs <= 0) return '—';
		return new Date(tsMs).toLocaleString();
	}

	function cameraTypeLabel(rawType) {
		switch (rawType) {
			case 1:
				return 'Red Light';
			case 2:
				return 'Speed';
			case 3:
				return 'Red+Speed';
			case 4:
				return 'ALPR';
			default:
				return `Type ${rawType}`;
		}
	}

	function datasetStatus(dataset) {
		if (!dataset?.present) return 'missing';
		return dataset.valid ? 'ok' : 'invalid';
	}

	async function fetchCameraStatus(silent = false) {
		if (statusFetchInFlight) return;
		statusFetchInFlight = true;
		if (!silent) statusError = '';
		try {
			const res = await fetch('/api/cameras/status');
			if (!res.ok) {
				if (!silent) statusError = 'Failed to load camera runtime status.';
				return;
			}
			const data = await res.json();
			status = {
				...status,
				...data,
				counters: { ...status.counters, ...(data.counters || {}) },
				loader: { ...status.loader, ...(data.loader || {}) },
				index: { ...status.index, ...(data.index || {}) }
			};
		} catch (e) {
			if (!silent) statusError = 'Failed to load camera runtime status.';
		} finally {
			statusFetchInFlight = false;
		}
	}

	async function fetchCameraCatalog(silent = false) {
		if (catalogFetchInFlight) return;
		catalogFetchInFlight = true;
		if (!silent) catalogError = '';
		try {
			const res = await fetch('/api/cameras/catalog');
			const data = await res.json().catch(() => ({}));
			if (!res.ok || data.success === false) {
				if (!silent) catalogError = data.message || 'Failed to load camera dataset catalog.';
				return;
			}
			catalog = {
				...catalog,
				...data,
				datasets: {
					alpr: { ...catalog.datasets.alpr, ...(data.datasets?.alpr || {}) },
					speed: { ...catalog.datasets.speed, ...(data.datasets?.speed || {}) },
					redlight: { ...catalog.datasets.redlight, ...(data.datasets?.redlight || {}) }
				}
			};
		} catch (e) {
			if (!silent) catalogError = 'Failed to load camera dataset catalog.';
		} finally {
			catalogFetchInFlight = false;
		}
	}

	async function fetchCameraEvents(silent = false) {
		if (eventsFetchInFlight) return;
		eventsFetchInFlight = true;
		if (!silent) eventsError = '';
		try {
			const res = await fetch(`/api/cameras/events?limit=${EVENTS_LIMIT}`);
			if (!res.ok) {
				if (!silent) eventsError = 'Failed to load camera events.';
				return;
			}
			const data = await res.json();
			recentEvents = Array.isArray(data.events) ? data.events : [];
			eventStats = {
				count: typeof data.count === 'number' ? data.count : recentEvents.length,
				published: typeof data.published === 'number' ? data.published : 0,
				drops: typeof data.drops === 'number' ? data.drops : 0,
				size: typeof data.size === 'number' ? data.size : recentEvents.length,
				capacity: typeof data.capacity === 'number' ? data.capacity : EVENTS_LIMIT
			};
		} catch (e) {
			if (!silent) eventsError = 'Failed to load camera events.';
		} finally {
			eventsFetchInFlight = false;
		}
	}

	async function fetchRuntimeConfig(silent = false) {
		if (configFetchInFlight) return;
		configFetchInFlight = true;
		if (!silent) configError = '';
		try {
			const res = await fetch('/api/settings');
			if (!res.ok) {
				if (!silent) configError = 'Failed to load runtime camera settings.';
				return;
			}
			const data = await res.json();
			runtimeConfig = {
				gpsEnabled: !!data.gpsEnabled,
				cameraEnabled: data.cameraEnabled === undefined ? true : !!data.cameraEnabled
			};
		} catch (e) {
			if (!silent) configError = 'Failed to load runtime camera settings.';
		} finally {
			configFetchInFlight = false;
		}
	}

	async function saveCameraEnabled(nextEnabled) {
		if (configSaveInFlight) return;
		configSaveInFlight = true;
		configError = '';
		try {
			const payload = new URLSearchParams();
			payload.set('cameraEnabled', nextEnabled ? '1' : '0');
			const res = await fetch('/api/settings', {
				method: 'POST',
				headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
				body: payload.toString()
			});
			if (!res.ok) {
				let errorMsg = 'Failed to save camera runtime setting.';
				const data = await res.json().catch(() => null);
				if (data?.error) {
					errorMsg = data.error;
				}
				configError = errorMsg;
				return;
			}
			await Promise.all([fetchRuntimeConfig(true), fetchCameraStatus(true)]);
		} catch (e) {
			configError = 'Failed to save camera runtime setting.';
		} finally {
			configSaveInFlight = false;
		}
	}
</script>

<div class="space-y-6">
	<div class="flex flex-wrap items-center justify-between gap-3">
		<div>
			<h1 class="text-2xl font-bold">Cameras</h1>
			<p class="text-sm text-base-content/70">Runtime controls, telemetry, and dataset inventory.</p>
		</div>
		<div class="flex items-center gap-2">
			<button class="btn btn-outline btn-sm" onclick={() => fetchCameraCatalog(false)} disabled={catalogFetchInFlight}>
				{catalogFetchInFlight ? 'Refreshing catalog...' : 'Refresh Catalog'}
			</button>
			<button class="btn btn-primary btn-sm" onclick={refreshAll} disabled={refreshing}>
				{refreshing ? 'Refreshing...' : 'Refresh All'}
			</button>
		</div>
	</div>

	{#if statusError}
		<div class="alert alert-error" role="alert"><span>{statusError}</span></div>
	{/if}
	{#if catalogError}
		<div class="alert alert-warning" role="alert"><span>{catalogError}</span></div>
	{/if}
	{#if eventsError}
		<div class="alert alert-warning" role="alert"><span>{eventsError}</span></div>
	{/if}
	{#if configError}
		<div class="alert alert-warning" role="alert"><span>{configError}</span></div>
	{/if}

	<div class="card bg-base-200 shadow">
		<div class="card-body gap-3">
			<div class="flex flex-wrap items-center justify-between gap-3">
				<div>
					<h2 class="card-title">Runtime Status</h2>
					<p class="text-sm text-base-content/70">
						Camera runtime stays lower priority than core BLE/display path.
					</p>
				</div>
				<div class="badge {status.indexLoaded ? 'badge-success' : 'badge-ghost'} badge-lg">
					{status.indexLoaded ? 'Index Loaded' : 'Index Empty'}
				</div>
			</div>
			<div class="bg-base-100 rounded-lg p-3 space-y-2">
				<label class="label cursor-pointer justify-start gap-3">
					<input
						type="checkbox"
						class="toggle toggle-primary"
						checked={runtimeConfig.cameraEnabled}
						onchange={(event) => saveCameraEnabled(event.currentTarget.checked)}
						disabled={configSaveInFlight}
					/>
					<div>
						<div class="font-medium">
							Camera Runtime
							{runtimeConfig.cameraEnabled ? 'Enabled' : 'Disabled'}
						</div>
						<div class="text-xs text-base-content/70">
							Effective state:
							{runtimeConfig.gpsEnabled && runtimeConfig.cameraEnabled ? 'active' : 'inactive'}
							(gated by GPS + camera setting)
						</div>
					</div>
				</label>
				{#if !runtimeConfig.gpsEnabled}
					<div class="text-xs text-warning">
						GPS is disabled. Camera matching remains inactive until GPS is re-enabled.
					</div>
				{/if}
			</div>
			<div class="stats stats-vertical md:stats-horizontal shadow bg-base-100">
				<div class="stat py-3 px-4">
					<div class="stat-title">Runtime</div>
					<div class="stat-value text-base">{status.enabled ? 'Enabled' : 'Disabled'}</div>
					<div class="stat-desc">guarded low-priority tick</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Loaded Records</div>
					<div class="stat-value text-base">{status.index?.cameraCount || 0}</div>
					<div class="stat-desc">active index</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Buckets</div>
					<div class="stat-value text-base">{status.index?.bucketCount || 0}</div>
					<div class="stat-desc">grid spans</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Index Version</div>
					<div class="stat-value text-base">{status.index?.version || 0}</div>
					<div class="stat-desc">ready {status.loader?.readyVersion || 0}</div>
				</div>
			</div>
			<div class="grid grid-cols-1 md:grid-cols-2 gap-3 text-sm">
				<div class="bg-base-100 rounded-lg p-3">
					<div>Tick: {status.lastTickDurationUs || 0} us (max {status.maxTickDurationUs || 0} us)</div>
					<div>Loader: {status.loader?.lastLoadDurationMs || 0} ms (max {status.loader?.maxLoadDurationMs || 0} ms)</div>
					<div>Sort/Spans: {status.loader?.lastSortDurationMs || 0} / {status.loader?.lastSpanBuildDurationMs || 0} ms</div>
				</div>
				<div class="bg-base-100 rounded-lg p-3">
					<div>Tick skips (overload/non-core/mem): {status.counters?.cameraTickSkipsOverload || 0} / {status.counters?.cameraTickSkipsNonCore || 0} / {status.counters?.cameraTickSkipsMemoryGuard || 0}</div>
					<div>Loads (attempts/fail/skips): {status.loader?.loadAttempts || 0} / {status.loader?.loadFailures || 0} / {status.loader?.loadSkipsMemoryGuard || 0}</div>
					<div>Internal SRAM: {status.lastInternalFree || 0} free, {status.lastInternalLargestBlock || 0} block</div>
				</div>
			</div>
		</div>
	</div>

	<div class="card bg-base-200 shadow">
		<div class="card-body gap-3">
			<div class="flex flex-wrap items-center justify-between gap-3">
				<div>
					<h2 class="card-title">Dataset Catalog</h2>
					<p class="text-sm text-base-content/70">
						SD header counts for ALPR, speed, and red light datasets.
					</p>
				</div>
				<div class="text-xs text-base-content/60">last scan: {formatTimestamp(catalog.tsMs)}</div>
			</div>

			<div class="stats stats-vertical md:stats-horizontal shadow bg-base-100">
				<div class="stat py-3 px-4">
					<div class="stat-title">Storage</div>
					<div class="stat-value text-base">{catalog.storageReady ? 'Ready' : 'Unavailable'}</div>
					<div class="stat-desc">{catalog.message || 'SD-backed catalog scan'}</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Catalog Total</div>
					<div class="stat-value text-base">{catalog.totalCount || 0}</div>
					<div class="stat-desc">{formatBytes(catalog.totalBytes || 0)}</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Loaded Index</div>
					<div class="stat-value text-base">{status.index?.cameraCount || 0}</div>
					<div class="stat-desc">enforcement-first in current M2 path</div>
				</div>
			</div>

			<div class="overflow-x-auto">
				<table class="table table-zebra table-sm">
					<thead>
						<tr>
							<th>Dataset</th>
							<th>Status</th>
							<th>Count</th>
							<th>Bytes</th>
						</tr>
					</thead>
					<tbody>
						<tr>
							<td>ALPR</td>
							<td>{datasetStatus(catalog.datasets?.alpr)}</td>
							<td>{catalog.datasets?.alpr?.count || 0}</td>
							<td>{formatBytes(catalog.datasets?.alpr?.bytes || 0)}</td>
						</tr>
						<tr>
							<td>Speed</td>
							<td>{datasetStatus(catalog.datasets?.speed)}</td>
							<td>{catalog.datasets?.speed?.count || 0}</td>
							<td>{formatBytes(catalog.datasets?.speed?.bytes || 0)}</td>
						</tr>
						<tr>
							<td>Red Light</td>
							<td>{datasetStatus(catalog.datasets?.redlight)}</td>
							<td>{catalog.datasets?.redlight?.count || 0}</td>
							<td>{formatBytes(catalog.datasets?.redlight?.bytes || 0)}</td>
						</tr>
					</tbody>
				</table>
			</div>
		</div>
	</div>

	<div class="card bg-base-200 shadow">
		<div class="card-body gap-3">
			<div class="flex flex-wrap items-center justify-between gap-3">
				<div>
					<h2 class="card-title">Recent Events</h2>
					<p class="text-sm text-base-content/70">Recent camera events from bounded runtime ring buffer.</p>
				</div>
				<div class="text-xs text-base-content/60">
					count {eventStats.count} · ring {eventStats.size}/{eventStats.capacity} · drops {eventStats.drops}
				</div>
			</div>

			{#if loading}
				<div class="flex items-center gap-2 text-sm text-base-content/70">
					<span class="loading loading-spinner loading-sm"></span>
					<span>Loading events...</span>
				</div>
			{:else if recentEvents.length === 0}
				<div class="text-sm text-base-content/70">No camera events captured yet.</div>
			{:else}
				<div class="overflow-x-auto">
					<table class="table table-zebra table-sm">
						<thead>
							<tr>
								<th>Timestamp</th>
								<th>Camera ID</th>
								<th>Type</th>
								<th>Distance</th>
								<th>Synthetic</th>
							</tr>
						</thead>
						<tbody>
							{#each recentEvents as sample}
								<tr>
									<td>{formatTimestamp(sample.tsMs)}</td>
									<td>{sample.cameraId}</td>
									<td>{cameraTypeLabel(sample.type)}</td>
									<td>{sample.distanceM} m</td>
									<td>{sample.synthetic ? 'yes' : 'no'}</td>
								</tr>
							{/each}
						</tbody>
					</table>
				</div>
			{/if}
		</div>
	</div>
</div>
