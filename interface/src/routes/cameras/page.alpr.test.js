import { render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

describe('cameras route ALPR-only surface', () => {
	afterEach(() => {
		vi.useRealTimers();
	});

	it('omits removed camera controls', async () => {
		installFetchMock([
			{
				method: 'GET',
				match: '/api/cameras/settings',
				respond: jsonResponse({
					cameraAlertsEnabled: false,
					cameraAlertRangeCm: 80467
				})
			},
			{
				method: 'GET',
				match: '/api/cameras/status',
				respond: jsonResponse({
					cameraCount: 3,
					displayActive: false,
					distanceCm: null
				})
			}
		]);

		const { unmount } = render(Page);

		await screen.findByText('ALPR Cameras');
		expect(screen.queryByText('Camera Types')).toBeNull();
		expect(screen.queryByText('Voice + Display')).toBeNull();
		expect(screen.queryByRole('button', { name: /camera arrow color/i })).toBeNull();
		expect(screen.queryByText('Close Alert Distance')).toBeNull();

		unmount();
	});

	it('polls live status after mount', async () => {
		vi.useFakeTimers();
		const fetchMock = installFetchMock([
			{
				method: 'GET',
				match: '/api/cameras/settings',
				respond: jsonResponse({
					cameraAlertsEnabled: true,
					cameraAlertRangeCm: 128748
				})
			},
			{
				method: 'GET',
				match: '/api/cameras/status',
				respond: jsonResponse({
					cameraCount: 2,
					displayActive: true,
					distanceCm: 15000
				})
			}
		]);

		const { unmount } = render(Page);
		await screen.findByText('Loaded ALPR cameras');

		const initialStatusCalls = fetchMock.mock.calls.filter(([url]) => url === '/api/cameras/status').length;
		expect(initialStatusCalls).toBe(1);

		await vi.advanceTimersByTimeAsync(2500);

		await waitFor(() => {
			const statusCalls = fetchMock.mock.calls.filter(([url]) => url === '/api/cameras/status').length;
			expect(statusCalls).toBeGreaterThan(1);
		});

		unmount();
	});
});
