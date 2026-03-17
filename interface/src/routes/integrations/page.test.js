import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function countCalls(fetchMock, url) {
	return fetchMock.mock.calls.filter(([requestUrl]) => requestUrl === url).length;
}

function installDefaultFetch() {
	let gpsEnabled = true;

	return installFetchMock(
		[
			{
				method: 'GET',
				match: '/api/gps/status',
				respond: () =>
					jsonResponse({
						enabled: gpsEnabled,
						runtimeEnabled: gpsEnabled,
						mode: 'drive',
						hasFix: true,
						stableHasFix: true,
						satellites: 7,
						stableSatellites: 7,
						sampleAgeMs: 1900,
						moduleDetected: true,
						detectionTimedOut: false,
						parserActive: true
					})
			},
			{
				method: 'POST',
				match: '/api/gps/config',
				respond: async ({ init }) => {
					const body = JSON.parse(init.body);
					gpsEnabled = body.enabled === true;
					return jsonResponse({ success: true });
				}
			},
			{
				method: 'GET',
				match: '/api/obd/config',
				respond: jsonResponse({ enabled: false, minRssi: -80 })
			},
			{
				method: 'GET',
				match: '/api/obd/status',
				respond: jsonResponse({ enabled: false, connected: false, pollCount: 0, pollErrors: 0 })
			},
			{ method: 'POST', match: '/api/obd/config', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/obd/scan', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/obd/forget', respond: jsonResponse({ success: true }) }
		],
		jsonResponse({})
	);
}

function installGpsRecoveryFetch() {
	let gpsEnabled = true;
	let gpsRequestCount = 0;

	return installFetchMock(
		[
			{
				method: 'GET',
				match: '/api/gps/status',
				respond: () => {
					gpsRequestCount += 1;
					if (gpsRequestCount === 1) {
						return jsonResponse({ error: 'gps unavailable' }, 503);
					}

					return jsonResponse({
						enabled: gpsEnabled,
						runtimeEnabled: gpsEnabled,
						mode: 'drive',
						hasFix: true,
						stableHasFix: true,
						satellites: 7,
						stableSatellites: 7,
						sampleAgeMs: 1900,
						moduleDetected: true,
						detectionTimedOut: false,
						parserActive: true
					});
				}
			},
			{
				method: 'POST',
				match: '/api/gps/config',
				respond: async ({ init }) => {
					const body = JSON.parse(init.body);
					gpsEnabled = body.enabled === true;
					return jsonResponse({ success: true });
				}
			},
			{
				method: 'GET',
				match: '/api/obd/config',
				respond: jsonResponse({ enabled: false, minRssi: -80 })
			},
			{
				method: 'GET',
				match: '/api/obd/status',
				respond: jsonResponse({ enabled: false, connected: false, pollCount: 0, pollErrors: 0 })
			},
			{ method: 'POST', match: '/api/obd/config', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/obd/scan', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/obd/forget', respond: jsonResponse({ success: true }) }
		],
		jsonResponse({})
	);
}

describe('integrations route page', () => {
	afterEach(() => {
		vi.useRealTimers();
		vi.restoreAllMocks();
	});

	it('loads gps runtime data from the shared runtime module', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('drive');
		await screen.findByText('7');
		await screen.findByText('2s');
		await waitFor(() => {
			expect(countCalls(fetchMock, '/api/gps/status')).toBeGreaterThanOrEqual(1);
		});

		unmount();
	});

	it('polls gps every 2.5s and refreshes after toggling gps', async () => {
		vi.useFakeTimers();
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await Promise.resolve();
		expect(countCalls(fetchMock, '/api/gps/status')).toBe(1);

		await vi.advanceTimersByTimeAsync(2500);
		expect(countCalls(fetchMock, '/api/gps/status')).toBe(2);

		const toggle = await screen.findByRole('checkbox', { name: /enabled/i });
		await fireEvent.click(toggle);

		await screen.findByText('GPS disabled');
		expect(
			fetchMock.mock.calls.some(
				([url, init]) => url === '/api/gps/config' && init?.method === 'POST'
			)
		).toBe(true);
		expect(countCalls(fetchMock, '/api/gps/status')).toBe(3);

		unmount();
	});

	it('shows a shared gps polling error when gps status fails on mount', async () => {
		const fetchMock = installFetchMock(
			[
				{
					method: 'GET',
					match: '/api/gps/status',
					respond: jsonResponse({ error: 'gps unavailable' }, 503)
				},
				{
					method: 'GET',
					match: '/api/obd/config',
					respond: jsonResponse({ enabled: false, minRssi: -80 })
				},
				{
					method: 'GET',
					match: '/api/obd/status',
					respond: jsonResponse({ enabled: false, connected: false, pollCount: 0, pollErrors: 0 })
				},
				{ method: 'POST', match: '/api/obd/config', respond: jsonResponse({ success: true }) },
				{ method: 'POST', match: '/api/obd/scan', respond: jsonResponse({ success: true }) },
				{ method: 'POST', match: '/api/obd/forget', respond: jsonResponse({ success: true }) }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await screen.findByText('GPS status unavailable');
		expect(countCalls(fetchMock, '/api/gps/status')).toBe(1);

		unmount();
	});

	it('clears the shared gps polling error after the next successful poll', async () => {
		vi.useFakeTimers();
		const fetchMock = installGpsRecoveryFetch();
		const { unmount } = render(Page);

		await screen.findByText('GPS status unavailable');
		expect(countCalls(fetchMock, '/api/gps/status')).toBe(1);

		await vi.advanceTimersByTimeAsync(2500);

		await screen.findByText('drive');
		await waitFor(() => {
			expect(screen.queryByText('GPS status unavailable')).not.toBeInTheDocument();
		});
		expect(countCalls(fetchMock, '/api/gps/status')).toBe(2);

		unmount();
	});

	it('shows an OBD settings error when the OBD settings fetch fails on mount', async () => {
		const errorSpy = vi.spyOn(console, 'error').mockImplementation(() => {});
		installFetchMock(
			[
				{
					method: 'GET',
					match: '/api/gps/status',
					respond: jsonResponse({
						enabled: true,
						runtimeEnabled: true,
						mode: 'drive',
						hasFix: true,
						stableHasFix: true,
						satellites: 7,
						stableSatellites: 7,
						sampleAgeMs: 1900,
						moduleDetected: true,
						detectionTimedOut: false,
						parserActive: true
					})
				},
				{
					method: 'GET',
					match: '/api/obd/config',
					respond: () => Promise.reject(new Error('settings unavailable'))
				},
				{
					method: 'GET',
					match: '/api/obd/status',
					respond: jsonResponse({ enabled: false, connected: false, pollCount: 0, pollErrors: 0 })
				},
				{ method: 'POST', match: '/api/obd/config', respond: jsonResponse({ success: true }) },
				{ method: 'POST', match: '/api/obd/scan', respond: jsonResponse({ success: true }) },
				{ method: 'POST', match: '/api/obd/forget', respond: jsonResponse({ success: true }) }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await screen.findByText('Failed to load OBD settings.');
		expect(errorSpy).toHaveBeenCalled();

		unmount();
	});

	it('maps OBD runtime state codes to the correct labels', async () => {
		installFetchMock(
			[
				{
					method: 'GET',
					match: '/api/gps/status',
					respond: jsonResponse({
						enabled: true,
						runtimeEnabled: true,
						mode: 'drive',
						hasFix: true,
						stableHasFix: true,
						satellites: 7,
						stableSatellites: 7,
						sampleAgeMs: 1900,
						moduleDetected: true,
						detectionTimedOut: false,
						parserActive: true
					})
				},
				{
					method: 'GET',
					match: '/api/obd/config',
					respond: jsonResponse({ enabled: true, minRssi: -80 })
				},
				{
					method: 'GET',
					match: '/api/obd/status',
					respond: jsonResponse({
						enabled: true,
						connected: true,
						securityReady: true,
						encrypted: true,
						bonded: true,
						speedValid: true,
						speedMph: 0,
						speedAgeMs: 12,
						rssi: -65,
						scanInProgress: false,
						savedAddressValid: true,
						pollCount: 166,
						pollErrors: 0,
						state: 8
					})
				},
				{ method: 'POST', match: '/api/obd/config', respond: jsonResponse({ success: true }) },
				{ method: 'POST', match: '/api/obd/scan', respond: jsonResponse({ success: true }) },
				{ method: 'POST', match: '/api/obd/forget', respond: jsonResponse({ success: true }) }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await screen.findByText('Polling');
		expect(screen.queryByText('ErrorBackoff')).not.toBeInTheDocument();

		unmount();
	});
});
