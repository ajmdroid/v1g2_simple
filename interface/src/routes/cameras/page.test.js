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
					enabled: true,
					cameraAlertRangeCm: 1600,
					cameraAlertNearRangeCm: 400,
					cameraVoiceEnabled: true,
					cameraVoiceNearEnabled: true,
					cameraVoiceSpeedEnabled: true,
					cameraVoiceRedEnabled: true,
					cameraVoiceBusEnabled: true,
					cameraVoiceAlprEnabled: true,
					cameraDisplayEnabled: true,
					cameraNotifyOnSignalMissing: true,
					cameraSignalMissingThresholdSec: 15,
					cameraSignalMissingCooldownSec: 30,
					colorCameraArrow: 0x780f,
					colorCameraText: 0x780f
				})
			},
			{ method: 'GET', match: '/api/cameras/status', respond: jsonResponse({ displayActive: false, type: 'speed', distanceCm: 0 }) },
			{ method: 'POST', match: '/api/cameras/settings', respond: jsonResponse({ success: true }) },
		],
		jsonResponse({})
	);
}

describe('cameras route page', () => {
	it('loads camera settings and status', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Camera Alerts');
		await waitFor(() => {
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/cameras/settings')).toBe(true);
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/cameras/status')).toBe(true);
		});

		unmount();
	});

	it('shows load error when camera settings fetch fails', async () => {
		installFetchMock(
			[
				{ method: 'GET', match: '/api/cameras/settings', respond: () => Promise.reject(new Error('offline')) },
				{ method: 'GET', match: '/api/cameras/status', respond: jsonResponse({ displayActive: false }) }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await screen.findByText('Failed to load camera settings');
		unmount();
	});

	it('opens and closes the color picker modal', async () => {
		installDefaultFetch();
		const { unmount } = render(Page);

		const colorButton = await screen.findByRole('button', { name: /camera arrow color/i });
		await fireEvent.click(colorButton);
		await screen.findByText('Camera Arrow Color');
		await fireEvent.click(screen.getByRole('button', { name: /cancel/i }));
		await waitFor(() => {
			expect(screen.queryByText('Camera Arrow Color')).toBeNull();
		});

		unmount();
	});

	it('applies the color picker modal value and closes the dialog', async () => {
		installDefaultFetch();
		const { unmount } = render(Page);

		const colorButton = await screen.findByRole('button', { name: /camera arrow color/i });
		await fireEvent.click(colorButton);
		await screen.findByText('Camera Arrow Color');
		await fireEvent.click(screen.getByRole('button', { name: /^Apply$/i }));
		await waitFor(() => {
			expect(screen.queryByText('Camera Arrow Color')).toBeNull();
		});

		unmount();
	});

	it('shows success message on save success', async () => {
		const fetchMock = installDefaultFetch([
			{ method: 'POST', match: '/api/cameras/settings', respond: jsonResponse({ success: true }) }
		]);
		const { unmount } = render(Page);

		const saveButton = await screen.findByRole('button', { name: /save camera settings/i });
		await fireEvent.click(saveButton);
		await screen.findByText('Camera settings saved.');
		expect(fetchMock.mock.calls.some(([url, init]) => url === '/api/cameras/settings' && init?.method === 'POST')).toBe(true);
		unmount();
	});

	it('shows API error message on save failure', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/cameras/settings', respond: jsonResponse({ success: false }, 500) }
		]);
		const { unmount } = render(Page);

		const saveButton = await screen.findByRole('button', { name: /save camera settings/i });
		await fireEvent.click(saveButton);
		await screen.findByText('Failed to save camera settings');

		unmount();
	});
});
