<script>
	import { onMount } from 'svelte';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';

	let loading = $state(true);
	let scanning = $state(false);
	let connecting = $state(false);
	let message = $state(null);
	let statusPoll = $state(null);
	let scanPoll = $state(null);
	let statusFetchInFlight = false;
	let gpsStatusFetchInFlight = false;
	let nearbyFetchInFlight = false;
	let savingObdEnabled = $state(false);
	let savingVwData = $state(false);
	let savingGpsEnabled = $state(false);

	const STATUS_POLL_INTERVAL_MS = 2500;
	const SCAN_POLL_INTERVAL_MS = 1500;

	let status = $state({
		enabled: true,
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
		sampleAgeMs: null,
		moduleDetected: false,
		detectionTimedOut: false,
		parserActive: false
	});

	let nearby = $state([]);
	let remembered = $state([]);

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

	function formatFrequency(mhz) {
		if (typeof mhz !== 'number' || !Number.isFinite(mhz) || mhz <= 0) return '—';
		if (mhz >= 1000) return `${(mhz / 1000).toFixed(3)} GHz`;
		return `${mhz.toFixed(1)} MHz`;
	}

	async function refreshAll() {
		await Promise.all([
			fetchStatus(),
			fetchNearby(),
			fetchRemembered(),
			fetchGpsStatus()
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
		} catch (e) {
			// Polling should fail silently.
		} finally {
			gpsStatusFetchInFlight = false;
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
		if (!status.enabled) {
			setMsg('error', 'Enable OBD service first.');
			return;
		}
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

	async function toggleObdEnabled(enabled) {
		if (savingObdEnabled) return;
		const previousEnabled = !!status.enabled;
		status = { ...status, enabled };
		savingObdEnabled = true;

		try {
			const res = await fetch('/api/obd/config', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ enabled })
			});
			const data = await res.json().catch(() => ({}));
			if (!res.ok) {
				setMsg('error', data.message || 'Failed to update OBD service');
				status = { ...status, enabled: previousEnabled };
				return;
			}

			const nextEnabled = data.enabled === undefined ? enabled : !!data.enabled;
			const nextVwDataEnabled =
				data.vwDataEnabled === undefined ? !!status.vwDataEnabled : !!data.vwDataEnabled;
			status = {
				...status,
				enabled: nextEnabled,
				vwDataEnabled: nextVwDataEnabled,
				connected: nextEnabled ? status.connected : false,
				scanning: nextEnabled ? status.scanning : false
			};
			if (!nextEnabled) {
				scanning = false;
				stopScanPoll();
			}
			setMsg('success', `OBD service ${nextEnabled ? 'enabled' : 'disabled'}`);
			await fetchStatus();
		} catch (e) {
			status = { ...status, enabled: previousEnabled };
			setMsg('error', 'Failed to update OBD service');
		} finally {
			savingObdEnabled = false;
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

</script>

<div class="page-stack">
	<PageHeader title="Integrations" subtitle="GPS and OBD runtime controls plus connectivity status.">
		<div class="flex items-center gap-2">
			<a href="/lockouts" class="btn btn-outline btn-sm">Open Lockouts</a>
			<div class="badge {getStateBadge(status.state)} badge-lg">{status.state}</div>
		</div>
	</PageHeader>

	{#if message}
		<div class="surface-alert alert-{message.type === 'error' ? 'error' : message.type === 'success' ? 'success' : 'info'}" role="status" aria-live="polite">
			<span>{message.text}</span>
		</div>
	{/if}

	<div class="surface-card">
		<div class="card-body gap-3">
			<CardSectionHead
				title="GPS Runtime"
				subtitle="GPS provides fix/location telemetry only. Speed control is OBD-only."
			>
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
			</CardSectionHead>

			<div class="surface-stats">
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
					<div class="stat-title">Sample Age</div>
					<div class="stat-value text-base">
						{typeof gpsStatus.sampleAgeMs === 'number' ? `${Math.round(gpsStatus.sampleAgeMs / 1000)}s` : '—'}
					</div>
					<div class="stat-desc">
						{gpsStatus.detectionTimedOut ? 'module timeout' : gpsStatus.hasFix ? 'latest fix sample' : 'waiting for fix'}
					</div>
				</div>
			</div>
		</div>
	</div>

		<div class="surface-card">
			<div class="card-body gap-3">
				<CardSectionHead
					title="Current Status"
					subtitle={!status.enabled
						? 'OBD service disabled'
						: status.connected
							? `Connected to ${status.deviceName || status.deviceAddress}`
							: 'Not connected'}
				>
					<div class="flex gap-2">
						<button class="btn btn-outline btn-sm" onclick={refreshAll}>Refresh</button>
						<button class="btn btn-warning btn-sm" onclick={disconnectObd} disabled={!status.connected}>Disconnect</button>
					</div>
				</CardSectionHead>
				<div class="form-control">
					<label class="label cursor-pointer justify-start gap-3 py-0">
						<input
							type="checkbox"
							class="toggle toggle-sm toggle-primary"
							checked={!!status.enabled}
							onchange={(e) => toggleObdEnabled(e.currentTarget.checked)}
							disabled={savingObdEnabled}
						/>
						<span class="label-text">OBD service</span>
					</label>
					<div class="copy-caption">
						{#if status.enabled}
							Scan/connect and auto-connect are active.
						{:else}
							Service is off. Scan/connect are blocked.
						{/if}
					</div>
				</div>
				<div class="form-control">
					<label class="label cursor-pointer justify-start gap-3 py-0">
						<input
							type="checkbox"
							class="toggle toggle-sm toggle-primary"
							checked={!!status.vwDataEnabled}
							onchange={(e) => toggleVwData(e.currentTarget.checked)}
							disabled={savingVwData || !status.enabled}
						/>
						<span class="label-text">VW data</span>
					</label>
					<div class="copy-caption">Enable VW-specific PIDs (oil temp)</div>
				</div>

				<div class="surface-stats">
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

		<div class="surface-card">
			<div class="card-body gap-3">
				<CardSectionHead title="Nearby Devices" subtitle="Scan runs only when you start it.">
					<div class="flex gap-2">
						{#if scanning}
							<button class="btn btn-warning btn-sm" onclick={stopScan}>Stop Scan</button>
						{:else}
							<button class="btn btn-primary btn-sm" onclick={startScan} disabled={!status.enabled || !status.v1Connected}>Scan Nearby</button>
						{/if}
						<button class="btn btn-ghost btn-sm" onclick={clearNearby} disabled={nearby.length === 0}>Clear</button>
					</div>
				</CardSectionHead>

				{#if scanning}
					<div class="surface-alert alert-info py-2">
						<span class="loading loading-spinner loading-sm"></span>
						<span>Scanning...</span>
					</div>
				{/if}

				{#if loading}
					<div class="state-loading tight">
						<span class="loading loading-spinner loading-md"></span>
					</div>
				{:else if nearby.length === 0}
					<div class="state-empty">No devices found yet.</div>
				{:else}
					<div class="surface-table-wrap">
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
											<button class="btn btn-primary btn-xs" onclick={() => openConnectModal(device)} disabled={!status.enabled}>Connect</button>
										</td>
									</tr>
								{/each}
							</tbody>
						</table>
					</div>
				{/if}
			</div>
		</div>

	<div class="surface-card">
		<div class="card-body gap-3">
			<CardSectionHead
				title="Previously Connected"
				subtitle="Auto-connect attempts only for devices enabled below. No background scan is started."
			/>

			{#if remembered.length === 0}
				<div class="state-empty">No remembered devices.</div>
			{:else}
				<div class="surface-table-wrap">
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
										<button class="btn btn-outline btn-xs" onclick={() => connectRememberedDevice(device)} disabled={!status.enabled}>Connect</button>
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
		<div class="modal-box surface-modal">
			<h3 class="font-bold text-lg">Connect to Device</h3>
			<p class="copy-subtle mt-1">
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
