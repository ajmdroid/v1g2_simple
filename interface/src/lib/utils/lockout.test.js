import { describe, expect, it } from 'vitest';
import {
	LEARNER_RADIUS_E5_MAX,
	LEARNER_RADIUS_E5_DEFAULT,
	bandNameToMask,
	clampHdopX10,
	clampIntervalHours,
	clampLearnerFreqToleranceMHz,
	clampLearnerPromotionHits,
	clampMinLearnerSpeed,
	clampManualDemotionMissCount,
	clampUnlearnCount,
	clampU16,
	defaultZoneEditorState,
	feetToRadiusE5,
	formatBandMask,
	formatBootTime,
	formatCoordinate,
	formatEpochMs,
	formatDirectionSummary,
	formatFixAgeMs,
	formatHdop,
	formatIntervalLabel,
	formatRadiusFeet,
	formatZoneRadiusFeet,
	lockoutZoneSourceLabel,
	mapHrefFromZone,
	normalizeDirectionMode,
	normalizeLearnerRadiusFeet,
	radiusE5ToFeet,
	signalMapHref
} from './lockout.js';

describe('lockout utilities', () => {
	it('clamps numeric settings into supported firmware ranges', () => {
		expect(clampU16(-1)).toBe(0);
		expect(clampU16(70000)).toBe(65535);
		expect(clampLearnerPromotionHits(1)).toBe(2);
		expect(clampLearnerPromotionHits(9)).toBe(6);
		expect(clampHdopX10(4)).toBe(10);
		expect(clampHdopX10(500)).toBe(100);
		expect(clampMinLearnerSpeed(-1)).toBe(0);
		expect(clampMinLearnerSpeed(25)).toBe(20);
		expect(clampLearnerFreqToleranceMHz(1)).toBe(2);
		expect(clampLearnerFreqToleranceMHz(99)).toBe(20);
		expect(clampUnlearnCount(-1)).toBe(0);
		expect(clampUnlearnCount(99)).toBe(10);
		expect(clampManualDemotionMissCount(-10)).toBe(0);
		expect(clampManualDemotionMissCount(11)).toBe(25);
		expect(clampManualDemotionMissCount(100)).toBe(50);
	});

	it('buckets interval and radius inputs to supported lockout values', () => {
		expect(clampIntervalHours(-1)).toBe(0);
		expect(clampIntervalHours(2)).toBe(4);
		expect(clampIntervalHours(18)).toBe(24);
		expect(formatIntervalLabel(2)).toBe('4h');
		expect(feetToRadiusE5('not-a-number')).toBe(LEARNER_RADIUS_E5_DEFAULT);
		expect(normalizeLearnerRadiusFeet(99999)).toBe(radiusE5ToFeet(LEARNER_RADIUS_E5_MAX));
	});

	it('formats zone metadata for display and maps', () => {
		expect(formatBandMask(0x06)).toBe('Ka+K');
		expect(formatDirectionSummary({ directionMode: 'forward', headingDeg: 182.4, headingToleranceDeg: 60 })).toBe(
			'Forward 182° ±60°'
		);
		expect(normalizeDirectionMode(' Reverse ')).toBe('reverse');
		expect(formatDirectionSummary({ directionMode: 'unknown' })).toBe('All');
		expect(formatZoneRadiusFeet({ radiusE5: 45, radiusM: 100 })).toBe(`${radiusE5ToFeet(45)} ft`);
		expect(signalMapHref({ locationValid: true, latitude: 35.1, longitude: -80.8 })).toBe(
			'https://maps.google.com/?q=35.1,-80.8'
		);
		expect(signalMapHref({ locationValid: false, latitude: 35.1, longitude: -80.8 })).toBe('');
		expect(mapHrefFromZone({ latitude: 35.1, longitude: -80.8 })).toBe('https://maps.google.com/?q=35.1,-80.8');
		expect(mapHrefFromZone({ latitude: null, longitude: -80.8 })).toBe('');
	});

	it('maps band/source helpers and default editor state', () => {
		expect(bandNameToMask('Ka Band')).toBe(0x02);
		expect(bandNameToMask('Laser')).toBe(0x01);
		expect(bandNameToMask('unknown')).toBe(0x04);
		expect(lockoutZoneSourceLabel({ manual: true, learned: true })).toBe('manual+learned');
		expect(lockoutZoneSourceLabel({ learned: true })).toBe('learned');
		expect(defaultZoneEditorState()).toMatchObject({
			bandMask: 0x04,
			confidence: 100,
			directionMode: 'all',
			headingToleranceDeg: 45
		});
	});

	it('formats numeric telemetry for the UI', () => {
		const ts = 1_700_000_000_000;
		expect(formatCoordinate(35.123456)).toBe('35.12346');
		expect(formatCoordinate(null)).toBe('—');
		expect(formatEpochMs(ts)).toBe(new Date(ts).toLocaleString());
		expect(formatEpochMs(0)).toBe('—');
		expect(formatFixAgeMs(1500)).toBe('1.5s');
		expect(formatFixAgeMs(undefined)).toBe('—');
		expect(formatBootTime(3200)).toBe('3.2s');
		expect(formatBootTime(undefined)).toBe('—');
		expect(formatHdop(1.26)).toBe('1.3');
		expect(formatHdop(undefined)).toBe('—');
		expect(formatRadiusFeet(20)).toBe('66 ft');
		expect(formatRadiusFeet(0)).toBe('—');
	});
});
