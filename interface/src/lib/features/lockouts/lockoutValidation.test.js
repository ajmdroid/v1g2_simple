import { describe, expect, it } from 'vitest';

import { LOCKOUT_PRESET_BALANCED_BLEND, defaultZoneEditorState, radiusE5ToFeet } from '$lib/utils/lockout';
import {
	buildZoneEditorFromObservation,
	buildZoneEditorFromZone,
	buildZoneEditorPayload,
	deriveLockoutConfigFromStatus,
	describeZoneTotals,
	lockoutConfigMatchesRuntime,
	resetZoneEditorState,
	stageLearnerPresetValues
} from './lockoutValidation.js';

describe('lockoutValidation', () => {
	it('returns null when lockout config is already initialized and dirty', () => {
		expect(deriveLockoutConfigFromStatus({ lockout: {} }, true, true)).toBeNull();
	});

	it('derives config using nested values first, legacy fallbacks second, and normalizes advisory mode', () => {
		const result = deriveLockoutConfigFromStatus(
			{
				lockout: {
					modeRaw: 2,
					coreGuardEnabled: true,
					maxQueueDrops: 7,
					maxPerfDrops: 8,
					maxEventBusDrops: 9,
					learnerPromotionHits: 1,
					learnerLearnIntervalHours: 2,
					learnerUnlearnIntervalHours: 18,
					learnerUnlearnCount: 99,
					manualDemotionMissCount: 11,
					kLearningEnabled: false,
					preQuietBufferE5: 12
				},
				lockoutLearnerRadiusE5: 45,
				lockoutLearnerFreqToleranceMHz: 1,
				lockoutKaLearningEnabled: true,
				gpsLockoutPreQuiet: true,
				gpsLockoutMaxHdopX10: 500,
				gpsLockoutMinLearnerSpeedMph: -2
			},
			false,
			false
		);

		expect(result).toEqual({
			modeRaw: 1,
			coreGuardEnabled: true,
			maxQueueDrops: 7,
			maxPerfDrops: 8,
			maxEventBusDrops: 9,
			learnerPromotionHits: 2,
			learnerRadiusFt: radiusE5ToFeet(45),
			learnerFreqToleranceMHz: 2,
			learnerLearnIntervalHours: 4,
			learnerUnlearnIntervalHours: 24,
			learnerUnlearnCount: 10,
			manualDemotionMissCount: 25,
			kaLearningEnabled: true,
			kLearningEnabled: false,
			xLearningEnabled: true,
			preQuiet: true,
			preQuietBufferE5: 12,
			maxHdopX10: 100,
			minLearnerSpeedMph: 0
		});
	});

	it('builds zone editors from zones and observations', () => {
		expect(
			buildZoneEditorFromZone({
				slot: 3,
				latitude: 35.123456,
				longitude: -80.654321,
				radiusE5: 45,
				bandMask: 0x02,
				frequencyMHz: 34700,
				frequencyToleranceMHz: 9,
				confidence: 80,
				directionMode: 'forward',
				headingDeg: 181.6,
				headingToleranceDeg: 20
			})
		).toEqual({
			slot: 3,
			editor: {
				latitude: '35.12346',
				longitude: '-80.65432',
				radiusFt: radiusE5ToFeet(45),
				bandMask: 0x02,
				frequencyMHz: '34700',
				frequencyToleranceMHz: 9,
				confidence: 80,
				directionMode: 'forward',
				headingDeg: '182',
				headingToleranceDeg: 20
			}
		});

		expect(buildZoneEditorFromObservation({ locationValid: false })).toEqual({
			error: 'Cannot create zone: observation has no GPS fix.'
		});
	});

	it('validates zone editor payload inputs and clamps payload fields', () => {
		expect(buildZoneEditorPayload({ latitude: '91', longitude: '0', directionMode: 'all' })).toEqual({
			error: 'Latitude must be between -90 and 90.'
		});

		expect(
			buildZoneEditorPayload({
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
			})
		).toEqual({ error: 'Frequency must be a positive MHz value.' });

		expect(
			buildZoneEditorPayload({
				latitude: '35.1',
				longitude: '-80.8',
				radiusFt: 'bad',
				bandMask: 'bad',
				frequencyMHz: '34700.4',
				frequencyToleranceMHz: 70000,
				confidence: 500,
				directionMode: 'forward',
				headingDeg: '400',
				headingToleranceDeg: -10
			})
		).toEqual({ error: 'Heading must be between 0 and 359 for directional zones.' });

		expect(
			buildZoneEditorPayload({
				latitude: '35.1',
				longitude: '-80.8',
				radiusFt: 'bad',
				bandMask: 'bad',
				frequencyMHz: '34700.4',
				frequencyToleranceMHz: 70000,
				confidence: 500,
				directionMode: 'all',
				headingDeg: '',
				headingToleranceDeg: -10
			})
		).toEqual({
			payload: {
				latitude: 35.1,
				longitude: -80.8,
				bandMask: 0x04,
				radiusE5: 135,
				frequencyMHz: 34700,
				frequencyToleranceMHz: 65535,
				confidence: 255,
				directionMode: 'all',
				headingDeg: null,
				headingToleranceDeg: 0
			}
		});
	});

	it('stages learner presets, compares runtime values, and reports zone totals', () => {
		const lockoutConfig = {
			modeRaw: 1,
			coreGuardEnabled: true,
			maxQueueDrops: 10,
			maxPerfDrops: 20,
			maxEventBusDrops: 30,
			learnerPromotionHits: 3,
			learnerRadiusFt: radiusE5ToFeet(135),
			learnerFreqToleranceMHz: 10,
			learnerLearnIntervalHours: 0,
			learnerUnlearnIntervalHours: 0,
			learnerUnlearnCount: 0,
			manualDemotionMissCount: 0,
			kaLearningEnabled: false,
			kLearningEnabled: true,
			xLearningEnabled: true,
			preQuietBufferE5: 0,
			maxHdopX10: 50,
			minLearnerSpeedMph: 5
		};

		stageLearnerPresetValues(lockoutConfig, LOCKOUT_PRESET_BALANCED_BLEND);

		expect(lockoutConfig).toMatchObject({
			learnerPromotionHits: 3,
			learnerLearnIntervalHours: 4,
			learnerFreqToleranceMHz: 10,
			learnerRadiusFt: radiusE5ToFeet(135),
			learnerUnlearnCount: 5,
			learnerUnlearnIntervalHours: 4,
			manualDemotionMissCount: 25
		});

		expect(
			lockoutConfigMatchesRuntime(lockoutConfig, {
				modeRaw: 1,
				coreGuardEnabled: true,
				maxQueueDrops: 10,
				maxPerfDrops: 20,
				maxEventBusDrops: 30,
				learnerPromotionHits: 3,
				learnerRadiusE5: 135,
				learnerFreqToleranceMHz: 10,
				learnerLearnIntervalHours: 4,
				learnerUnlearnIntervalHours: 4,
				learnerUnlearnCount: 5,
				manualDemotionMissCount: 25,
				kaLearningEnabled: false,
				kLearningEnabled: true,
				xLearningEnabled: true,
				preQuietBufferE5: 0,
				maxHdopX10: 50,
				minLearnerSpeedMph: 5
			})
		).toBe(true);
		expect(lockoutConfigMatchesRuntime(lockoutConfig, { modeRaw: 0 })).toBe(false);
		expect(describeZoneTotals({ activeCount: 2, pendingCount: 1 })).toEqual({
			zoneCount: 2,
			pendingCount: 1,
			desc: '2 active + 1 pending'
		});
		expect(resetZoneEditorState()).toEqual(defaultZoneEditorState());
	});
});
