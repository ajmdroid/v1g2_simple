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

async function selectRestoreFile(contents, name = 'backup.json') {
	await screen.findByRole('button', { name: /restore from backup/i });
	const input = document.querySelector('input[type="file"]');
	expect(input).not.toBeNull();
	const file = new File([contents], name, { type: 'application/json' });
	await fireEvent.change(input, { target: { files: [file] } });
	return file;
}

async function startRestore(contents, name = 'backup.json') {
	await selectRestoreFile(contents, name);
	await fireEvent.click(await screen.findByRole('button', { name: /restore from backup/i }));
}

async function openWifiModalAndSelectNetwork() {
	const scanButton = await screen.findByRole('button', { name: /scan for networks/i });
	await fireEvent.click(scanButton);
	await vi.advanceTimersByTimeAsync(1000);
	await screen.findByText('Select WiFi Network');
	await fireEvent.click(await screen.findByRole('button', { name: /BenchAP/i }));
}

async function openWifiModalAndConnect() {
	await openWifiModalAndSelectNetwork();
	await fireEvent.click(await screen.findByRole('button', { name: /^Connect$/i }));
}

describe('settings route page', () => {
	afterEach(() => {
		vi.useRealTimers();
		vi.restoreAllMocks();
		vi.unstubAllGlobals();
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

	it('times out wifi connection polling after 30 seconds of connecting', async () => {
		vi.useFakeTimers();
		let statusCalls = 0;
		const fetchMock = installFetchMock(
			[
				{
					method: 'GET',
					match: '/api/device/settings',
					respond: jsonResponse({ ap_ssid: 'V1', proxy_ble: true })
				},
				{
					method: 'GET',
					match: '/api/wifi/status',
					respond: () => {
						statusCalls += 1;
						if (statusCalls === 1) {
							return jsonResponse({ enabled: true, state: 'disconnected', savedSSID: 'HomeWifi' });
						}
						return jsonResponse({ enabled: true, state: 'connecting', savedSSID: 'BenchAP' });
					}
				},
				{ method: 'GET', match: '/api/status', respond: jsonResponse({ time: { valid: false } }) },
				{
					method: 'POST',
					match: '/api/wifi/scan',
					respond: jsonResponse({
						scanning: false,
						networks: [{ ssid: 'BenchAP', secure: false, rssi: -42 }]
					})
				},
				{ method: 'POST', match: '/api/wifi/connect', respond: jsonResponse({ success: true }) }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await openWifiModalAndConnect();
		vi.setSystemTime(Date.now() + 30000);
		await vi.advanceTimersByTimeAsync(1000);
		await Promise.resolve();
		expect(screen.getByText('Wi-Fi connection timed out. Check status and retry.')).toBeInTheDocument();

		const stoppedCalls = countCalls(fetchMock, '/api/wifi/status');
		await vi.advanceTimersByTimeAsync(5000);
		expect(countCalls(fetchMock, '/api/wifi/status')).toBe(stoppedCalls);

		unmount();
	});

	it('stops wifi connection polling on disconnected terminal state', async () => {
		vi.useFakeTimers();
		let statusCalls = 0;
		const fetchMock = installFetchMock(
			[
				{
					method: 'GET',
					match: '/api/device/settings',
					respond: jsonResponse({ ap_ssid: 'V1', proxy_ble: true })
				},
				{
					method: 'GET',
					match: '/api/wifi/status',
					respond: () => {
						statusCalls += 1;
						if (statusCalls === 1) {
							return jsonResponse({ enabled: true, state: 'disconnected', savedSSID: 'HomeWifi' });
						}
						return jsonResponse({ enabled: true, state: 'disconnected', savedSSID: 'BenchAP' });
					}
				},
				{ method: 'GET', match: '/api/status', respond: jsonResponse({ time: { valid: false } }) },
				{
					method: 'POST',
					match: '/api/wifi/scan',
					respond: jsonResponse({
						scanning: false,
						networks: [{ ssid: 'BenchAP', secure: false, rssi: -42 }]
					})
				},
				{ method: 'POST', match: '/api/wifi/connect', respond: jsonResponse({ success: true }) }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await openWifiModalAndConnect();
		await vi.advanceTimersByTimeAsync(1000);
		await screen.findByText('Wi-Fi connection timed out. Check status and retry.');

		const stoppedCalls = countCalls(fetchMock, '/api/wifi/status');
		await vi.advanceTimersByTimeAsync(5000);
		expect(countCalls(fetchMock, '/api/wifi/status')).toBe(stoppedCalls);

		unmount();
	});

	it('stops wifi connection polling after a successful connection', async () => {
		vi.useFakeTimers();
		let statusCalls = 0;
		const fetchMock = installFetchMock(
			[
				{
					method: 'GET',
					match: '/api/device/settings',
					respond: jsonResponse({ ap_ssid: 'V1', proxy_ble: true })
				},
				{
					method: 'GET',
					match: '/api/wifi/status',
					respond: () => {
						statusCalls += 1;
						if (statusCalls === 1) {
							return jsonResponse({ enabled: true, state: 'disconnected', savedSSID: 'HomeWifi' });
						}
						return jsonResponse({
							enabled: true,
							state: 'connected',
							savedSSID: 'BenchAP',
							connectedSSID: 'BenchAP',
							ip: '192.168.1.10',
							rssi: -41
						});
					}
				},
				{ method: 'GET', match: '/api/status', respond: jsonResponse({ time: { valid: false } }) },
				{
					method: 'POST',
					match: '/api/wifi/scan',
					respond: jsonResponse({
						scanning: false,
						networks: [{ ssid: 'BenchAP', secure: false, rssi: -42 }]
					})
				},
				{ method: 'POST', match: '/api/wifi/connect', respond: jsonResponse({ success: true }) }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await openWifiModalAndConnect();
		await vi.advanceTimersByTimeAsync(1000);
		await screen.findByText('Connected to BenchAP!');

		const stoppedCalls = countCalls(fetchMock, '/api/wifi/status');
		await vi.advanceTimersByTimeAsync(5000);
		expect(countCalls(fetchMock, '/api/wifi/status')).toBe(stoppedCalls);

		unmount();
	});

	it('prevents overlapping wifi connect requests from double-clicking connect', async () => {
		vi.useFakeTimers();
		const connectRequest = createDeferred();
		const fetchMock = installFetchMock(
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
					respond: jsonResponse({
						scanning: false,
						networks: [{ ssid: 'BenchAP', secure: false, rssi: -42 }]
					})
				},
				{ method: 'POST', match: '/api/wifi/connect', respond: () => connectRequest.promise }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await openWifiModalAndSelectNetwork();
		const connectButton = await screen.findByRole('button', { name: /^Connect$/i });
		await fireEvent.click(connectButton);
		await fireEvent.click(connectButton);

		expect(countCalls(fetchMock, '/api/wifi/connect')).toBe(1);

		connectRequest.resolve(jsonResponse({ success: false }, 500));
		await screen.findByText('Failed to initiate connection');

		unmount();
	});

	it('prevents overlapping wifi connect requests from repeated enter presses', async () => {
		vi.useFakeTimers();
		const connectRequest = createDeferred();
		const fetchMock = installFetchMock(
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
					respond: jsonResponse({
						scanning: false,
						networks: [{ ssid: 'BenchAP', secure: true, rssi: -42 }]
					})
				},
				{ method: 'POST', match: '/api/wifi/connect', respond: () => connectRequest.promise }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await openWifiModalAndSelectNetwork();
		const passwordInput = await screen.findByLabelText('Password');
		await fireEvent.input(passwordInput, { target: { value: 'secret123' } });
		await fireEvent.keyDown(passwordInput, { key: 'Enter' });
		await fireEvent.keyDown(passwordInput, { key: 'Enter' });

		expect(countCalls(fetchMock, '/api/wifi/connect')).toBe(1);

		connectRequest.resolve(jsonResponse({ success: false }, 500));
		await screen.findByText('Failed to initiate connection');

		unmount();
	});

	it('does not post wifi connect when enter is pressed with an empty secure password', async () => {
		vi.useFakeTimers();
		const fetchMock = installFetchMock(
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
					respond: jsonResponse({
						scanning: false,
						networks: [{ ssid: 'BenchAP', secure: true, rssi: -42 }]
					})
				}
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await openWifiModalAndSelectNetwork();
		const passwordInput = await screen.findByLabelText('Password');
		await fireEvent.keyDown(passwordInput, { key: 'Enter' });

		expect(countCalls(fetchMock, '/api/wifi/connect')).toBe(0);

		unmount();
	});

	it('keeps wifi modal controls inert during an active connect attempt', async () => {
		vi.useFakeTimers();
		const connectRequest = createDeferred();
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
					respond: jsonResponse({
						scanning: false,
						networks: [{ ssid: 'BenchAP', secure: false, rssi: -42 }]
					})
				},
				{ method: 'POST', match: '/api/wifi/connect', respond: () => connectRequest.promise }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await openWifiModalAndSelectNetwork();
		await fireEvent.click(await screen.findByRole('button', { name: /^Connect$/i }));

		const backButton = await screen.findByRole('button', { name: /^Back$/i });
		const closeButton = await screen.findByRole('button', { name: /^Close$/i });
		await waitFor(() => {
			expect(backButton).toBeDisabled();
			expect(closeButton).toBeDisabled();
		});

		await fireEvent.click(backButton);
		await fireEvent.click(closeButton);
		await fireEvent.click(screen.getByRole('presentation'));
		expect(screen.getByText('Select WiFi Network')).toBeInTheDocument();
		expect(screen.getByText((_, node) => node?.textContent === 'Connect to BenchAP')).toBeInTheDocument();

		connectRequest.resolve(jsonResponse({ success: false }, 500));
		await screen.findByText('Failed to initiate connection');

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

	it('rejects invalid backup JSON before posting restore', async () => {
		const fetchMock = installDefaultFetch();
		vi.stubGlobal('confirm', vi.fn(() => true));
		const { unmount } = render(Page);

		await startRestore('{not-json');

		await screen.findByText('Selected file is not valid JSON.');
		expect(fetchMock.mock.calls.some(([url]) => url === '/api/settings/restore')).toBe(false);

		unmount();
	});

	it('rejects unrecognized backup types before posting restore', async () => {
		const fetchMock = installDefaultFetch();
		vi.stubGlobal('confirm', vi.fn(() => true));
		const { unmount } = render(Page);

		await startRestore(JSON.stringify({ _type: 'wrong_backup_type' }));

		await screen.findByText('Selected file is not a V1 Simple settings backup.');
		expect(fetchMock.mock.calls.some(([url]) => url === '/api/settings/restore')).toBe(false);

		unmount();
	});

	it('posts valid backups to restore endpoint', async () => {
		const fetchMock = installDefaultFetch([
			{ method: 'POST', match: '/api/settings/restore', respond: jsonResponse({ success: true }) }
		]);
		vi.stubGlobal('confirm', vi.fn(() => true));
		const { unmount } = render(Page);
		const payload = JSON.stringify({ _type: 'v1simple_backup', profiles: [] });

		await startRestore(payload);

		await screen.findByText('Settings restored! Refresh to see changes.');
		expect(
			fetchMock.mock.calls.some(([url, init]) => url === '/api/settings/restore' && init?.body === payload)
		).toBe(true);

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
