import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { radiusE5ToFeet } from '$lib/utils/lockout';
import {
	clearAllZonesRequest,
	deleteZoneRequest,
	exportLockoutZonesRequest,
	importLockoutZonesFromFile,
	saveLockoutConfigRequest,
	saveZoneEditorRequest
} from './lockoutRequests.js';

function makeResponse({
	ok = true,
	status = 200,
	json = {},
	text = '',
	contentType = 'application/json'
} = {}) {
	return {
		ok,
		status,
		headers: {
			get: vi.fn((key) => (key === 'content-type' ? contentType : null))
		},
		json: vi.fn().mockResolvedValue(json),
		text: vi.fn().mockResolvedValue(text)
	};
}

describe('lockoutRequests', () => {
	beforeEach(() => {
		vi.restoreAllMocks();
	});

	afterEach(() => {
		vi.restoreAllMocks();
	});

	it('posts lockout config with the existing route and returns normalized values', async () => {
		const fetchWithTimeout = vi.fn().mockResolvedValue(makeResponse());

		const result = await saveLockoutConfigRequest(fetchWithTimeout, {
			modeRaw: 1,
			coreGuardEnabled: true,
			maxQueueDrops: 70000,
			maxPerfDrops: 2,
			maxEventBusDrops: 3,
			learnerPromotionHits: 1,
			learnerRadiusFt: radiusE5ToFeet(45),
			learnerFreqToleranceMHz: 99,
			learnerLearnIntervalHours: 2,
			learnerUnlearnIntervalHours: 18,
			learnerUnlearnCount: 99,
			manualDemotionMissCount: 11,
			kaLearningEnabled: true,
			kLearningEnabled: false,
			xLearningEnabled: true,
			preQuiet: true,
			preQuietBufferE5: 7,
			maxHdopX10: 500,
			minLearnerSpeedMph: -1
		});

		expect(fetchWithTimeout).toHaveBeenCalledWith('/api/gps/config', {
			method: 'POST',
			headers: { 'Content-Type': 'application/json' },
			body: JSON.stringify({
				lockoutMode: 1,
				lockoutCoreGuardEnabled: true,
				lockoutMaxQueueDrops: 65535,
				lockoutMaxPerfDrops: 2,
				lockoutMaxEventBusDrops: 3,
				lockoutLearnerPromotionHits: 2,
				lockoutLearnerRadiusE5: 45,
				lockoutLearnerFreqToleranceMHz: 20,
				lockoutLearnerLearnIntervalHours: 4,
				lockoutLearnerUnlearnIntervalHours: 24,
				lockoutLearnerUnlearnCount: 10,
				lockoutManualDemotionMissCount: 25,
				lockoutKaLearningEnabled: true,
				lockoutKLearningEnabled: false,
				lockoutXLearningEnabled: true,
				lockoutPreQuiet: true,
				lockoutPreQuietBufferE5: 7,
				lockoutMaxHdopX10: 100,
				lockoutMinLearnerSpeedMph: 0
			})
		});
		expect(result).toEqual({
			ok: true,
			normalized: {
				learnerPromotionHits: 2,
				learnerRadiusFt: radiusE5ToFeet(45),
				learnerFreqToleranceMHz: 20,
				learnerLearnIntervalHours: 4,
				learnerUnlearnIntervalHours: 24,
				learnerUnlearnCount: 10,
				manualDemotionMissCount: 25,
				kaLearningEnabled: true,
				preQuiet: true,
				maxHdopX10: 100,
				minLearnerSpeedMph: 0
			}
		});
	});

	it('parses JSON and text error bodies without changing fallback messages', async () => {
		const jsonError = await saveLockoutConfigRequest(
			vi.fn().mockResolvedValue(makeResponse({ ok: false, status: 500, json: { message: 'Nope' } })),
			{}
		);
		const textError = await saveLockoutConfigRequest(
			vi.fn().mockResolvedValue(
				makeResponse({ ok: false, status: 502, text: 'Gateway down', contentType: 'text/plain' })
			),
			{}
		);

		expect(jsonError).toEqual({ ok: false, error: 'Nope' });
		expect(textError).toEqual({ ok: false, error: 'Gateway down' });
	});

	it('selects create and update zone endpoints and preserves slot messaging', async () => {
		const fetchWithTimeout = vi
			.fn()
			.mockResolvedValueOnce(makeResponse({ json: { slot: 8 } }))
			.mockResolvedValueOnce(makeResponse({ json: {} }));

		const createResult = await saveZoneEditorRequest(
			fetchWithTimeout,
			{
				latitude: '35.1',
				longitude: '-80.8',
				radiusFt: radiusE5ToFeet(45),
				bandMask: 0x04,
				frequencyMHz: '',
				frequencyToleranceMHz: 10,
				confidence: 100,
				directionMode: 'all',
				headingDeg: '',
				headingToleranceDeg: 45
			},
			null
		);
		const updateResult = await saveZoneEditorRequest(
			fetchWithTimeout,
			{
				latitude: '35.1',
				longitude: '-80.8',
				radiusFt: radiusE5ToFeet(45),
				bandMask: 0x04,
				frequencyMHz: '',
				frequencyToleranceMHz: 10,
				confidence: 100,
				directionMode: 'all',
				headingDeg: '',
				headingToleranceDeg: 45
			},
			12
		);

		expect(fetchWithTimeout.mock.calls[0][0]).toBe('/api/lockouts/zones/create');
		expect(fetchWithTimeout.mock.calls[1][0]).toBe('/api/lockouts/zones/update');
		expect(JSON.parse(fetchWithTimeout.mock.calls[1][1].body)).toMatchObject({ slot: 12 });
		expect(createResult).toEqual({ ok: true, message: 'Created lockout zone 8' });
		expect(updateResult).toEqual({ ok: true, message: 'Updated lockout zone 12' });
	});

	it('rejects invalid delete requests before making a network call', async () => {
		const fetchWithTimeout = vi.fn();
		const result = await deleteZoneRequest(fetchWithTimeout, { slot: -1 }, () => 'zone');

		expect(result).toEqual({ ok: false, error: 'Invalid zone slot.' });
		expect(fetchWithTimeout).not.toHaveBeenCalled();
	});

	it('exports zones through the existing download flow and reports zone counts', async () => {
		const fetchWithTimeout = vi.fn().mockResolvedValue(
			makeResponse({
				contentType: 'text/plain',
				text: JSON.stringify({ zones: [{}, {}] })
			})
		);
		const createObjectURL = vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:test');
		const revokeObjectURL = vi.spyOn(URL, 'revokeObjectURL').mockImplementation(() => {});
		const click = vi.fn();
		const createElement = vi.spyOn(document, 'createElement').mockImplementation((tag) => {
			if (tag !== 'a') return document.createElement(tag);
			return {
				href: '',
				download: '',
				click,
				remove: vi.fn()
			};
		});
		const appendChild = vi.spyOn(document.body, 'appendChild').mockImplementation(() => {});

		const result = await exportLockoutZonesRequest(fetchWithTimeout);

		expect(result).toEqual({ ok: true, message: 'Exported lockout zones (2 zones).' });
		expect(createObjectURL).toHaveBeenCalledTimes(1);
		expect(click).toHaveBeenCalledTimes(1);
		expect(revokeObjectURL).toHaveBeenCalledWith('blob:test');
		expect(createElement).toHaveBeenCalledWith('a');
		expect(appendChild).toHaveBeenCalledTimes(1);
	});

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

	it('supports replace cancel, merge export failure, and merge dedupe behavior', async () => {
		const file = {
			name: 'zones.json',
			text: vi.fn().mockResolvedValue(
				JSON.stringify({
					zones: [
						{ lat: 1, lon: 2, band: 'K' },
						{ lat: 3, lon: 4, band: 'Ka' }
					]
				})
			)
		};

		const replaceCancelled = await importLockoutZonesFromFile(vi.fn(), file, vi.fn(() => false));
		expect(replaceCancelled).toEqual({ ok: false, cancelled: true });

		const exportFailure = await importLockoutZonesFromFile(
			vi.fn().mockResolvedValue(makeResponse({ ok: false, status: 503, json: {} })),
			file,
			vi.fn(() => true)
		);
		expect(exportFailure).toEqual({
			ok: false,
			error: 'Failed to fetch current zones for merge (503)'
		});

		const fetchWithTimeout = vi
			.fn()
			.mockResolvedValueOnce(
				makeResponse({
					json: {
						zones: [{ lat: 1, lon: 2, band: 'K' }]
					}
				})
			)
			.mockResolvedValueOnce(makeResponse({ json: {} }));

		const merged = await importLockoutZonesFromFile(fetchWithTimeout, file, vi.fn(() => true));

		expect(fetchWithTimeout.mock.calls[0][0]).toBe('/api/lockouts/zones/export');
		expect(fetchWithTimeout.mock.calls[1][0]).toBe('/api/lockouts/zones/import');
		expect(JSON.parse(fetchWithTimeout.mock.calls[1][1].body)).toEqual({
			_type: 'v1simple_lockout_zones',
			_version: 1,
			zones: [
				{ lat: 1, lon: 2, band: 'K' },
				{ lat: 3, lon: 4, band: 'Ka' }
			]
		});
		expect(merged).toEqual({
			ok: true,
			message: 'Merged 1 new zones into 1 existing (1 duplicates skipped).'
		});
	});

	it('clears zones and keeps pending clear as best effort', async () => {
		const fetchWithTimeout = vi
			.fn()
			.mockResolvedValueOnce(makeResponse({ json: {} }))
			.mockRejectedValueOnce(new Error('clear failed'));

		const result = await clearAllZonesRequest(fetchWithTimeout, 2);

		expect(fetchWithTimeout).toHaveBeenNthCalledWith(1, '/api/lockouts/zones/import', {
			method: 'POST',
			headers: { 'Content-Type': 'application/json' },
			body: JSON.stringify({ _type: 'v1simple_lockout_zones', _version: 1, zones: [] })
		});
		expect(fetchWithTimeout).toHaveBeenNthCalledWith(2, '/api/lockouts/pending/clear', {
			method: 'POST'
		});
		expect(result).toEqual({ ok: true });
	});
});
