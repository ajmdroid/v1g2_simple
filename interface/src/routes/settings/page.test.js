import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function installDefaultFetch(overrides = []) {
	return installFetchMock(
		[
			...overrides,
			{ method: 'GET', match: '/api/settings', respond: jsonResponse({ ap_ssid: 'V1', proxy_ble: true }) },
			{
				method: 'GET',
				match: '/api/wifi/status',
				respond: jsonResponse({ enabled: true, state: 'disconnected', savedSSID: 'HomeWifi' })
			},
			{ method: 'GET', match: '/api/status', respond: jsonResponse({ time: { valid: false } }) },
			{ method: 'POST', match: '/api/settings', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/wifi/scan', respond: jsonResponse({ scanning: false, networks: [] }) },
		],
		jsonResponse({})
	);
}

describe('settings route page', () => {
	it('loads and fetches initial settings/status data', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Settings');
		await waitFor(() => {
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/settings')).toBe(true);
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/wifi/status')).toBe(true);
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/status')).toBe(true);
		});

		unmount();
	});

	it('shows load error when settings endpoint fails', async () => {
		installFetchMock(
			[
				{ method: 'GET', match: '/api/settings', respond: () => Promise.reject(new Error('boom')) },
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

	it('shows success message on save success', async () => {
		const fetchMock = installDefaultFetch([
			{ method: 'POST', match: '/api/settings', respond: jsonResponse({ success: true }) }
		]);
		const { unmount } = render(Page);

		const saveButton = await screen.findByRole('button', { name: /save settings/i });
		await fireEvent.click(saveButton);
		await screen.findByText('Settings saved! WiFi will restart.');
		expect(fetchMock.mock.calls.some(([url, init]) => url === '/api/settings' && init?.method === 'POST')).toBe(true);
		unmount();
	});

	it('shows API error message on save failure', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/settings', respond: jsonResponse({ success: false }, 500) }
		]);
		const { unmount } = render(Page);

		const saveButton = await screen.findByRole('button', { name: /save settings/i });
		await fireEvent.click(saveButton);

		await screen.findByText('Failed to save settings');
		unmount();
	});
});
