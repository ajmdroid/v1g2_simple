import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function installDefaultFetch(overrides = []) {
	return installFetchMock(
		[
			...overrides,
			{ method: 'GET', match: '/api/v1/profiles', respond: jsonResponse({ profiles: [{ name: 'Daily Drive' }] }) },
			{
				method: 'GET',
				match: '/api/v1/current',
				respond: jsonResponse({ connected: true, settings: { lockoutsEnabled: true } })
			},
			{ method: 'POST', match: '/api/v1/pull', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/v1/profile', respond: jsonResponse({ success: true }) },
		],
		jsonResponse({})
	);
}

describe('profiles route page', () => {
	it('loads profiles and current settings', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('V1 Profiles');
		await screen.findByText('Daily Drive');
		await waitFor(() => {
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/profiles')).toBe(true);
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/v1/current')).toBe(true);
		});

		unmount();
	});

	it('keeps route stable when profile load fails', async () => {
		installFetchMock(
			[
				{ method: 'GET', match: '/api/v1/profiles', respond: () => Promise.reject(new Error('offline')) },
				{ method: 'GET', match: '/api/v1/current', respond: jsonResponse({ connected: false, settings: {} }) }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await screen.findByText('V1 Profiles');
		unmount();
	});

	it('opens and closes the save profile dialog', async () => {
		installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('V1 Profiles');
		const saveButtons = await screen.findAllByRole('button', { name: /^Save$/i });
		await fireEvent.click(saveButtons[0]);

		await screen.findByText('Save Profile');
		await fireEvent.click(screen.getByRole('button', { name: /cancel/i }));
		await waitFor(() => {
			expect(screen.queryByText('Save Profile')).toBeNull();
		});

		unmount();
	});

	it('shows API error message when pull fails', async () => {
		installDefaultFetch([
			{ method: 'POST', match: '/api/v1/pull', respond: jsonResponse({ error: 'bad' }, 500) }
		]);
		const { unmount } = render(Page);

		await screen.findByText('V1 Connected');
		const pullButton = await screen.findByRole('button', { name: /pull from v1/i });
		await fireEvent.click(pullButton);
		await screen.findByText('Failed to pull settings');

		unmount();
	});
});
