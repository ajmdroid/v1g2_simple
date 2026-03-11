import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function installDefaultFetch(overrides = []) {
	return installFetchMock(
		[
			...overrides,
			{
				method: 'GET',
				match: '/api/cameras/settings',
				respond: jsonResponse({
					cameraAlertsEnabled: true,
					cameraAlertRangeCm: 160934
				})
			},
			{
				method: 'GET',
				match: '/api/cameras/status',
				respond: jsonResponse({
					cameraCount: 7,
					displayActive: true,
					distanceCm: 32000
				})
			},
			{ method: 'POST', match: '/api/cameras/settings', respond: jsonResponse({ success: true }) }
		],
		jsonResponse({})
	);
}

describe('cameras route page', () => {
	it('loads ALPR settings and status', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('ALPR Cameras');
		await screen.findByText('Loaded ALPR cameras');
		await waitFor(() => {
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/cameras/settings')).toBe(true);
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/cameras/status')).toBe(true);
		});

		unmount();
	});

	it('shows load error when settings fetch fails', async () => {
		installFetchMock(
			[
				{
					method: 'GET',
					match: '/api/cameras/settings',
					respond: () => Promise.reject(new Error('offline'))
				},
				{
					method: 'GET',
					match: '/api/cameras/status',
					respond: jsonResponse({ cameraCount: 0, displayActive: false, distanceCm: null })
				}
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await screen.findByText('Failed to load ALPR settings');
		unmount();
	});

	it('posts the reduced ALPR payload on save', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		const saveButton = await screen.findByRole('button', { name: /save alpr settings/i });
		await fireEvent.click(saveButton);
		await screen.findByText('ALPR settings saved.');

		const saveCall = fetchMock.mock.calls.find(
			([url, init]) => url === '/api/cameras/settings' && init?.method === 'POST'
		);
		expect(saveCall).toBeTruthy();
		const [, init] = saveCall;
		expect(init.body.toString()).toContain('cameraAlertsEnabled=');
		expect(init.body.toString()).toContain('cameraAlertRangeCm=');
		expect(init.body.toString()).not.toContain('cameraAlertNearRangeCm');
		expect(init.body.toString()).not.toContain('cameraVoice');

		unmount();
	});

	it('shows API error message on save failure', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/cameras/settings', respond: jsonResponse({ success: false }, 500) }
		]);
		const { unmount } = render(Page);

		const saveButton = await screen.findByRole('button', { name: /save alpr settings/i });
		await fireEvent.click(saveButton);
		await screen.findByText('Failed to save ALPR settings');

		unmount();
	});
});
