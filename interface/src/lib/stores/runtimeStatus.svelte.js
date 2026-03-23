import { writable } from 'svelte/store';

import { createPoll, fetchWithTimeout } from '$lib/utils/poll';

const STATUS_POLL_INTERVAL_MS = 3000;

function createDefaultStatus() {
	return {
		wifi: {
			sta_connected: false,
			ap_active: false,
			sta_ip: '',
			ap_ip: '',
			ssid: '',
			rssi: 0
		},
		device: {
			uptime: 0,
			heap_free: 0,
			hostname: 'v1g2'
		},
		v1_connected: false,
		alert: null
	};
}

function createDefaultGpsStatus() {
	return {
		enabled: false,
		runtimeEnabled: false,
		mode: 'scaffold',
		hasFix: false,
		stableHasFix: false,
		satellites: 0,
		stableSatellites: 0,
		speedMph: null,
		hdop: null,
		moduleDetected: false,
		detectionTimedOut: false,
		sampleAgeMs: null,
		stableFixAgeMs: null,
		parserActive: false
	};
}

export const runtimeStatus = writable(createDefaultStatus());
export const runtimeStatusError = writable(null);
export const runtimeStatusLoading = writable(true);
export const runtimeGpsStatus = writable(createDefaultGpsStatus());
export const runtimeGpsError = writable(null);

let statusFetchInFlight = false;
let gpsFetchInFlight = false;
let statusConsumerCount = 0;
let nextConsumerId = 1;
let stateVersion = 0;

const gpsConsumers = new Map();

let statusPoll = null;
let gpsPoll = null;
let activeGpsPollIntervalMs = null;

function resetRuntimeState() {
	stateVersion += 1;
	runtimeStatus.set(createDefaultStatus());
	runtimeStatusError.set(null);
	runtimeStatusLoading.set(true);
	runtimeGpsStatus.set(createDefaultGpsStatus());
	runtimeGpsError.set(null);
	statusFetchInFlight = false;
	gpsFetchInFlight = false;
}

function stopStatusPoll() {
	if (statusPoll) {
		statusPoll.stop();
		statusPoll = null;
	}
}

function stopGpsPoll() {
	if (gpsPoll) {
		gpsPoll.stop();
		gpsPoll = null;
	}
	activeGpsPollIntervalMs = null;
}

function syncStatusPoll() {
	if (statusConsumerCount <= 0) {
		stopStatusPoll();
		return;
	}

	if (!statusPoll) {
		statusPoll = createPoll(fetchRuntimeStatus, STATUS_POLL_INTERVAL_MS);
	}
	statusPoll.start();
}

function getActiveGpsPollIntervalMs() {
	let nextIntervalMs = null;
	for (const intervalMs of gpsConsumers.values()) {
		if (!Number.isFinite(intervalMs) || intervalMs <= 0) continue;
		nextIntervalMs = nextIntervalMs === null ? intervalMs : Math.min(nextIntervalMs, intervalMs);
	}
	return nextIntervalMs;
}

function syncGpsPoll() {
	const nextIntervalMs = getActiveGpsPollIntervalMs();
	if (nextIntervalMs === null) {
		stopGpsPoll();
		return;
	}

	if (gpsPoll && activeGpsPollIntervalMs === nextIntervalMs) {
		gpsPoll.start();
		return;
	}

	stopGpsPoll();
	gpsPoll = createPoll(fetchRuntimeGpsStatus, nextIntervalMs);
	activeGpsPollIntervalMs = nextIntervalMs;
	gpsPoll.start();
}

function hasActiveGpsConsumers() {
	return gpsConsumers.size > 0;
}

async function fetchRuntimeStatus() {
	if (statusFetchInFlight) return;
	statusFetchInFlight = true;
	const fetchVersion = stateVersion;

	try {
		const statusRes = await fetchWithTimeout('/api/status');
		if (fetchVersion !== stateVersion || statusConsumerCount <= 0) return;

		if (statusRes.ok) {
			runtimeStatus.set(await statusRes.json());
			runtimeStatusError.set(null);
			return;
		}

		runtimeStatusError.set('API error');
	} catch (e) {
		if (fetchVersion === stateVersion && statusConsumerCount > 0) {
			runtimeStatusError.set('Connection lost');
		}
	} finally {
		if (fetchVersion === stateVersion && statusConsumerCount > 0) {
			runtimeStatusLoading.set(false);
		}
		statusFetchInFlight = false;
	}
}

export async function fetchRuntimeGpsStatus() {
	if (gpsFetchInFlight) return;
	gpsFetchInFlight = true;
	const fetchVersion = stateVersion;

	try {
		const gpsRes = await fetchWithTimeout('/api/gps/status');
		if (fetchVersion !== stateVersion || !hasActiveGpsConsumers()) return;
		if (!gpsRes.ok) {
			runtimeGpsError.set('GPS status unavailable');
			return;
		}

		const gpsData = await gpsRes.json();
		runtimeGpsStatus.update((current) => ({ ...current, ...gpsData }));
		runtimeGpsError.set(null);
	} catch (e) {
		if (fetchVersion === stateVersion && hasActiveGpsConsumers()) {
			runtimeGpsError.set('GPS connection lost');
		}
	} finally {
		gpsFetchInFlight = false;
	}
}

export function retainRuntimeStatus({ needsStatus = false, gpsPollIntervalMs = null } = {}) {
	const consumerId = nextConsumerId++;

	if (needsStatus) {
		statusConsumerCount += 1;
	}
	if (Number.isFinite(gpsPollIntervalMs) && gpsPollIntervalMs > 0) {
		gpsConsumers.set(consumerId, gpsPollIntervalMs);
	}

	syncStatusPoll();
	syncGpsPoll();

	if (needsStatus) {
		void fetchRuntimeStatus();
	}
	if (gpsConsumers.has(consumerId)) {
		void fetchRuntimeGpsStatus();
	}

	return () => {
		if (needsStatus && statusConsumerCount > 0) {
			statusConsumerCount -= 1;
		}
		gpsConsumers.delete(consumerId);

		syncStatusPoll();
		syncGpsPoll();

		if (statusConsumerCount === 0 && gpsConsumers.size === 0) {
			resetRuntimeState();
		}
	};
}
