import { render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../test/fetch-mock.js';
import Page from './+page.svelte';

function installDefaultFetch(overrides = []) {
	return installFetchMock(
		[
			...overrides,
			{
				method: 'GET',
				match: '/api/status',
				respond: jsonResponse({
					wifi: {
						sta_connected: true,
						ap_active: false,
						sta_ip: '192.168.1.23',
						ap_ip: '192.168.4.1',
						ssid: 'GarageNet',
						rssi: -54
					},
					device: {
						uptime: 3665,
						heap_free: 49152,
						hostname: 'v1g2',
						firmware_version: '1.2.3'
					},
					v1_connected: true,
					alert: null
				})
			}
		],
		jsonResponse({})
	);
}

function countCalls(fetchMock, url) {
	return fetchMock.mock.calls.filter(([requestUrl]) => requestUrl === url).length;
}

describe('dashboard route page', () => {
	afterEach(() => {
		vi.useRealTimers();
		vi.restoreAllMocks();
	});

	it('loads shared runtime status on mount', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Connected');
		await screen.findByText('GarageNet • -54 dBm');
		await waitFor(() => {
			expect(countCalls(fetchMock, '/api/status')).toBeGreaterThanOrEqual(1);
		});

		unmount();
	});

	it('polls status every 3s through the shared runtime module', async () => {
		vi.useFakeTimers();
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await Promise.resolve();
		expect(countCalls(fetchMock, '/api/status')).toBe(1);

		await vi.advanceTimersByTimeAsync(9000);

		expect(countCalls(fetchMock, '/api/status')).toBe(4);

		unmount();
	});

	it('shows a shared status api error when /api/status returns 500', async () => {
		const fetchMock = installDefaultFetch([
			{
				method: 'GET',
				match: '/api/status',
				respond: jsonResponse({ error: 'nope' }, 500)
			}
		]);
		const { unmount } = render(Page);

		await screen.findByText('API error');
		expect(countCalls(fetchMock, '/api/status')).toBeGreaterThanOrEqual(1);

		unmount();
	});

	it('shows a shared status connection error when /api/status throws', async () => {
		const fetchMock = installFetchMock(
			[
				{
					method: 'GET',
					match: '/api/status',
					respond: () => {
						throw new Error('network down');
					}
				}
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await screen.findByText('Connection lost');
		expect(countCalls(fetchMock, '/api/status')).toBeGreaterThanOrEqual(1);

		unmount();
	});
});
