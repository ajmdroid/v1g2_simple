import { describe, expect, it, vi } from 'vitest';

import { importLockoutZonesFromFile } from './lockoutPageHelpers.js';

describe('lockout page helpers', () => {
	it('returns a friendly error when the import file cannot be read', async () => {
		const fetchWithTimeout = vi.fn();
		const confirmFn = vi.fn();
		const file = {
			name: 'broken.json',
			text: vi.fn().mockRejectedValue(new Error('read failed'))
		};

		const result = await importLockoutZonesFromFile(fetchWithTimeout, file, confirmFn);

		expect(result).toEqual({ ok: false, error: 'Failed to read lockout zones file.' });
		expect(file.text).toHaveBeenCalledTimes(1);
		expect(confirmFn).not.toHaveBeenCalled();
		expect(fetchWithTimeout).not.toHaveBeenCalled();
	});

	it('keeps invalid JSON errors distinct from file read failures', async () => {
		const fetchWithTimeout = vi.fn();
		const confirmFn = vi.fn();
		const file = {
			name: 'invalid.json',
			text: vi.fn().mockResolvedValue('{not-json')
		};

		const result = await importLockoutZonesFromFile(fetchWithTimeout, file, confirmFn);

		expect(result).toEqual({ ok: false, error: 'Invalid JSON file.' });
		expect(file.text).toHaveBeenCalledTimes(1);
		expect(confirmFn).not.toHaveBeenCalled();
		expect(fetchWithTimeout).not.toHaveBeenCalled();
	});
});
