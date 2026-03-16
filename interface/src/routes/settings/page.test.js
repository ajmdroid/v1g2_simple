import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import * as settingsLazyComponents from '$lib/features/settings/settingsLazyComponents.js';
import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function countCalls(fetchMock, url) {
	return fetchMock.mock.calls.filter(([requestUrl]) => requestUrl === url).length;
}

function createDeferred() {
	let resolve;
	let reject;
	const promise = new Promise((res, rej) => {
		resolve = res;
		reject = rej;
	});
	return { promise, resolve, reject };
}

function installDefaultFetch(overrides = []) {
	return installFetchMock(
		[
			...overrides,
			{
				method: 'GET',
				match: '/api/device/settings',
				respond: jsonResponse({ ap_ssid: 'V1', proxy_ble: true })
			},
			{
				method: 'GET',
				match: '/api/wifi/status',
				respond: jsonResponse({ enabled: true, state: 'disconnected', savedSSID: 'HomeWifi' })
			},
			{ method: 'GET', match: '/api/status', respond: jsonResponse({ time: { valid: false } }) },
			{ method: 'POST', match: '/api/device/settings', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/wifi/scan', respond: jsonResponse({ scanning: false, networks: [] }) },
		],
		jsonResponse({})
	);
}

describe('settings route page', () => {
	afterEach(() => {
		vi.useRealTimers();
		vi.restoreAllMocks();
	});

	it('loads and fetches initial settings/status data', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Settings');
		await waitFor(() => {
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/device/settings')).toBe(true);
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/wifi/status')).toBe(true);
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/status')).toBe(true);
		});

		unmount();
	});

	it('shows load error when settings endpoint fails', async () => {
		installFetchMock(
			[
				{
					method: 'GET',
					match: '/api/device/settings',
					respond: () => Promise.reject(new Error('boom'))
				},
				{ method: 'GET', match: '/api/wifi/status', respond: jsonResponse({ enabled: false, state: 'disabled' }) },
				{ method: 'GET', match: '/api/status', respond: jsonResponse({ time: { valid: false } }) }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await screen.findByText('Failed to load settings');
		unmount();
	});

	it('shows WiFi scan modal when connect action is clicked', async () => {
		installDefaultFetch([
			{
				method: 'POST',
				match: '/api/wifi/scan',
				respond: jsonResponse({
					scanning: false,
					networks: [{ ssid: 'BenchAP', secure: true, rssi: -42 }]
				})
			}
		]);
		const { unmount } = render(Page);

		const scanButton = await screen.findByRole('button', { name: /scan for networks/i });
		await fireEvent.click(scanButton);

		await screen.findByText('Select WiFi Network');
		await fireEvent.click(await screen.findByRole('button', { name: /BenchAP/i }));
		await screen.findByRole('button', { name: /^Back$/i });
		await fireEvent.click(screen.getByRole('button', { name: /^Close$/i }));
		await waitFor(() => {
			expect(screen.queryByText('Select WiFi Network')).toBeNull();
		});
		unmount();
	});

	it('lazy-loads the WiFi modal on first open and reuses it after closing', async () => {
		const deferred = createDeferred();
		const wifiModalLoader = vi
			.spyOn(settingsLazyComponents, 'loadSettingsWifiModal')
			.mockReturnValue(deferred.promise);
		installDefaultFetch([
			{
				method: 'POST',
				match: '/api/wifi/scan',
				respond: jsonResponse({
					scanning: false,
					networks: [{ ssid: 'BenchAP', secure: true, rssi: -42 }]
				})
			}
		]);
		const { unmount } = render(Page);

		expect(wifiModalLoader).not.toHaveBeenCalled();
		await fireEvent.click(await screen.findByRole('button', { name: /scan for networks/i }));

		expect(wifiModalLoader).toHaveBeenCalledTimes(1);
		await screen.findByText('Loading WiFi modal...');

		deferred.resolve(await import('$lib/features/settings/SettingsWifiModal.svelte'));
		await screen.findByText('Select WiFi Network');
		await fireEvent.click(screen.getByRole('button', { name: /^Close$/i }));
		await waitFor(() => {
			expect(screen.queryByText('Select WiFi Network')).toBeNull();
		});

		await fireEvent.click(screen.getByRole('button', { name: /scan for networks/i }));
		await screen.findByText('Select WiFi Network');
		expect(wifiModalLoader).toHaveBeenCalledTimes(1);

		unmount();
	});

	it('shows WiFi status load errors in the page alert', async () => {
		installFetchMock(
			[
				{
					method: 'GET',
					match: '/api/device/settings',
					respond: jsonResponse({ ap_ssid: 'V1', proxy_ble: true })
				},
				{ method: 'GET', match: '/api/wifi/status', respond: jsonResponse({ error: 'bad wifi' }, 500) },
				{ method: 'GET', match: '/api/status', respond: jsonResponse({ time: { valid: false } }) }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await screen.findByText('Failed to load WiFi status');
		expect(screen.getByText('Settings')).toBeInTheDocument();

		unmount();
	});

	it('shows WiFi scan polling errors in the page alert', async () => {
		vi.useFakeTimers();
		let scanCalls = 0;
		installFetchMock(
			[
				{
					method: 'GET',
					match: '/api/device/settings',
					respond: jsonResponse({ ap_ssid: 'V1', proxy_ble: true })
				},
				{
					method: 'GET',
					match: '/api/wifi/status',
					respond: jsonResponse({ enabled: true, state: 'disconnected', savedSSID: 'HomeWifi' })
				},
				{ method: 'GET', match: '/api/status', respond: jsonResponse({ time: { valid: false } }) },
				{
					method: 'POST',
					match: '/api/wifi/scan',
					respond: () => {
						scanCalls += 1;
						if (scanCalls === 1) {
							return jsonResponse({ scanning: true, networks: [] });
						}
						return jsonResponse({ error: 'scan failed' }, 500);
					}
				}
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		const scanButton = await screen.findByRole('button', { name: /scan for networks/i });
		await fireEvent.click(scanButton);
		await vi.advanceTimersByTimeAsync(1000);

		await screen.findByText('Failed to update WiFi scan');

		unmount();
	});

	it('shows success message on save success', async () => {
		const fetchMock = installDefaultFetch([
			{ method: 'POST', match: '/api/device/settings', respond: jsonResponse({ success: true }) }
		]);
		const { unmount } = render(Page);

		const saveButton = await screen.findByRole('button', { name: /save settings/i });
		await fireEvent.click(saveButton);
		await screen.findByText('Settings saved! WiFi will restart.');
		expect(
			fetchMock.mock.calls.some(([url, init]) => url === '/api/device/settings' && init?.method === 'POST')
		).toBe(true);
		unmount();
	});

	it('shows API error message on save failure', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/device/settings', respond: jsonResponse({ success: false }, 500) }
		]);
		const { unmount } = render(Page);

		const saveButton = await screen.findByRole('button', { name: /save settings/i });
		await fireEvent.click(saveButton);

		await screen.findByText('Failed to save settings');
		unmount();
	});

	it('shows the runtime time snapshot without manual sync controls', async () => {
		installFetchMock(
			[
				{
					method: 'GET',
					match: '/api/device/settings',
					respond: jsonResponse({ ap_ssid: 'V1', proxy_ble: true })
				},
				{
					method: 'GET',
					match: '/api/wifi/status',
					respond: jsonResponse({ enabled: true, state: 'disconnected', savedSSID: 'HomeWifi' })
				},
				{
					method: 'GET',
					match: '/api/status',
					respond: jsonResponse({
						time: {
							valid: true,
							source: 2,
							confidence: 2,
							epochMs: 1710000000000,
							tzOffsetMin: -240,
							ageMs: 0
						}
					})
				}
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await screen.findByText('Device Time');
		await screen.findByText('Read-only runtime clock snapshot. Time is sourced by device services, not the browser.');
		expect(screen.queryByRole('button', { name: /sync time from phone/i })).toBeNull();
		await screen.findByText(/2024-03-09 12:00:00 \(UTC-04:00\)/i);

		unmount();
	});
});
