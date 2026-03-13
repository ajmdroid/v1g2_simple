import { fireEvent, render, screen } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { installFetchMock, jsonResponse } from '../../test/fetch-mock.js';
import Page from './+page.svelte';

function installDefaultFetch() {
	return installFetchMock(
		[
			{
				method: 'GET',
				match: '/api/v1/devices',
				respond: jsonResponse({
					devices: [
						{
							address: 'AA:BB:CC:DD:EE:FF',
							name: 'Daily Driver',
							defaultProfile: 2,
							connected: true
						}
					]
				})
			},
			{
				method: 'GET',
				match: '/api/autopush/slots',
				respond: jsonResponse({
					slots: [{ name: 'Default' }, { name: 'Highway' }, { name: 'Comfort' }]
				})
			},
			{ method: 'POST', match: '/api/v1/devices/name', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/v1/devices/profile', respond: jsonResponse({ success: true }) },
			{ method: 'POST', match: '/api/v1/devices/delete', respond: jsonResponse({ success: true }) }
		],
		jsonResponse({})
	);
}

describe('devices route page', () => {
	beforeEach(() => {
		global.confirm = vi.fn(() => true);
	});

	afterEach(() => {
		vi.restoreAllMocks();
	});

	it('loads saved devices and opens the rename form', async () => {
		installDefaultFetch();
		const { unmount } = render(Page);

		await screen.findByText('Saved V1 Devices');
		await screen.findByText('Daily Driver');
		await screen.findByText('Connected');

		expect(await screen.findByRole('combobox')).toHaveValue('2');

		await fireEvent.click(screen.getByRole('button', { name: /^rename$/i }));

		expect(await screen.findByDisplayValue('Daily Driver')).toBeInTheDocument();
		expect(screen.getByRole('button', { name: /^save$/i })).toBeInTheDocument();

		unmount();
	});
});
