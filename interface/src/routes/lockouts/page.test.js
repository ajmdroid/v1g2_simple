import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

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
					manualDemotionMissCount: 12
				})
			},
			{ method: 'POST', match: '/api/gps/config', respond: jsonResponse({ success: true }) },
		],
		jsonResponse({})
	);
}

describe('lockouts route page', () => {
	beforeEach(() => {
		global.confirm = vi.fn(() => true);
	});

	afterEach(() => {
		vi.restoreAllMocks();
	});

	it('loads lockout status/events/zones on mount', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Lockouts');
		await waitFor(() => {
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/gps/status')).toBe(true);
			expect(fetchMock.mock.calls.some(([url]) => String(url).startsWith('/api/lockouts/events'))).toBe(true);
			expect(fetchMock.mock.calls.some(([url]) => String(url).startsWith('/api/lockouts/zones'))).toBe(true);
		});

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

	it('opens manual zone editor after safety gate unlock', async () => {
		installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Lockouts');
		await fireEvent.click(screen.getByRole('checkbox', { name: /unlock advanced writes/i }));
		await fireEvent.click(screen.getByRole('button', { name: /new manual zone/i }));

		await screen.findByText('Create Manual Lockout Zone');
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
});
