import { fireEvent, render, screen } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function createStorageMock() {
	const data = new Map();

	return {
		getItem(key) {
			return data.has(key) ? data.get(key) : null;
		},
		setItem(key, value) {
			data.set(String(key), String(value));
		},
		removeItem(key) {
			data.delete(String(key));
		}
	};
}

function installDefaultFetch() {
	return installFetchMock(
		[
			{
				method: 'GET',
				match: '/api/settings',
				respond: jsonResponse({
					enableWifiAtBoot: true,
					enableSignalTraceLogging: false
				})
			},
			{
				method: 'GET',
				match: '/api/debug/perf-files',
				respond: jsonResponse({
					storageReady: true,
					onSdCard: true,
					path: '/perf',
					files: [{ name: 'perf-0001.csv', sizeBytes: 1536, active: true }]
				})
			},
			{
				method: 'GET',
				match: '/api/debug/metrics',
				respond: jsonResponse({
					rxPackets: 12,
					parseSuccesses: 12,
					queueDrops: 0,
					queueHighWater: 3,
					displayUpdates: 9,
					displaySkips: 1,
					monitoringEnabled: true,
					latencyMinUs: 1200,
					latencyAvgUs: 2500,
					latencyMaxUs: 4400,
					latencySamples: 5,
					proxy: {
						connected: true,
						sendCount: 4,
						dropCount: 0,
						errorCount: 0
					},
					reconnects: 1,
					disconnects: 0
				})
			},
			{ method: 'POST', match: '/api/displaycolors', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/debug/perf-files/delete', respond: jsonResponse({ success: true }) }
		],
		jsonResponse({})
	);
}

describe('dev route page', () => {
	beforeEach(() => {
		const localStorageMock = createStorageMock();
		const sessionStorageMock = createStorageMock();
		Object.defineProperty(window, 'localStorage', { configurable: true, value: localStorageMock });
		Object.defineProperty(window, 'sessionStorage', { configurable: true, value: sessionStorageMock });
		Object.defineProperty(globalThis, 'localStorage', { configurable: true, value: localStorageMock });
		Object.defineProperty(globalThis, 'sessionStorage', { configurable: true, value: sessionStorageMock });
		global.confirm = vi.fn(() => true);
		window.open = vi.fn();
	});

	afterEach(() => {
		vi.restoreAllMocks();
	});

	it('loads development settings, metrics, and perf files', async () => {
		installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Development Settings');
		await screen.findByText('Enable WiFi at Boot');
		await screen.findByText('perf-0001.csv');

		await fireEvent.click(screen.getByRole('button', { name: /^expand$/i }));

		expect(await screen.findByText('BLE Queue (V1 to Display)')).toBeInTheDocument();
		expect(screen.getByText('Perf CSV Files')).toBeInTheDocument();
		expect(screen.getByText('active')).toBeInTheDocument();

		unmount();
	});
});
