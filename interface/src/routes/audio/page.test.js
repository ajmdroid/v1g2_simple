import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function installDefaultFetch(overrides = []) {
	return installFetchMock(
		[
			...overrides,
			{
				method: 'GET',
				match: '/api/displaycolors',
				respond: jsonResponse({
					voiceAlertMode: 3,
					voiceDirectionEnabled: true,
					announceBogeyCount: true,
					muteVoiceIfVolZero: false,
					voiceVolume: 72,
					announceSecondaryAlerts: true,
					secondaryLaser: true,
					secondaryKa: true,
					secondaryK: false,
					secondaryX: false,
					alertVolumeFadeEnabled: true,
					alertVolumeFadeDelaySec: 4,
					alertVolumeFadeVolume: 2
				})
			},
			{ method: 'POST', match: '/api/displaycolors', respond: jsonResponse({ success: true }) }
		],
		jsonResponse({})
	);
}

describe('audio route page', () => {
	afterEach(() => {
		vi.restoreAllMocks();
	});

	it('loads audio settings from the display colors API', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByRole('button', { name: /save audio settings/i });
		await waitFor(() => {
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/displaycolors')).toBe(true);
		});
		expect(screen.getByText(/Ka 34\.712 ahead 2 bogeys/)).toBeInTheDocument();
		expect(screen.getByText('72%')).toBeInTheDocument();

		unmount();
	});

	it('shows load error when the settings request fails', async () => {
		installFetchMock(
			[{ method: 'GET', match: '/api/displaycolors', respond: () => Promise.reject(new Error('offline')) }],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await screen.findByText('Failed to load settings');
		unmount();
	});

	it('disables dependent toggles when voice alerts are disabled', async () => {
		installDefaultFetch([
			{
				method: 'GET',
				match: '/api/displaycolors',
				respond: jsonResponse({
					voiceAlertMode: 0,
					voiceDirectionEnabled: true,
					announceBogeyCount: true,
					muteVoiceIfVolZero: true,
					announceSecondaryAlerts: true
				})
			}
		]);
		const { unmount } = render(Page);

		await screen.findByRole('button', { name: /save audio settings/i });
		expect(screen.getByText(/\(silent\)/i)).toBeInTheDocument();
		expect(screen.getByRole('checkbox', { name: /include direction/i })).toBeDisabled();
		expect(screen.getByRole('checkbox', { name: /announce bogey count/i })).toBeDisabled();
		expect(screen.getByRole('checkbox', { name: /mute voice at volume 0/i })).toBeDisabled();
		expect(screen.getByRole('checkbox', { name: /announce secondary alerts/i })).toBeDisabled();

		unmount();
	});

	it('posts audio settings and shows success feedback on save', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		const saveButton = await screen.findByRole('button', { name: /save audio settings/i });
		await fireEvent.click(saveButton);

		await screen.findByText('Audio settings saved!');
		const postCall = fetchMock.mock.calls.find(
			([url, init]) => url === '/api/displaycolors' && init?.method === 'POST'
		);
		expect(postCall).toBeTruthy();
		const [, init] = postCall;
		expect(init.body.get('voiceAlertMode')).toBe('3');
		expect(init.body.get('voiceVolume')).toBe('72');
		expect(init.body.get('announceSecondaryAlerts')).toBe('true');
		expect(init.body.get('alertVolumeFadeEnabled')).toBe('true');

		unmount();
	});

	it('shows an error message when save fails', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/displaycolors', respond: jsonResponse({ success: false }, 500) }
		]);
		const { unmount } = render(Page);

		const saveButton = await screen.findByRole('button', { name: /save audio settings/i });
		await fireEvent.click(saveButton);

		await screen.findByText('Failed to save settings');
		unmount();
	});
});
