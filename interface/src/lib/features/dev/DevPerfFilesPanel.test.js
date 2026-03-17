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
			path: '/perf',
			loggingActive: false,
			activeFile: '',
			fileOpsBlocked: false,
			fileOpsBlockedReason: '',
			fileOpsBlockedReasonCode: ''
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
			perfFiles: [
				{
					name: 'perf-0001.csv',
					sizeBytes: 1536,
					active: false,
					downloadAllowed: true,
					deleteAllowed: true
				}
			],
			ondelete
		});

		await fireEvent.click(screen.getByRole('button', { name: /^delete$/i }));
		expect(ondelete).toHaveBeenCalledWith('perf-0001.csv');
	});

	it('disables perf file actions when the API marks them protected', async () => {
		const ondelete = vi.fn();
		const ondownload = vi.fn();
		renderPanel({
			perfFiles: [
				{
					name: 'perf-0001.csv',
					sizeBytes: 1536,
					active: true,
					downloadAllowed: false,
					deleteAllowed: false,
					blockedReason: 'Perf logging active'
				}
			],
			perfFilesInfo: {
				storageReady: true,
				onSdCard: true,
				path: '/perf',
				loggingActive: true,
				activeFile: 'perf-0001.csv',
				fileOpsBlocked: true,
				fileOpsBlockedReason: 'Perf logging active',
				fileOpsBlockedReasonCode: 'perf_logging_active'
			},
			ondelete,
			ondownload
		});

		expect(screen.getByText(/download and delete are temporarily unavailable/i)).toBeInTheDocument();
		expect(screen.getByRole('button', { name: /^download$/i })).toBeDisabled();
		expect(screen.getByRole('button', { name: /^delete$/i })).toBeDisabled();
		expect(ondelete).not.toHaveBeenCalled();
		expect(ondownload).not.toHaveBeenCalled();
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
