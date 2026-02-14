<script>
	import { onMount } from 'svelte';

	let loading = $state(true);
	let scanning = $state(false);
	let connecting = $state(false);
	let message = $state(null);
	let statusPoll = $state(null);
	let scanPoll = $state(null);
	let statusFetchInFlight = false;
	let gpsStatusFetchInFlight = false;
	let nearbyFetchInFlight = false;
	let lockoutFetchInFlight = false;
	let lockoutZonesFetchInFlight = false;
	let savingVwData = $state(false);
	let savingGpsEnabled = $state(false);
	let lockoutLoading = $state(false);
	let lockoutError = $state('');
	let lockoutZonesLoading = $state(false);
	let lockoutZonesError = $state('');
	let savingLockoutConfig = $state(false);
	let lockoutConfigInitialized = false;
	let lockoutConfigDirty = $state(false);

	const STATUS_POLL_INTERVAL_MS = 2500;
	const SCAN_POLL_INTERVAL_MS = 1500;
	const LOCKOUT_EVENTS_LIMIT = 48;
	const LOCKOUT_ZONES_LIMIT = 64;

	let status = $state({
		state: 'IDLE',
		connected: false,
		scanning: false,
		deviceName: '',
		deviceAddress: '',
		v1Connected: false,
		hasValidData: false,
		speedMph: 0,
		speedKph: 0,
		rpm: 0,
		voltage: 0,
		sampleTsMs: 0,
		sampleAgeMs: 0,
		vwDataEnabled: true,
		oilTempC: null,
		intakeAirTempC: null,
		rememberedCount: 0,
		autoConnectCount: 0
	});
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

	let nearby = $state([]);
	let remembered = $state([]);
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

	let showPinModal = $state(false);
	let selectedDevice = $state(null);
	let pin = $state('');
	let rememberDevice = $state(true);
	let autoConnect = $state(true);

	onMount(async () => {
		await refreshAll();
		statusPoll = setInterval(async () => {
			await Promise.all([fetchStatus(), fetchGpsStatus()]);
			if (!status.scanning) {
				scanning = false;
				stopScanPoll();
			}
		}, STATUS_POLL_INTERVAL_MS);

		return () => {
			if (statusPoll) clearInterval(statusPoll);
			stopScanPoll();
		};
	});

	function stopScanPoll() {
		if (scanPoll) {
			clearInterval(scanPoll);
			scanPoll = null;
		}
	}

	function setMsg(type, text) {
		message = { type, text };
	}

	function getStateBadge(state) {
		switch (state) {
			case 'POLLING':
			case 'READY':
				return 'badge-success';
			case 'SCANNING':
			case 'CONNECTING':
			case 'INITIALIZING':
				return 'badge-warning';
			case 'FAILED':
				return 'badge-error';
			default:
				return 'badge-ghost';
		}
	}

	function formatTemp(value) {
		if (typeof value !== 'number') return '—';
		const f = Math.round(value * 9 / 5 + 32);
		return `${f}°F`;
	}

	function formatBootTime(tsMs) {
		if (typeof tsMs !== 'number' || !Number.isFinite(tsMs)) return '—';
		return `${(tsMs / 1000).toFixed(1)}s`;
	}

	function formatFrequency(mhz) {
		if (typeof mhz !== 'number' || !Number.isFinite(mhz) || mhz <= 0) return '—';
		if (mhz >= 1000) return `${(mhz / 1000).toFixed(3)} GHz`;
		return `${mhz.toFixed(1)} MHz`;
	}

	function formatCoordinate(value) {
		if (typeof value !== 'number' || !Number.isFinite(value)) return '—';
		return value.toFixed(5);
	}

	function formatHdop(value) {
		if (typeof value !== 'number' || !Number.isFinite(value)) return '—';
		return value.toFixed(1);
	}

	function formatFixAgeMs(value) {
		if (typeof value !== 'number' || !Number.isFinite(value)) return '—';
		return `${(value / 1000).toFixed(1)}s`;
	}

	function signalMapHref(event) {
		if (!event?.locationValid) return '';
		if (typeof event.latitude !== 'number' || typeof event.longitude !== 'number') return '';
		return `https://maps.google.com/?q=${event.latitude},${event.longitude}`;
	}

	function formatEpochMs(epochMs) {
		if (typeof epochMs !== 'number' || !Number.isFinite(epochMs) || epochMs <= 0) return '—';
		return new Date(epochMs).toLocaleString();
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

	async function refreshAll() {
		await Promise.all([
			fetchStatus(),
			fetchNearby(),
			fetchRemembered(),
			fetchGpsStatus(),
			fetchLockoutEvents(),
			fetchLockoutZones()
		]);
		loading = false;
	}

	async function fetchStatus() {
		if (statusFetchInFlight) return;
		statusFetchInFlight = true;
		try {
			const res = await fetch('/api/obd/status');
			if (!res.ok) return;
			const data = await res.json();
			status = { ...status, ...data };
			scanning = !!data.scanning;
		} catch (e) {
			// Polling should fail silently.
		} finally {
			statusFetchInFlight = false;
		}
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
				activeReturned: typeof data.activeReturned === 'number' ? data.activeReturned : activeLockoutZones.length,
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

	async function fetchNearby() {
		if (nearbyFetchInFlight) return;
		nearbyFetchInFlight = true;
		try {
			const res = await fetch('/api/obd/devices');
			if (!res.ok) return;
			const data = await res.json();
			nearby = data.devices || [];
			scanning = !!data.scanning;
		} catch (e) {
			// ignore
		} finally {
			nearbyFetchInFlight = false;
		}
	}

	async function fetchRemembered() {
		try {
			const res = await fetch('/api/obd/remembered');
			if (!res.ok) return;
			const data = await res.json();
			remembered = data.devices || [];
		} catch (e) {
			// ignore
		}
	}

	async function startScan() {
		if (!status.v1Connected) {
			setMsg('error', 'Connect V1 first, then run OBD scan.');
			return;
		}

		setMsg('info', 'Scanning for nearby BLE devices...');
		scanning = true;
		nearby = [];

		try {
			const res = await fetch('/api/obd/scan', { method: 'POST' });
			const data = await res.json().catch(() => ({}));
			if (!res.ok) {
				setMsg('error', data.message || 'Failed to start scan');
				scanning = false;
				return;
			}

			stopScanPoll();
			scanPoll = setInterval(async () => {
				await fetchNearby();
				if (!scanning) {
					stopScanPoll();
				}
			}, SCAN_POLL_INTERVAL_MS);
		} catch (e) {
			setMsg('error', 'Failed to start scan');
			scanning = false;
		}
	}

	async function stopScan() {
		try {
			await fetch('/api/obd/scan/stop', { method: 'POST' });
		} finally {
			scanning = false;
			stopScanPoll();
			await Promise.all([fetchNearby(), fetchStatus()]);
		}
	}

	function openConnectModal(device) {
		selectedDevice = device;
		pin = '';
		rememberDevice = true;
		autoConnect = true;
		showPinModal = true;
	}

	function closeConnectModal() {
		showPinModal = false;
		selectedDevice = null;
		pin = '';
	}

	async function connectSelectedDevice() {
		if (!selectedDevice || connecting) return;
		connecting = true;

		try {
			const body = {
				address: selectedDevice.address,
				name: selectedDevice.name || selectedDevice.address,
				pin,
				remember: rememberDevice,
				autoConnect: rememberDevice ? autoConnect : false
			};

			const res = await fetch('/api/obd/connect', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify(body)
			});
			const data = await res.json().catch(() => ({}));

			if (!res.ok) {
				setMsg('error', data.message || 'Failed to queue OBD connection');
				return;
			}

			setMsg('success', `Connecting to ${selectedDevice.name || selectedDevice.address}...`);
			closeConnectModal();
			await Promise.all([fetchStatus(), fetchRemembered()]);
		} catch (e) {
			setMsg('error', 'Connection request failed');
		} finally {
			connecting = false;
		}
	}

	async function connectRememberedDevice(device) {
		openConnectModal(device);
		autoConnect = device.autoConnect;
	}

	async function toggleRememberedAutoConnect(device, enabled) {
		try {
			const res = await fetch('/api/obd/remembered/autoconnect', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ address: device.address, enabled })
			});
			if (!res.ok) {
				setMsg('error', 'Failed to update auto-connect');
				await fetchRemembered();
				return;
			}
			await Promise.all([fetchRemembered(), fetchStatus()]);
		} catch (e) {
			setMsg('error', 'Failed to update auto-connect');
		}
	}

	async function forgetRememberedDevice(device) {
		const confirmed = confirm(`Forget ${device.name || device.address}?`);
		if (!confirmed) return;

		try {
			const res = await fetch('/api/obd/forget', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ address: device.address })
			});
			if (!res.ok) {
				setMsg('error', 'Failed to forget device');
				return;
			}
			setMsg('success', 'Remembered device removed');
			await Promise.all([fetchRemembered(), fetchStatus()]);
		} catch (e) {
			setMsg('error', 'Failed to forget device');
		}
	}

	async function disconnectObd() {
		try {
			await fetch('/api/obd/disconnect', { method: 'POST' });
			setMsg('info', 'OBD disconnected');
			await fetchStatus();
		} catch (e) {
			setMsg('error', 'Failed to disconnect OBD');
		}
	}

	async function clearNearby() {
		try {
			await fetch('/api/obd/devices/clear', { method: 'POST' });
			nearby = [];
		} catch (e) {
			// ignore
		}
	}

	async function toggleVwData(enabled) {
		if (savingVwData) return;
		const previous = !!status.vwDataEnabled;
		status = { ...status, vwDataEnabled: enabled };
		savingVwData = true;

		try {
			const res = await fetch('/api/obd/config', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ vwDataEnabled: enabled })
			});
			const data = await res.json().catch(() => ({}));
			if (!res.ok) {
				setMsg('error', data.message || 'Failed to update VW data setting');
				status = { ...status, vwDataEnabled: previous };
				return;
			}

			status = { ...status, vwDataEnabled: !!data.vwDataEnabled };
			setMsg('success', `VW data ${enabled ? 'enabled' : 'disabled'}`);
		} catch (e) {
			status = { ...status, vwDataEnabled: previous };
			setMsg('error', 'Failed to update VW data setting');
		} finally {
			savingVwData = false;
		}
	}

	async function toggleGpsEnabled(enabled) {
		if (savingGpsEnabled) return;
		const previous = !!gpsStatus.enabled;
		gpsStatus = { ...gpsStatus, enabled };
		savingGpsEnabled = true;

		try {
			const res = await fetch('/api/gps/config', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ enabled })
			});
			const data = await res.json().catch(() => ({}));
			if (!res.ok) {
				setMsg('error', data.message || 'Failed to update GPS setting');
				gpsStatus = { ...gpsStatus, enabled: previous };
				return;
			}

			setMsg('success', `GPS ${enabled ? 'enabled' : 'disabled'}`);
			await fetchGpsStatus();
		} catch (e) {
			gpsStatus = { ...gpsStatus, enabled: previous };
			setMsg('error', 'Failed to update GPS setting');
		} finally {
			savingGpsEnabled = false;
		}
	}

	async function saveLockoutConfig() {
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
	<div class="flex flex-wrap items-center justify-between gap-3">
		<h1 class="text-2xl font-bold">Integrations</h1>
		<div class="badge {getStateBadge(status.state)} badge-lg">{status.state}</div>
	</div>

	{#if message}
		<div class="alert alert-{message.type === 'error' ? 'error' : message.type === 'success' ? 'success' : 'info'}" role="status" aria-live="polite">
			<span>{message.text}</span>
		</div>
	{/if}

	<div class="card bg-base-200 shadow">
		<div class="card-body gap-3">
			<div class="flex flex-wrap items-center justify-between gap-3">
				<div>
					<h2 class="card-title">GPS Runtime</h2>
					<p class="text-sm text-base-content/70">
						Use GPS as fallback speed source when OBD is not connected.
					</p>
				</div>
				<label class="label cursor-pointer justify-start gap-3 py-0">
					<span class="label-text">Enabled</span>
					<input
						type="checkbox"
						class="toggle toggle-primary"
						checked={!!gpsStatus.enabled}
						onchange={(e) => toggleGpsEnabled(e.currentTarget.checked)}
						disabled={savingGpsEnabled}
					/>
				</label>
			</div>

			<div class="stats stats-vertical md:stats-horizontal shadow bg-base-100">
				<div class="stat py-3 px-4">
					<div class="stat-title">Mode</div>
					<div class="stat-value text-base">{gpsStatus.mode || 'scaffold'}</div>
					<div class="stat-desc">{gpsStatus.runtimeEnabled ? 'runtime active' : 'runtime idle'}</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Fix</div>
					<div class="stat-value text-base">{gpsStatus.hasFix ? 'Yes' : 'No'}</div>
					<div class="stat-desc">{gpsStatus.moduleDetected ? 'module detected' : 'waiting for module'}</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Satellites</div>
					<div class="stat-value text-base">{gpsStatus.satellites || 0}</div>
					<div class="stat-desc">{gpsStatus.parserActive ? 'parser active' : 'parser idle'}</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Speed (mph)</div>
					<div class="stat-value text-base">
						{typeof gpsStatus.speedMph === 'number' ? Math.round(gpsStatus.speedMph) : '—'}
					</div>
					<div class="stat-desc">{gpsStatus.detectionTimedOut ? 'module timeout' : 'live sample'}</div>
				</div>
			</div>
		</div>
	</div>

	<div class="card bg-base-200 shadow">
		<div class="card-body gap-3">
			<div class="flex flex-wrap items-center justify-between gap-3">
				<div>
					<h2 class="card-title">Lockout Runtime Controls</h2>
					<p class="text-sm text-base-content/70">
						Configure lockout behavior without enabling extra background work.
					</p>
				</div>
				<div class="flex gap-2">
					<button class="btn btn-outline btn-sm" onclick={() => fetchGpsStatus()}>
						Reload
					</button>
					<button
						class="btn btn-primary btn-sm"
						onclick={saveLockoutConfig}
						disabled={savingLockoutConfig || !lockoutConfigDirty}
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
					<select class="select select-bordered select-sm" bind:value={lockoutConfig.modeRaw} onchange={markLockoutDirty}>
						<option value={0}>Off</option>
						<option value={1}>Shadow (read-only)</option>
						<option value={2}>Advisory (read-only)</option>
						<option value={3}>Enforce (mute enabled)</option>
					</select>
				</label>
				<label class="label cursor-pointer justify-start gap-3 py-0">
					<span class="label-text text-sm">Core guard</span>
					<input
						type="checkbox"
						class="toggle toggle-primary toggle-sm"
						checked={!!lockoutConfig.coreGuardEnabled}
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
											<td>{formatFrequency(zone.frequencyMHz)}</td>
											<td>{typeof zone.confidence === 'number' ? zone.confidence : '—'}</td>
											<td>{typeof zone.radiusM === 'number' ? `${Math.round(zone.radiusM)} m` : '—'}</td>
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
											<td>{formatFrequency(zone.frequencyMHz)}</td>
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
						Read-only recent signal observations for post-test lockout review.
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
							? `${formatFrequency(lockoutEvents[0].frequencyMHz)} • fix age ${formatFixAgeMs(lockoutEvents[0].fixAgeMs)}`
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
									<td>{formatFrequency(event.frequencyMHz)}</td>
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

	<div class="card bg-base-200 shadow">
		<div class="card-body gap-3">
			<div class="flex flex-wrap items-center justify-between gap-3">
				<div>
					<h2 class="card-title">Current Status</h2>
					<p class="text-sm text-base-content/70">
						{#if status.connected}
							Connected to {status.deviceName || status.deviceAddress}
						{:else}
							Not connected
						{/if}
					</p>
				</div>
				<div class="flex gap-2">
					<button class="btn btn-outline btn-sm" onclick={refreshAll}>Refresh</button>
					<button class="btn btn-warning btn-sm" onclick={disconnectObd} disabled={!status.connected}>Disconnect</button>
				</div>
			</div>
			<div class="form-control">
				<label class="label cursor-pointer justify-start gap-3 py-0">
					<input
						type="checkbox"
						class="toggle toggle-sm toggle-primary"
						checked={!!status.vwDataEnabled}
						onchange={(e) => toggleVwData(e.currentTarget.checked)}
						disabled={savingVwData}
					/>
					<span class="label-text">VW data</span>
				</label>
				<div class="text-xs text-base-content/60">Enable VW-specific PIDs (oil temp)</div>
			</div>

				<div class="stats stats-vertical md:stats-horizontal shadow bg-base-100">
					<div class="stat py-3 px-4">
						<div class="stat-title">V1 Link</div>
						<div class="stat-value text-base">{status.v1Connected ? 'Ready' : 'Offline'}</div>
					</div>
					<div class="stat py-3 px-4">
						<div class="stat-title">Speed (mph)</div>
						<div class="stat-value text-base">{status.hasValidData ? Math.round(status.speedMph) : '—'}</div>
					</div>
					<div class="stat py-3 px-4">
						<div class="stat-title">RPM</div>
						<div class="stat-value text-base">{status.sampleTsMs ? Math.round(status.rpm || 0) : '—'}</div>
					</div>
					<div class="stat py-3 px-4">
						<div class="stat-title">IAT</div>
						<div class="stat-value text-base">{formatTemp(status.intakeAirTempC)}</div>
					</div>
					<div class="stat py-3 px-4">
						<div class="stat-title">Oil Temp</div>
						<div class="stat-value text-base">{formatTemp(status.oilTempC)}</div>
						<div class="stat-desc">{status.vwDataEnabled ? 'VW PID' : 'disabled'}</div>
					</div>
					<div class="stat py-3 px-4">
						<div class="stat-title">Data Age</div>
						<div class="stat-value text-base">{status.sampleTsMs ? `${Math.round(status.sampleAgeMs / 1000)}s` : '—'}</div>
						<div class="stat-desc">{status.sampleTsMs && status.sampleAgeMs <= 4000 ? 'fresh' : 'waiting for data'}</div>
					</div>
					<div class="stat py-3 px-4">
						<div class="stat-title">Remembered</div>
						<div class="stat-value text-base">{status.rememberedCount}</div>
						<div class="stat-desc">Auto-connect: {status.autoConnectCount}</div>
					</div>
				</div>
		</div>
	</div>

	<div class="card bg-base-200 shadow">
		<div class="card-body gap-3">
			<div class="flex flex-wrap items-center justify-between gap-3">
				<div>
					<h2 class="card-title">Nearby Devices</h2>
					<p class="text-sm text-base-content/70">Scan runs only when you start it.</p>
				</div>
				<div class="flex gap-2">
					{#if scanning}
						<button class="btn btn-warning btn-sm" onclick={stopScan}>Stop Scan</button>
					{:else}
						<button class="btn btn-primary btn-sm" onclick={startScan} disabled={!status.v1Connected}>Scan Nearby</button>
					{/if}
					<button class="btn btn-ghost btn-sm" onclick={clearNearby} disabled={nearby.length === 0}>Clear</button>
				</div>
			</div>

			{#if scanning}
				<div class="alert alert-info py-2">
					<span class="loading loading-spinner loading-sm"></span>
					<span>Scanning...</span>
				</div>
			{/if}

			{#if loading}
				<div class="flex justify-center p-6"><span class="loading loading-spinner loading-md"></span></div>
			{:else if nearby.length === 0}
				<div class="text-sm text-base-content/70">No devices found yet.</div>
			{:else}
				<div class="overflow-x-auto">
					<table class="table table-sm">
						<thead>
							<tr>
								<th>Name</th>
								<th>Address</th>
								<th>RSSI</th>
								<th></th>
							</tr>
						</thead>
						<tbody>
							{#each nearby as device}
								<tr>
									<td class="font-medium">{device.name || 'Unnamed'}</td>
									<td class="font-mono text-xs">{device.address}</td>
									<td>{device.rssi}</td>
									<td>
										<button class="btn btn-primary btn-xs" onclick={() => openConnectModal(device)}>Connect</button>
									</td>
								</tr>
							{/each}
						</tbody>
					</table>
				</div>
			{/if}
		</div>
	</div>

	<div class="card bg-base-200 shadow">
		<div class="card-body gap-3">
			<h2 class="card-title">Previously Connected</h2>
			<p class="text-sm text-base-content/70">
				Auto-connect attempts only for devices enabled below. No background scan is started.
			</p>

			{#if remembered.length === 0}
				<div class="text-sm text-base-content/70">No remembered devices.</div>
			{:else}
				<div class="overflow-x-auto">
					<table class="table table-sm">
						<thead>
							<tr>
								<th>Name</th>
								<th>Address</th>
								<th>Auto-connect</th>
								<th></th>
							</tr>
						</thead>
						<tbody>
							{#each remembered as device}
								<tr>
									<td>
										<div class="font-medium">{device.name || 'Unnamed'}</div>
										{#if device.connected}
											<div class="badge badge-success badge-xs">connected</div>
										{/if}
									</td>
									<td class="font-mono text-xs">{device.address}</td>
									<td>
										<input
											type="checkbox"
											class="toggle toggle-sm toggle-primary"
											checked={device.autoConnect}
											onchange={(e) => toggleRememberedAutoConnect(device, e.currentTarget.checked)}
										/>
									</td>
									<td class="space-x-1">
										<button class="btn btn-outline btn-xs" onclick={() => connectRememberedDevice(device)}>Connect</button>
										<button class="btn btn-error btn-outline btn-xs" onclick={() => forgetRememberedDevice(device)}>Forget</button>
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

{#if showPinModal && selectedDevice}
	<div class="modal modal-open">
		<div class="modal-box">
			<h3 class="font-bold text-lg">Connect to Device</h3>
			<p class="text-sm mt-1 text-base-content/70">
				{selectedDevice.name || 'Unnamed'}<br />
				<span class="font-mono text-xs">{selectedDevice.address}</span>
			</p>

			<div class="form-control mt-4">
				<label class="label" for="obd-pin">
					<span class="label-text">PIN (optional)</span>
				</label>
				<input
					id="obd-pin"
					type="text"
					class="input input-bordered"
					placeholder="1234 or 123456"
					bind:value={pin}
				/>
				<div class="label">
					<span class="label-text-alt">Leave blank to use adapter default.</span>
				</div>
			</div>

			<div class="form-control mt-2">
				<label class="label cursor-pointer justify-start gap-3">
					<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={rememberDevice} />
					<span class="label-text">Remember this device</span>
				</label>
			</div>
			<div class="form-control mt-1">
				<label class="label cursor-pointer justify-start gap-3">
					<input type="checkbox" class="toggle toggle-primary toggle-sm" bind:checked={autoConnect} disabled={!rememberDevice} />
					<span class="label-text">Enable auto-connect for this device</span>
				</label>
			</div>

			<div class="modal-action">
				<button class="btn btn-ghost" onclick={closeConnectModal} disabled={connecting}>Cancel</button>
				<button class="btn btn-primary" onclick={connectSelectedDevice} disabled={connecting}>
					{#if connecting}
						<span class="loading loading-spinner loading-sm"></span>
						Connecting...
					{:else}
						Connect
					{/if}
				</button>
			</div>
		</div>
	</div>
{/if}
