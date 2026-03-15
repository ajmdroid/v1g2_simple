import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import DevPerfFilesPanel from './DevPerfFilesPanel.svelte';

function renderPanel(props = {}) {
	return render(DevPerfFilesPanel, {
		acknowledged: true,
		perfFiles: [],
		perfFilesLoading: false,
		perfFileActionBusy: '',
		perfFilesInfo: {
			storageReady: true,
			onSdCard: true,
			path: '/perf'
		},
		onrefresh: vi.fn(),
		ondownload: vi.fn(),
		ondelete: vi.fn(),
		...props
	});
}

describe('DevPerfFilesPanel', () => {
	it('renders the perf file list with active badge and size formatting', async () => {
		renderPanel({
			perfFiles: [{ name: 'perf-0001.csv', sizeBytes: 1536, active: true }]
		});

		expect(screen.getByText('perf-0001.csv')).toBeInTheDocument();
		expect(screen.getByText('active')).toBeInTheDocument();
		expect(screen.getByText('1.5 KB')).toBeInTheDocument();
	});

	it('calls the delete handler for the selected file', async () => {
		const ondelete = vi.fn();
		renderPanel({
			perfFiles: [{ name: 'perf-0001.csv', sizeBytes: 1536, active: false }],
			ondelete
		});

		await fireEvent.click(screen.getByRole('button', { name: /^delete$/i }));
		expect(ondelete).toHaveBeenCalledWith('perf-0001.csv');
	});

	it('renders the storage warning when perf files are unavailable', async () => {
		renderPanel({
			perfFilesInfo: {
				storageReady: false,
				onSdCard: false,
				path: '/perf'
			}
		});

		expect(screen.getByText('SD storage not ready. Perf CSV files are unavailable.')).toBeInTheDocument();
	});

	it('renders the empty state when no perf files exist', async () => {
		renderPanel();

		expect(screen.getByText('No perf CSV files found.')).toBeInTheDocument();
	});
});
