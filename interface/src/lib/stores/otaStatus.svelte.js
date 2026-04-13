/**
 * OTA Status Store
 *
 * Manages OTA update state. Two modes:
 * - One-shot check: called once on page load (doesn't hit GitHub on every poll)
 * - Progress polling: 2-second interval during an active update
 */

import { writable } from 'svelte/store';
import { fetchWithTimeout, createPoll } from '$lib/utils/poll';

function createDefaultOtaStatus() {
	return {
		state: 'idle',
		current_version: '',
		available_version: null,
		changelog: '',
		breaking: false,
		notes: '',
		can_update: false,
		blocked_reason: null,
		progress: 0,
		error: null,
		sta_connected: false,
		check_done: false
	};
}

export const otaStatus = writable(createDefaultOtaStatus());
export const otaError = writable(null);

let progressPoll = null;
let checkInFlight = false;
let consecutiveFailures = 0;
const MAX_POLL_FAILURES = 5;

/**
 * Fetch current OTA status from the device.
 * Stops the progress poll after repeated failures (device restarting).
 */
async function fetchOtaStatus() {
	try {
		const res = await fetchWithTimeout('/api/ota/status');
		if (!res.ok) {
			otaError.set('Failed to fetch OTA status');
			consecutiveFailures++;
			if (consecutiveFailures >= MAX_POLL_FAILURES && progressPoll) {
				stopProgressPoll();
				otaStatus.update((s) => ({ ...s, state: 'restarting' }));
			}
			return null;
		}
		consecutiveFailures = 0;
		const data = await res.json();
		otaStatus.set(data);
		otaError.set(null);
		return data;
	} catch (e) {
		consecutiveFailures++;
		if (consecutiveFailures >= MAX_POLL_FAILURES && progressPoll) {
			// Device is likely restarting — stop hammering it.
			stopProgressPoll();
			otaStatus.update((s) => ({ ...s, state: 'restarting' }));
			otaError.set(null);
		} else {
			otaError.set('Connection error');
		}
		return null;
	}
}

/**
 * Trigger a version check against GitHub. Called once per session
 * when the user opens the web UI on a STA connection.
 * Returns the status data or null on failure.
 */
export async function checkForUpdate() {
	if (checkInFlight) return null;
	checkInFlight = true;

	try {
		const res = await fetchWithTimeout('/api/ota/check', {
			method: 'POST'
		});

		if (!res.ok) {
			const data = await res.json().catch(() => ({}));
			otaError.set(data.message || data.error || 'Check failed');
			checkInFlight = false;
			return null;
		}

		// The check is async on the device — poll status to get the result.
		// Give the device a moment to complete the GitHub API call.
		await new Promise((r) => setTimeout(r, 3000));
		const status = await fetchOtaStatus();
		checkInFlight = false;
		return status;
	} catch (e) {
		otaError.set('Connection error during check');
		checkInFlight = false;
		return null;
	}
}

/**
 * Start the OTA update. Returns true if the device accepted the request.
 */
export async function startUpdate(target = 'both') {
	try {
		const res = await fetchWithTimeout('/api/ota/start', {
			method: 'POST',
			headers: { 'Content-Type': 'application/json' },
			body: JSON.stringify({ target })
		});

		if (!res.ok) {
			const data = await res.json().catch(() => ({}));
			otaError.set(data.message || data.error || 'Failed to start update');
			return false;
		}

		// Start polling for progress.
		startProgressPoll();
		return true;
	} catch (e) {
		otaError.set('Connection error');
		return false;
	}
}

/**
 * Start polling OTA status every 2 seconds during an active update.
 */
function startProgressPoll() {
	stopProgressPoll();
	progressPoll = createPoll(fetchOtaStatus, 2000);
	progressPoll.start();
}

/**
 * Stop the progress poll.
 */
export function stopProgressPoll() {
	if (progressPoll) {
		progressPoll.stop();
		progressPoll = null;
	}
}

/**
 * Refresh OTA status without triggering a GitHub check.
 * Used by the settings card to get the current state.
 */
export async function refreshOtaStatus() {
	return fetchOtaStatus();
}
