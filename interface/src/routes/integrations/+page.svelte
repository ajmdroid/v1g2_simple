<script>
	import { onMount } from 'svelte';

	let loading = $state(true);
	let scanning = $state(false);
	let connecting = $state(false);
	let message = $state(null);
	let statusPoll = $state(null);
	let scanPoll = $state(null);

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
		oilTempC: null,
		intakeAirTempC: null,
		rememberedCount: 0,
		autoConnectCount: 0
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
			await fetchStatus();
			if (!status.scanning) {
				scanning = false;
				stopScanPoll();
			}
		}, 1500);

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
		return `${Math.round(value)}°C`;
	}

	async function refreshAll() {
		await Promise.all([fetchStatus(), fetchNearby(), fetchRemembered()]);
		loading = false;
	}

	async function fetchStatus() {
		try {
			const res = await fetch('/api/obd/status');
			if (!res.ok) return;
			const data = await res.json();
			status = { ...status, ...data };
			scanning = !!data.scanning;
		} catch (e) {
			// Polling should fail silently.
		}
	}

	async function fetchNearby() {
		try {
			const res = await fetch('/api/obd/devices');
			if (!res.ok) return;
			const data = await res.json();
			nearby = data.devices || [];
			scanning = !!data.scanning;
		} catch (e) {
			// ignore
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
				await Promise.all([fetchNearby(), fetchStatus()]);
				if (!scanning) {
					stopScanPoll();
				}
			}, 1200);
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
</script>

<div class="space-y-6">
	<div class="flex flex-wrap items-center justify-between gap-3">
		<h1 class="text-2xl font-bold">OBD Integration</h1>
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
