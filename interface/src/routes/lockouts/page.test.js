import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import * as lockoutModalLoaders from '$lib/features/lockouts/lockoutModalLoaders.js';
import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function installDefaultFetch(overrides = []) {
	return installFetchMock(
		[
			...overrides,
			{
				method: 'GET',
				match: '/api/gps/status',
				respond: jsonResponse({
					enabled: true,
					runtimeEnabled: true,
					mode: 'drive',
					hasFix: true,
					satellites: 7,
					speedMph: 32,
					hdop: 0.9,
					locationValid: true,
					moduleDetected: true,
					detectionTimedOut: false,
					parserActive: true,
					lockout: {
						modeRaw: 0,
						coreGuardEnabled: true,
						maxQueueDrops: 0,
						maxPerfDrops: 0,
						maxEventBusDrops: 0,
						learnerPromotionHits: 3,
						learnerRadiusE5: 45,
						learnerFreqToleranceMHz: 8,
						learnerLearnIntervalHours: 12,
						learnerUnlearnIntervalHours: 0,
						learnerUnlearnCount: 0,
						manualDemotionMissCount: 12,
						kaLearningEnabled: false,
						kLearningEnabled: true,
						xLearningEnabled: false,
						preQuiet: false,
						preQuietBufferE5: 0,
						maxHdopX10: 20,
						minLearnerSpeedMph: 2
					}
				})
			},
			{
				method: 'GET',
				match: '/api/lockouts/events',
				respond: jsonResponse({
					events: [],
					published: 0,
					drops: 0,
					size: 0,
					capacity: 200,
					sd: { enabled: false }
				})
			},
			{
				method: 'GET',
				match: '/api/lockouts/zones',
				respond: jsonResponse({
					activeZones: [],
					pendingZones: [],
					activeCount: 0,
					activeCapacity: 0,
					activeReturned: 0,
					pendingCount: 0,
					pendingCapacity: 0,
					pendingReturned: 0,
					promotionHits: 3,
					promotionRadiusE5: 45,
					promotionFreqToleranceMHz: 8,
					learnIntervalHours: 12,
					unlearnIntervalHours: 0,
					unlearnCount: 0,
					manualDemotionMissCount: 12,
					droppedManualCount: 0
				})
			},
			{ method: 'POST', match: '/api/gps/config', respond: jsonResponse({ success: true }) },
		],
		jsonResponse({})
	);
}

function countCalls(fetchMock, url) {
	return fetchMock.mock.calls.filter(([requestUrl]) => requestUrl === url).length;
}

function countPrefixCalls(fetchMock, urlPrefix) {
	return fetchMock.mock.calls.filter(([requestUrl]) => String(requestUrl).startsWith(urlPrefix)).length;
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

describe('lockouts route page', () => {
	beforeEach(() => {
		global.confirm = vi.fn(() => true);
	});

	afterEach(() => {
		vi.useRealTimers();
		vi.restoreAllMocks();
	});

	it('loads lockout data and shared gps state on mount without duplicating the gps fetch', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Lockouts');
		await screen.findByText('32 mph');
		await waitFor(() => {
			expect(countCalls(fetchMock, '/api/gps/status')).toBe(1);
			expect(countPrefixCalls(fetchMock, '/api/lockouts/events')).toBe(1);
			expect(countPrefixCalls(fetchMock, '/api/lockouts/zones')).toBe(1);
		});

		unmount();
	});

	it('does not overwrite dirty lockout config when shared gps polling refreshes', async () => {
		vi.useFakeTimers();
		let gpsCallCount = 0;
		const fetchMock = installDefaultFetch([
			{
				method: 'GET',
				match: '/api/gps/status',
				respond: () => {
					gpsCallCount += 1;
					return jsonResponse({
						enabled: true,
						runtimeEnabled: true,
						mode: 'drive',
						hasFix: true,
						satellites: gpsCallCount >= 2 ? 10 : 7,
						speedMph: 32,
						hdop: 0.9,
						locationValid: true,
						moduleDetected: true,
						detectionTimedOut: false,
						parserActive: true,
						lockout: {
							modeRaw: 0,
							coreGuardEnabled: true,
							maxQueueDrops: 0,
							maxPerfDrops: 0,
							maxEventBusDrops: 0,
							learnerPromotionHits: 3,
							learnerRadiusE5: 45,
							learnerFreqToleranceMHz: 8,
							learnerLearnIntervalHours: 12,
							learnerUnlearnIntervalHours: 0,
							learnerUnlearnCount: 0,
							manualDemotionMissCount: 12,
							kaLearningEnabled: false,
							kLearningEnabled: true,
							xLearningEnabled: false,
							preQuiet: false,
							preQuietBufferE5: 0,
							maxHdopX10: 20,
							minLearnerSpeedMph: 2
						}
					});
				}
			}
		]);
		const { unmount } = render(Page);

		await screen.findByText('32 mph');
		await fireEvent.click(screen.getByRole('checkbox', { name: /unlock advanced writes/i }));
		const modeSelect = screen.getByLabelText('Mode');
		await fireEvent.change(modeSelect, { target: { value: '3' } });
		expect(modeSelect).toHaveValue('3');

		await vi.advanceTimersByTimeAsync(2500);

		await waitFor(() => {
			expect(countCalls(fetchMock, '/api/gps/status')).toBe(2);
		});
		expect(modeSelect).toHaveValue('3');

		unmount();
	});

	it('shows zone load failure in the zones card', async () => {
		installDefaultFetch([
			{ method: 'GET', match: '/api/lockouts/zones', respond: jsonResponse({}, 500) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('Failed to load lockout zones');
		unmount();
	});

	it('does not expose manual zone creation after safety gate unlock', async () => {
		installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Lockouts');
		await fireEvent.click(screen.getByRole('checkbox', { name: /unlock advanced writes/i }));
		expect(screen.queryByRole('button', { name: /new manual zone/i })).toBeNull();
		expect(screen.queryByText('Create Manual Lockout Zone')).toBeNull();
		unmount();
	});

	it('does not preload the lockout zone editor modal on mount', async () => {
		const zoneLoader = vi.spyOn(lockoutModalLoaders, 'loadLockoutZoneEditorModal');
		installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Lockouts');
		expect(zoneLoader).not.toHaveBeenCalled();

		unmount();
	});

	it('lazy-loads the Ka warning modal on first open and reuses it after closing', async () => {
		const deferred = createDeferred();
		const kaLoader = vi
			.spyOn(lockoutModalLoaders, 'loadLockoutKaWarningModal')
			.mockReturnValue(deferred.promise);
		installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Lockouts');
		await fireEvent.click(screen.getByRole('checkbox', { name: /unlock advanced writes/i }));
		await fireEvent.click(screen.getByRole('checkbox', { name: /ka band learning/i }));

		expect(kaLoader).toHaveBeenCalledTimes(1);
		await screen.findByText('Loading Ka warning...');

		deferred.resolve(await import('$lib/components/LockoutKaWarningModal.svelte'));
		await screen.findByText('Ka Lockout Learning Warning');

		await fireEvent.click(screen.getByRole('button', { name: /cancel \(recommended\)/i }));
		await waitFor(() => {
			expect(screen.queryByText('Ka Lockout Learning Warning')).toBeNull();
		});

		const kaToggle = screen.getByRole('checkbox', { name: /ka band learning/i });
		kaToggle.checked = true;
		await fireEvent.change(kaToggle);
		await screen.findByText('Ka Lockout Learning Warning');
		expect(kaLoader).toHaveBeenCalledTimes(1);

		unmount();
	});

	it('shows success message when lockout save succeeds', async () => {
		const fetchMock = installDefaultFetch([
			{ method: 'POST', match: '/api/gps/config', respond: jsonResponse({ success: true }) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('Lockouts');
		await fireEvent.click(screen.getByRole('checkbox', { name: /unlock advanced writes/i }));
		await fireEvent.change(screen.getByLabelText('Mode'), { target: { value: '1' } });
		const saveButton = screen.getByRole('button', { name: /^save$/i });
		await waitFor(() => {
			expect(saveButton).toBeEnabled();
		});
		await fireEvent.click(saveButton);

		await screen.findByText('Lockout runtime settings updated');
		expect(fetchMock.mock.calls.some(([url, init]) => url === '/api/gps/config' && init?.method === 'POST')).toBe(true);
		unmount();
	});

	it('shows API error message when lockout save fails', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/gps/config', respond: jsonResponse({ message: 'Failed lockout save' }, 500) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('Lockouts');
		await fireEvent.click(screen.getByRole('checkbox', { name: /unlock advanced writes/i }));
		await fireEvent.change(screen.getByLabelText('Mode'), { target: { value: '1' } });
		const saveButton = screen.getByRole('button', { name: /^save$/i });
		await waitFor(() => {
			expect(saveButton).toBeEnabled();
		});
		await fireEvent.click(saveButton);

		await screen.findByText(/Failed lockout save/i);
		unmount();
	});

	it('surfaces dropped legacy manual lockouts as a warning', async () => {
		installDefaultFetch([
			{
				method: 'GET',
				match: '/api/lockouts/zones',
				respond: jsonResponse({
					activeZones: [],
					pendingZones: [],
					activeCount: 0,
					activeCapacity: 0,
					activeReturned: 0,
					pendingCount: 0,
					pendingCapacity: 0,
					pendingReturned: 0,
					promotionHits: 3,
					promotionRadiusE5: 45,
					promotionFreqToleranceMHz: 8,
					learnIntervalHours: 12,
					unlearnIntervalHours: 0,
					unlearnCount: 0,
					manualDemotionMissCount: 12,
					droppedManualCount: 2
				})
			}
		]);
		const { unmount } = render(Page);

		await screen.findByText('Dropped 2 legacy manual lockout entries during migration.');
		unmount();
	});

	it('stops shared gps polling when the page unmounts', async () => {
		vi.useFakeTimers();
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('32 mph');
		expect(countCalls(fetchMock, '/api/gps/status')).toBe(1);

		unmount();
		await vi.advanceTimersByTimeAsync(7500);

		expect(countCalls(fetchMock, '/api/gps/status')).toBe(1);
	});
});
