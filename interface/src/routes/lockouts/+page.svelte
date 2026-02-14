<script>
	import { onMount } from 'svelte';

	let loading = $state(true);
	let message = $state(null);
	let statusPoll = $state(null);
	let gpsStatusFetchInFlight = false;
	let lockoutFetchInFlight = false;
	let lockoutZonesFetchInFlight = false;
	let lockoutLoading = $state(false);
	let lockoutError = $state('');
	let lockoutZonesLoading = $state(false);
	let lockoutZonesError = $state('');
	let savingLockoutConfig = $state(false);
	let lockoutConfigInitialized = false;
	let lockoutConfigDirty = $state(false);
	let advancedUnlocked = $state(false);
	let kaPreviewEnabled = $state(false);

	const STATUS_POLL_INTERVAL_MS = 2500;
	const LOCKOUT_EVENTS_LIMIT = 48;
	const LOCKOUT_ZONES_LIMIT = 64;
	const FEET_PER_METER = 3.28084;
	const FALLBACK_PROMOTION_HITS = 3;
	const FALLBACK_FREQ_TOLERANCE_MHZ = 10;
	const FALLBACK_RADIUS_FT = 492;

	let gpsStatus = $state({
		enabled: false,
		runtimeEnabled: false,
		mode: 'scaffold',
		hasFix: false,
		satellites: 0,
		speedMph: null,
		moduleDetected: false,
		detectionTimedOut: false,
		parserActive: false,
		lockout: {
			mode: 'off',
			modeRaw: 0,
			coreGuardEnabled: true,
			maxQueueDrops: 0,
			maxPerfDrops: 0,
			maxEventBusDrops: 0,
			coreGuardTripped: false,
			coreGuardReason: '',
			enforceAllowed: false
		}
	});

	let lockoutEvents = $state([]);
	let lockoutStats = $state({
		published: 0,
		drops: 0,
		size: 0,
		capacity: 0
	});
	let lockoutSd = $state({
		enabled: false,
		path: '',
		enqueued: 0,
		queueDrops: 0,
		deduped: 0,
		written: 0,
		writeFail: 0,
		rotations: 0
	});
	let lockoutConfig = $state({
		modeRaw: 0,
		coreGuardEnabled: true,
		maxQueueDrops: 0,
		maxPerfDrops: 0,
		maxEventBusDrops: 0
	});
	let lockoutZonesStats = $state({
		activeCount: 0,
		activeCapacity: 0,
		activeReturned: 0,
		pendingCount: 0,
		pendingCapacity: 0,
		pendingReturned: 0,
		promotionHits: 0
	});
	let activeLockoutZones = $state([]);
	let pendingLockoutZones = $state([]);

	onMount(async () => {
		await refreshAll();
		statusPoll = setInterval(async () => {
			await fetchGpsStatus();
		}, STATUS_POLL_INTERVAL_MS);
		return () => {
			if (statusPoll) clearInterval(statusPoll);
		};
	});

	function setMsg(type, text) {
		message = { type, text };
	}

	function formatFrequencyMhz(mhz) {
		if (typeof mhz !== 'number' || !Number.isFinite(mhz) || mhz <= 0) return '—';
		return `${Math.round(mhz)} MHz`;
	}

	function formatCoordinate(value) {
		if (typeof value !== 'number' || !Number.isFinite(value)) return '—';
		return value.toFixed(5);
	}

	function formatEpochMs(epochMs) {
		if (typeof epochMs !== 'number' || !Number.isFinite(epochMs) || epochMs <= 0) return '—';
		return new Date(epochMs).toLocaleString();
	}

	function formatFixAgeMs(value) {
		if (typeof value !== 'number' || !Number.isFinite(value)) return '—';
		return `${(value / 1000).toFixed(1)}s`;
	}

	function formatBootTime(tsMs) {
		if (typeof tsMs !== 'number' || !Number.isFinite(tsMs)) return '—';
		return `${(tsMs / 1000).toFixed(1)}s`;
	}

	function formatHdop(value) {
		if (typeof value !== 'number' || !Number.isFinite(value)) return '—';
		return value.toFixed(1);
	}

	function formatBandMask(mask) {
		if (typeof mask !== 'number' || !Number.isFinite(mask)) return '—';
		const parts = [];
		if (mask & 0x01) parts.push('Laser');
		if (mask & 0x02) parts.push('Ka');
		if (mask & 0x04) parts.push('K');
		if (mask & 0x08) parts.push('X');
		return parts.length > 0 ? parts.join('+') : '—';
	}

	function formatRadiusFeet(radiusM) {
		if (typeof radiusM !== 'number' || !Number.isFinite(radiusM) || radiusM <= 0) return '—';
		return `${Math.round(radiusM * FEET_PER_METER)} ft`;
	}

	function signalMapHref(event) {
		if (!event?.locationValid) return '';
		if (typeof event.latitude !== 'number' || typeof event.longitude !== 'number') return '';
		return `https://maps.google.com/?q=${event.latitude},${event.longitude}`;
	}

	function clampU16(value) {
		const parsed = Number(value);
		if (!Number.isFinite(parsed)) return 0;
		return Math.max(0, Math.min(65535, Math.round(parsed)));
	}

	function applyLockoutStatus(data) {
		const lockout = data?.lockout;
		if (!lockout) return;
		if (lockoutConfigInitialized && lockoutConfigDirty) return;
		lockoutConfig = {
			modeRaw: typeof lockout.modeRaw === 'number' ? lockout.modeRaw : 0,
			coreGuardEnabled: !!lockout.coreGuardEnabled,
			maxQueueDrops: typeof lockout.maxQueueDrops === 'number' ? lockout.maxQueueDrops : 0,
			maxPerfDrops: typeof lockout.maxPerfDrops === 'number' ? lockout.maxPerfDrops : 0,
			maxEventBusDrops: typeof lockout.maxEventBusDrops === 'number' ? lockout.maxEventBusDrops : 0
		};
		lockoutConfigInitialized = true;
	}

	function markLockoutDirty() {
		lockoutConfigDirty = true;
	}

	function defaultRadiusFeet() {
		const fromZone = activeLockoutZones.find(
			(zone) => typeof zone?.radiusM === 'number' && Number.isFinite(zone.radiusM)
		);
		if (fromZone) {
			return Math.round(fromZone.radiusM * FEET_PER_METER);
		}
		return FALLBACK_RADIUS_FT;
	}

	function defaultFreqToleranceMhz() {
		const fromZone = activeLockoutZones.find(
			(zone) =>
				typeof zone?.frequencyToleranceMHz === 'number' &&
				Number.isFinite(zone.frequencyToleranceMHz)
		);
		if (fromZone) {
			return Math.round(fromZone.frequencyToleranceMHz);
		}
		return FALLBACK_FREQ_TOLERANCE_MHZ;
	}

	async function refreshAll() {
		await Promise.all([fetchGpsStatus(), fetchLockoutEvents(), fetchLockoutZones()]);
		loading = false;
	}

	async function fetchGpsStatus() {
		if (gpsStatusFetchInFlight) return;
		gpsStatusFetchInFlight = true;
		try {
			const res = await fetch('/api/gps/status');
			if (!res.ok) return;
			const data = await res.json();
			gpsStatus = { ...gpsStatus, ...data };
			applyLockoutStatus(data);
		} catch (e) {
			// Polling should fail silently.
		} finally {
			gpsStatusFetchInFlight = false;
		}
	}

	async function fetchLockoutEvents(options = {}) {
		const { silent = false } = options;
		if (lockoutFetchInFlight) return;
		lockoutFetchInFlight = true;
		if (!silent) {
			lockoutLoading = true;
		}
		lockoutError = '';
		try {
			const res = await fetch(`/api/lockouts/events?limit=${LOCKOUT_EVENTS_LIMIT}`);
			if (!res.ok) {
				if (!silent) lockoutError = 'Failed to load lockout candidates';
				return;
			}
			const data = await res.json();
			lockoutEvents = Array.isArray(data.events) ? data.events : [];
			lockoutStats = {
				published: typeof data.published === 'number' ? data.published : 0,
				drops: typeof data.drops === 'number' ? data.drops : 0,
				size: typeof data.size === 'number' ? data.size : lockoutEvents.length,
				capacity: typeof data.capacity === 'number' ? data.capacity : lockoutStats.capacity
			};
			lockoutSd = {
				enabled: !!data?.sd?.enabled,
				path: typeof data?.sd?.path === 'string' ? data.sd.path : '',
				enqueued: typeof data?.sd?.enqueued === 'number' ? data.sd.enqueued : 0,
				queueDrops: typeof data?.sd?.queueDrops === 'number' ? data.sd.queueDrops : 0,
				deduped: typeof data?.sd?.deduped === 'number' ? data.sd.deduped : 0,
				written: typeof data?.sd?.written === 'number' ? data.sd.written : 0,
				writeFail: typeof data?.sd?.writeFail === 'number' ? data.sd.writeFail : 0,
				rotations: typeof data?.sd?.rotations === 'number' ? data.sd.rotations : 0
			};
		} catch (e) {
			if (!silent) lockoutError = 'Failed to load lockout candidates';
		} finally {
			lockoutFetchInFlight = false;
			lockoutLoading = false;
		}
	}

	async function fetchLockoutZones(options = {}) {
		const { silent = false } = options;
		if (lockoutZonesFetchInFlight) return;
		lockoutZonesFetchInFlight = true;
		if (!silent) lockoutZonesLoading = true;
		lockoutZonesError = '';
		try {
			const res = await fetch(
				`/api/lockouts/zones?activeLimit=${LOCKOUT_ZONES_LIMIT}&pendingLimit=${LOCKOUT_ZONES_LIMIT}`
			);
			if (!res.ok) {
				if (!silent) lockoutZonesError = 'Failed to load lockout zones';
				return;
			}
			const data = await res.json();
			activeLockoutZones = Array.isArray(data.activeZones) ? data.activeZones : [];
			pendingLockoutZones = Array.isArray(data.pendingZones) ? data.pendingZones : [];
			lockoutZonesStats = {
				activeCount: typeof data.activeCount === 'number' ? data.activeCount : 0,
				activeCapacity: typeof data.activeCapacity === 'number' ? data.activeCapacity : 0,
				activeReturned:
					typeof data.activeReturned === 'number' ? data.activeReturned : activeLockoutZones.length,
				pendingCount: typeof data.pendingCount === 'number' ? data.pendingCount : 0,
				pendingCapacity: typeof data.pendingCapacity === 'number' ? data.pendingCapacity : 0,
				pendingReturned:
					typeof data.pendingReturned === 'number' ? data.pendingReturned : pendingLockoutZones.length,
				promotionHits: typeof data.promotionHits === 'number' ? data.promotionHits : 0
			};
		} catch (e) {
			if (!silent) lockoutZonesError = 'Failed to load lockout zones';
		} finally {
			lockoutZonesFetchInFlight = false;
			lockoutZonesLoading = false;
		}
	}

	async function saveLockoutConfig() {
		if (!advancedUnlocked) {
			setMsg('error', 'Unlock advanced controls before applying lockout changes.');
			return;
		}
		if (!confirm('Apply lockout runtime changes? Incorrect values can suppress real alerts.')) {
			return;
		}
		if (savingLockoutConfig) return;
		savingLockoutConfig = true;
		try {
			const modeRaw = Math.max(0, Math.min(3, Number(lockoutConfig.modeRaw) || 0));
			const payload = {
				lockoutMode: modeRaw,
				lockoutCoreGuardEnabled: !!lockoutConfig.coreGuardEnabled,
				lockoutMaxQueueDrops: clampU16(lockoutConfig.maxQueueDrops),
				lockoutMaxPerfDrops: clampU16(lockoutConfig.maxPerfDrops),
				lockoutMaxEventBusDrops: clampU16(lockoutConfig.maxEventBusDrops)
			};
			const res = await fetch('/api/gps/config', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify(payload)
			});
			const data = await res.json().catch(() => ({}));
			if (!res.ok) {
				setMsg('error', data.message || 'Failed to update lockout settings');
				return;
			}
			lockoutConfigDirty = false;
			setMsg('success', 'Lockout runtime settings updated');
			await Promise.all([fetchGpsStatus(), fetchLockoutZones({ silent: true })]);
		} catch (e) {
			setMsg('error', 'Failed to update lockout settings');
		} finally {
			savingLockoutConfig = false;
		}
	}
</script>

<div class="space-y-6">
	<div class="flex flex-wrap items-start justify-between gap-3">
		<div>
			<h1 class="text-2xl font-bold">Lockouts</h1>
			<p class="text-sm text-base-content/70">
				Dedicated lockout controls and observability. Phase 1 keeps lockout algorithm behavior unchanged.
			</p>
		</div>
		<div class="flex gap-2">
			<a href="/integrations" class="btn btn-outline btn-sm">OBD & GPS</a>
			<button class="btn btn-outline btn-sm" onclick={refreshAll}>Refresh All</button>
		</div>
	</div>

	{#if message}
		<div
			class="alert alert-{message.type === 'error' ? 'error' : message.type === 'success' ? 'success' : 'info'}"
			role="status"
			aria-live="polite"
		>
			<span>{message.text}</span>
		</div>
	{/if}

	<div class="card bg-base-200 shadow">
		<div class="card-body gap-3">
			<div class="flex flex-wrap items-center justify-between gap-3">
				<div>
					<h2 class="card-title">Safety Gate</h2>
					<p class="text-sm text-base-content/70">
						Advanced lockout writes stay disabled until explicitly unlocked for this session.
					</p>
				</div>
				<label class="label cursor-pointer justify-start gap-3 py-0">
					<span class="label-text">Unlock advanced writes</span>
					<input
						type="checkbox"
						class="toggle toggle-warning"
						checked={advancedUnlocked}
						onchange={(e) => {
							advancedUnlocked = e.currentTarget.checked;
						}}
					/>
				</label>
			</div>
			<div class="text-xs text-base-content/70">
				Use caution in `Enforce` mode. Bad lockout settings can mute real threats.
			</div>
		</div>
	</div>

	<div class="card bg-base-200 shadow">
		<div class="card-body gap-3">
			<div class="flex flex-wrap items-center justify-between gap-3">
				<div>
					<h2 class="card-title">Lockout Runtime Controls</h2>
					<p class="text-sm text-base-content/70">
						Live runtime controls currently available in firmware.
					</p>
				</div>
				<div class="flex gap-2">
					<button class="btn btn-outline btn-sm" onclick={() => fetchGpsStatus()}>Reload</button>
					<button
						class="btn btn-primary btn-sm"
						onclick={saveLockoutConfig}
						disabled={!advancedUnlocked || savingLockoutConfig || !lockoutConfigDirty}
					>
						{#if savingLockoutConfig}
							<span class="loading loading-spinner loading-xs"></span>
						{/if}
						Save
					</button>
				</div>
			</div>

			<div class="grid grid-cols-1 md:grid-cols-2 gap-3">
				<label class="form-control">
					<span class="label-text text-sm">Mode</span>
					<select
						class="select select-bordered select-sm"
						bind:value={lockoutConfig.modeRaw}
						onchange={markLockoutDirty}
						disabled={!advancedUnlocked}
					>
						<option value={0}>Off</option>
						<option value={1}>Shadow (read-only)</option>
						<option value={2}>Advisory (read-only)</option>
						<option value={3}>Enforce (risk: can mute alerts)</option>
					</select>
				</label>
				<label class="label cursor-pointer justify-start gap-3 py-0">
					<span class="label-text text-sm">Core guard</span>
					<input
						type="checkbox"
						class="toggle toggle-primary toggle-sm"
						checked={!!lockoutConfig.coreGuardEnabled}
						disabled={!advancedUnlocked}
						onchange={(e) => {
							lockoutConfig.coreGuardEnabled = e.currentTarget.checked;
							markLockoutDirty();
						}}
					/>
				</label>
				<label class="form-control">
					<span class="label-text text-sm">Max queue drops</span>
					<input
						type="number"
						min="0"
						max="65535"
						class="input input-bordered input-sm"
						value={lockoutConfig.maxQueueDrops}
						disabled={!advancedUnlocked}
						onchange={(e) => {
							lockoutConfig.maxQueueDrops = clampU16(e.currentTarget.value);
							markLockoutDirty();
						}}
					/>
				</label>
				<label class="form-control">
					<span class="label-text text-sm">Max perf drops</span>
					<input
						type="number"
						min="0"
						max="65535"
						class="input input-bordered input-sm"
						value={lockoutConfig.maxPerfDrops}
						disabled={!advancedUnlocked}
						onchange={(e) => {
							lockoutConfig.maxPerfDrops = clampU16(e.currentTarget.value);
							markLockoutDirty();
						}}
					/>
				</label>
				<label class="form-control">
					<span class="label-text text-sm">Max event-bus drops</span>
					<input
						type="number"
						min="0"
						max="65535"
						class="input input-bordered input-sm"
						value={lockoutConfig.maxEventBusDrops}
						disabled={!advancedUnlocked}
						onchange={(e) => {
							lockoutConfig.maxEventBusDrops = clampU16(e.currentTarget.value);
							markLockoutDirty();
						}}
					/>
				</label>
			</div>

			<div class="text-xs text-base-content/65">
				Current mode: {gpsStatus?.lockout?.mode || 'off'} · enforce allowed:{' '}
				{gpsStatus?.lockout?.enforceAllowed ? 'yes' : 'no'} · core guard:{' '}
				{gpsStatus?.lockout?.coreGuardTripped ? 'tripped' : 'clear'}
				{#if gpsStatus?.lockout?.coreGuardReason}
					· reason: {gpsStatus.lockout.coreGuardReason}
				{/if}
			</div>
		</div>
	</div>

	<div class="card bg-base-200 shadow">
		<div class="card-body gap-3">
			<div>
				<h2 class="card-title">Learner Settings (Phase 1)</h2>
				<p class="text-sm text-base-content/70">
					Displayed for visibility only in this phase. Firmware lockout-learning constants are not writable yet.
				</p>
			</div>
			<div class="stats stats-vertical md:stats-horizontal shadow bg-base-100">
				<div class="stat py-3 px-4">
					<div class="stat-title">Hits to Promote</div>
					<div class="stat-value text-base">
						{lockoutZonesStats.promotionHits || FALLBACK_PROMOTION_HITS}
					</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Drift Tolerance</div>
					<div class="stat-value text-base">±{defaultFreqToleranceMhz()} MHz</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Lockout Radius</div>
					<div class="stat-value text-base">{defaultRadiusFeet()} ft</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Candidate Expiry</div>
					<div class="stat-value text-base">7 days</div>
				</div>
			</div>
			<label class="label cursor-pointer justify-start gap-3 py-0">
				<span class="label-text text-sm">Ka lockout learning (preview only)</span>
				<input
					type="checkbox"
					class="toggle toggle-warning toggle-sm"
					checked={kaPreviewEnabled}
					disabled
					onchange={(e) => {
						kaPreviewEnabled = e.currentTarget.checked;
					}}
				/>
			</label>
			<div class="text-xs text-warning">
				Ka learning remains disabled in firmware by policy. If enabled later, it will ship behind a warning gate.
			</div>
		</div>
	</div>

	<div class="card bg-base-200 shadow">
		<div class="card-body gap-3">
			<div class="flex flex-wrap items-center justify-between gap-3">
				<div>
					<h2 class="card-title">Lockout Zones</h2>
					<p class="text-sm text-base-content/70">
						Read-only snapshot of active lockouts and pending learner candidates.
					</p>
				</div>
				<button class="btn btn-outline btn-sm" onclick={() => fetchLockoutZones()} disabled={lockoutZonesLoading}>
					{#if lockoutZonesLoading}
						<span class="loading loading-spinner loading-xs"></span>
					{/if}
					Refresh
				</button>
			</div>

			{#if lockoutZonesError}
				<div class="alert alert-warning py-2" role="status">
					<span>{lockoutZonesError}</span>
				</div>
			{/if}

			<div class="stats stats-vertical md:stats-horizontal shadow bg-base-100">
				<div class="stat py-3 px-4">
					<div class="stat-title">Active</div>
					<div class="stat-value text-base">
						{lockoutZonesStats.activeReturned}/{lockoutZonesStats.activeCount}
					</div>
					<div class="stat-desc">showing/total zones</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Pending</div>
					<div class="stat-value text-base">
						{lockoutZonesStats.pendingReturned}/{lockoutZonesStats.pendingCount}
					</div>
					<div class="stat-desc">showing/total candidates</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Promotion Hits</div>
					<div class="stat-value text-base">{lockoutZonesStats.promotionHits || '—'}</div>
					<div class="stat-desc">candidate threshold</div>
				</div>
			</div>

			{#if lockoutZonesLoading}
				<div class="flex justify-center p-6"><span class="loading loading-spinner loading-md"></span></div>
			{:else}
				<div class="grid grid-cols-1 xl:grid-cols-2 gap-4">
					<div class="overflow-x-auto">
						<div class="text-sm font-medium mb-2">Active Zones</div>
						{#if activeLockoutZones.length === 0}
							<div class="text-sm text-base-content/70">No active lockout zones.</div>
						{:else}
							<table class="table table-sm">
								<thead>
									<tr>
										<th>Slot</th>
										<th>Source</th>
										<th>Band</th>
										<th>Freq</th>
										<th>Conf</th>
										<th>Radius</th>
										<th>Location</th>
									</tr>
								</thead>
								<tbody>
									{#each activeLockoutZones as zone}
										<tr>
											<td class="font-mono text-xs">{zone.slot}</td>
											<td class="text-xs">
												{zone.manual && zone.learned
													? 'manual+learned'
													: zone.manual
														? 'manual'
														: zone.learned
															? 'learned'
															: 'active'}
											</td>
											<td>{formatBandMask(zone.bandMask)}</td>
											<td>{formatFrequencyMhz(zone.frequencyMHz)}</td>
											<td>{typeof zone.confidence === 'number' ? zone.confidence : '—'}</td>
											<td>{formatRadiusFeet(zone.radiusM)}</td>
											<td>
												<div class="font-mono text-xs">
													{formatCoordinate(zone.latitude)}, {formatCoordinate(zone.longitude)}
												</div>
												<a
													class="link link-primary text-xs"
													href={`https://maps.google.com/?q=${zone.latitude},${zone.longitude}`}
													target="_blank"
													rel="noopener noreferrer"
												>
													map
												</a>
											</td>
										</tr>
									{/each}
								</tbody>
							</table>
						{/if}
					</div>

					<div class="overflow-x-auto">
						<div class="text-sm font-medium mb-2">Pending Candidates</div>
						{#if pendingLockoutZones.length === 0}
							<div class="text-sm text-base-content/70">No pending candidates.</div>
						{:else}
							<table class="table table-sm">
								<thead>
									<tr>
										<th>Slot</th>
										<th>Band</th>
										<th>Freq</th>
										<th>Hits</th>
										<th>Remaining</th>
										<th>Last Seen</th>
										<th>Location</th>
									</tr>
								</thead>
								<tbody>
									{#each pendingLockoutZones as zone}
										<tr>
											<td class="font-mono text-xs">{zone.slot}</td>
											<td>{zone.band || 'UNK'}</td>
											<td>{formatFrequencyMhz(zone.frequencyMHz)}</td>
											<td>{typeof zone.hitCount === 'number' ? zone.hitCount : '—'}</td>
											<td>{typeof zone.hitsRemaining === 'number' ? zone.hitsRemaining : '—'}</td>
											<td class="text-xs">{formatEpochMs(zone.lastSeenMs)}</td>
											<td>
												<div class="font-mono text-xs">
													{formatCoordinate(zone.latitude)}, {formatCoordinate(zone.longitude)}
												</div>
												<a
													class="link link-primary text-xs"
													href={`https://maps.google.com/?q=${zone.latitude},${zone.longitude}`}
													target="_blank"
													rel="noopener noreferrer"
												>
													map
												</a>
											</td>
										</tr>
									{/each}
								</tbody>
							</table>
						{/if}
					</div>
				</div>
			{/if}
		</div>
	</div>

	<div class="card bg-base-200 shadow">
		<div class="card-body gap-3">
			<div class="flex flex-wrap items-center justify-between gap-3">
				<div>
					<h2 class="card-title">Lockout Candidates</h2>
					<p class="text-sm text-base-content/70">
						Recent signal observations for post-drive lockout review.
					</p>
				</div>
				<button class="btn btn-outline btn-sm" onclick={() => fetchLockoutEvents()} disabled={lockoutLoading}>
					{#if lockoutLoading}
						<span class="loading loading-spinner loading-xs"></span>
					{/if}
					Refresh
				</button>
			</div>

			{#if lockoutError}
				<div class="alert alert-warning py-2" role="status">
					<span>{lockoutError}</span>
				</div>
			{/if}

			<div class="stats stats-vertical md:stats-horizontal shadow bg-base-100">
				<div class="stat py-3 px-4">
					<div class="stat-title">Buffer</div>
					<div class="stat-value text-base">
						{lockoutStats.size}/{lockoutStats.capacity || '—'}
					</div>
					<div class="stat-desc">recent observations</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Published</div>
					<div class="stat-value text-base">{lockoutStats.published}</div>
					<div class="stat-desc">since boot</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Drops</div>
					<div class="stat-value text-base">{lockoutStats.drops}</div>
					<div class="stat-desc">oldest overwritten</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Latest</div>
					<div class="stat-value text-base">
						{lockoutEvents[0]?.band || '—'}
					</div>
					<div class="stat-desc">
						{lockoutEvents[0]
							? `${formatFrequencyMhz(lockoutEvents[0].frequencyMHz)} • fix age ${formatFixAgeMs(lockoutEvents[0].fixAgeMs)}`
							: 'no samples yet'}
					</div>
				</div>
			</div>

			<div class="text-xs text-base-content/65">
				SD {lockoutSd.enabled ? 'enabled' : 'disabled'} · writes {lockoutSd.written} · deduped {lockoutSd.deduped}
				· queue drops {lockoutSd.queueDrops} · write fail {lockoutSd.writeFail} · rotations {lockoutSd.rotations}
				{#if lockoutSd.path}
					· <span class="font-mono">{lockoutSd.path}</span>
				{/if}
			</div>

			{#if loading || lockoutLoading}
				<div class="flex justify-center p-6"><span class="loading loading-spinner loading-md"></span></div>
			{:else if lockoutEvents.length === 0}
				<div class="text-sm text-base-content/70">
					No candidates logged yet. Run a drive test, then refresh this card.
				</div>
			{:else}
				<div class="overflow-x-auto">
					<table class="table table-sm">
						<thead>
							<tr>
								<th>Seen (boot)</th>
								<th>Band</th>
								<th>Frequency</th>
								<th>Strength</th>
								<th>Fix Age</th>
								<th>Sats</th>
								<th>HDOP</th>
								<th>Location</th>
							</tr>
						</thead>
						<tbody>
							{#each lockoutEvents as event}
								<tr>
									<td class="font-mono text-xs">{formatBootTime(event.tsMs)}</td>
									<td>{event.band || 'UNK'}</td>
									<td>{formatFrequencyMhz(event.frequencyMHz)}</td>
									<td>{typeof event.strength === 'number' ? event.strength : '—'}</td>
									<td>{formatFixAgeMs(event.fixAgeMs)}</td>
									<td>{typeof event.satellites === 'number' ? event.satellites : '—'}</td>
									<td>{formatHdop(event.hdop)}</td>
									<td>
										{#if event.locationValid}
											<div class="font-mono text-xs">
												{formatCoordinate(event.latitude)}, {formatCoordinate(event.longitude)}
											</div>
											{#if signalMapHref(event)}
												<a class="link link-primary text-xs" href={signalMapHref(event)} target="_blank" rel="noopener noreferrer">
													map
												</a>
											{/if}
										{:else}
											<span class="text-xs text-base-content/60">no fix</span>
										{/if}
									</td>
								</tr>
							{/each}
						</tbody>
					</table>
				</div>
			{/if}
		</div>
	</div>
</div>
