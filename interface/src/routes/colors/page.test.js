import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { cloneDefaultColors } from '$lib/utils/colors';
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
					...cloneDefaultColors(),
					hideBatteryIcon: false,
					showBatteryPercent: true,
					brightness: 123
				})
			},
			{ method: 'GET', match: '/api/settings', respond: jsonResponse({ displayStyle: 3 }) },
			{ method: 'POST', match: '/api/settings', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/displaycolors', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/displaycolors/preview', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/displaycolors/reset', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/displaycolors/clear', respond: jsonResponse({ success: true }) }
		],
		jsonResponse({})
	);
}

describe('colors route page', () => {
	beforeEach(() => {
		global.confirm = vi.fn(() => true);
	});

	afterEach(() => {
		vi.useRealTimers();
		vi.restoreAllMocks();
	});

	it('loads colors and display style on mount', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		const displayStyleSelect = await screen.findByRole('combobox');
		await waitFor(() => {
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/displaycolors')).toBe(true);
			expect(fetchMock.mock.calls.some(([url]) => url === '/api/settings')).toBe(true);
		});
		expect(displayStyleSelect).toHaveValue('3');
		expect(screen.getByText('48%')).toBeInTheDocument();

		unmount();
	});

	it('shows load error when the colors request fails', async () => {
		installFetchMock(
			[
				{ method: 'GET', match: '/api/displaycolors', respond: () => Promise.reject(new Error('offline')) },
				{ method: 'GET', match: '/api/settings', respond: jsonResponse({ displayStyle: 0 }) }
			],
			jsonResponse({})
		);
		const { unmount } = render(Page);

		await screen.findByText('Failed to load colors');
		unmount();
	});

	it('disables battery percentage when the battery icon is hidden', async () => {
		installDefaultFetch([
			{
				method: 'GET',
				match: '/api/displaycolors',
				respond: jsonResponse({
					...cloneDefaultColors(),
					hideBatteryIcon: true,
					showBatteryPercent: true
				})
			}
		]);
		const { unmount } = render(Page);

		expect(
			await screen.findByRole('checkbox', { name: /show battery percentage/i })
		).toBeDisabled();

		unmount();
	});

	it('saves display style through the settings API', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		const select = await screen.findByRole('combobox');
		await fireEvent.change(select, { target: { value: '0' } });

		await screen.findByText('Display style updated!');
		const postCall = fetchMock.mock.calls.find(
			([url, init]) => url === '/api/settings' && init?.method === 'POST'
		);
		expect(postCall).toBeTruthy();
		const [, init] = postCall;
		expect(init.body.get('displayStyle')).toBe('0');

		unmount();
	});

	it('posts colors and clears the preview after save', async () => {
		vi.useFakeTimers();
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		const saveButton = await screen.findByRole('button', { name: /save colors/i });
		await fireEvent.click(saveButton);

		await screen.findByText('Colors saved! Previewing on display...');
		await vi.advanceTimersByTimeAsync(3000);
		await waitFor(() => {
			expect(
				fetchMock.mock.calls.some(
					([url, init]) => url === '/api/displaycolors/clear' && init?.method === 'POST'
				)
			).toBe(true);
		});

		unmount();
	});

	it('runs preview and reset actions', async () => {
		const fetchMock = installDefaultFetch();
		const { unmount } = render(Page);

		await fireEvent.click(await screen.findByRole('button', { name: /^preview$/i }));
		await fireEvent.click(screen.getByRole('button', { name: /reset defaults/i }));

		await screen.findByText('Colors reset to defaults!');
		expect(
			fetchMock.mock.calls.some(
				([url, init]) => url === '/api/displaycolors/preview' && init?.method === 'POST'
			)
		).toBe(true);
		expect(
			fetchMock.mock.calls.some(
				([url, init]) => url === '/api/displaycolors/reset' && init?.method === 'POST'
			)
		).toBe(true);

		unmount();
	});
});
