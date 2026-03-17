import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import * as devLazyComponents from '$lib/features/dev/devLazyComponents.js';
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

function installDefaultFetch(overrides = []) {
	return installFetchMock(
		[
			...overrides,
			{
				method: 'GET',
				match: '/api/device/settings',
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
					loggingActive: false,
					activeFile: '',
					fileOpsBlocked: false,
					files: [
						{
							name: 'perf-0001.csv',
							sizeBytes: 1536,
							active: true,
							downloadAllowed: true,
							deleteAllowed: true
						}
					]
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
			{ method: 'POST', match: '/api/device/settings', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/debug/perf-files/delete', respond: jsonResponse({ success: true }) }
		],
		jsonResponse({})
	);
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

	it('lazy-loads the metrics panel on first expand and reuses it after collapse', async () => {
		const deferred = createDeferred();
		const metricsPanelLoader = vi
			.spyOn(devLazyComponents, 'loadDevMetricsPanel')
			.mockReturnValue(deferred.promise);
		installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('perf-0001.csv');
		expect(metricsPanelLoader).not.toHaveBeenCalled();

		await fireEvent.click(screen.getByRole('button', { name: /^expand$/i }));
		expect(metricsPanelLoader).toHaveBeenCalledTimes(1);
		await screen.findByText('Loading metrics panel...');

		deferred.resolve(await import('$lib/features/dev/DevMetricsPanel.svelte'));
		await screen.findByText('BLE Queue (V1 to Display)');

		await fireEvent.click(screen.getByRole('button', { name: /^collapse$/i }));
		await waitFor(() => {
			expect(screen.queryByText('BLE Queue (V1 to Display)')).toBeNull();
		});

		await fireEvent.click(screen.getByRole('button', { name: /^expand$/i }));
		await screen.findByText('BLE Queue (V1 to Display)');
		expect(metricsPanelLoader).toHaveBeenCalledTimes(1);

		unmount();
	});

	it('keeps metrics auto refresh working after the panel is lazy-loaded', async () => {
		vi.useFakeTimers();
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('perf-0001.csv');
		await fireEvent.click(screen.getByRole('button', { name: /^expand$/i }));
		await screen.findByText('BLE Queue (V1 to Display)');

		await fireEvent.click(screen.getByText('Auto (2s)'));
		await vi.advanceTimersByTimeAsync(2000);

		await waitFor(() => {
			const metricsCalls = fetchMock.mock.calls.filter(([url]) => url === '/api/debug/metrics');
			expect(metricsCalls.length).toBeGreaterThanOrEqual(2);
		});

		unmount();
	});

	it('keeps the metrics panel usable when metrics fetch fails', async () => {
		installDefaultFetch([
			{ method: 'GET', match: '/api/debug/metrics', respond: jsonResponse({ error: 'bad metrics' }, 500) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('perf-0001.csv');
		await fireEvent.click(screen.getByRole('button', { name: /^expand$/i }));

		await waitFor(() => {
			expect(screen.getByText('Click Refresh or enable Auto to load metrics')).toBeInTheDocument();
		});
		expect(screen.getByText('Failed to load metrics')).toBeInTheDocument();
		expect(screen.queryByText('BLE Queue (V1 to Display)')).not.toBeInTheDocument();

		unmount();
	});

	it('shows an error when perf files fail to load on mount', async () => {
		installDefaultFetch([
			{ method: 'GET', match: '/api/debug/perf-files', respond: jsonResponse({ error: 'bad files' }, 500) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('Failed to load perf files');
		expect(screen.getByText('Perf CSV Files')).toBeInTheDocument();
		expect(screen.queryByText('perf-0001.csv')).not.toBeInTheDocument();

		unmount();
	});

	it('keeps the perf file listed when delete fails', async () => {
		installDefaultFetch([
			{
				method: 'POST',
				match: '/api/debug/perf-files/delete',
				respond: jsonResponse({ error: 'locked' }, 500)
			}
		]);
		const { unmount } = render(Page);

		await screen.findByText('perf-0001.csv');
		await fireEvent.click(screen.getByRole('checkbox', { name: /i understand the risks/i }));
		await fireEvent.click(screen.getByRole('button', { name: /^delete$/i }));

		await screen.findByText('Failed to delete perf-0001.csv: locked');
		expect(screen.getByText('perf-0001.csv')).toBeInTheDocument();

		unmount();
	});

	it('disables protected perf file actions when logging is active', async () => {
		installDefaultFetch([
			{
				method: 'GET',
				match: '/api/debug/perf-files',
				respond: jsonResponse({
					storageReady: true,
					onSdCard: true,
					path: '/perf',
					loggingActive: true,
					activeFile: 'perf-0001.csv',
					fileOpsBlocked: true,
					fileOpsBlockedReason: 'Perf logging active',
					fileOpsBlockedReasonCode: 'perf_logging_active',
					files: [
						{
							name: 'perf-0001.csv',
							sizeBytes: 1536,
							active: true,
							downloadAllowed: false,
							deleteAllowed: false,
							blockedReason: 'Perf logging active',
							blockedReasonCode: 'perf_logging_active'
						}
					]
				})
			}
		]);
		const { unmount } = render(Page);

		await screen.findByText('perf-0001.csv');
		expect(screen.getByText(/download and delete are temporarily unavailable/i)).toBeInTheDocument();
		expect(screen.getByRole('button', { name: /^download$/i })).toBeDisabled();
		expect(screen.getByRole('button', { name: /^delete$/i })).toBeDisabled();

		unmount();
	});
});
