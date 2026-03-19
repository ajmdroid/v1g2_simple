import {
	GPS_MAX_HDOP_X10_DEFAULT,
	GPS_MIN_LEARNER_SPEED_MPH_DEFAULT,
	LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT,
	LEARNER_PROMOTION_HITS_DEFAULT,
	LEARNER_RADIUS_E5_DEFAULT,
	LEARNER_UNLEARN_COUNT_DEFAULT,
	bandNameToMask,
	clampHdopX10,
	clampInt,
	clampIntervalHours,
	clampLearnerFreqToleranceMHz,
	clampLearnerPromotionHits,
	clampLearnerRadiusE5,
	clampManualDemotionMissCount,
	clampMinLearnerSpeed,
	clampU16,
	clampUnlearnCount,
	defaultZoneEditorState,
	feetToRadiusE5,
	normalizeDirectionMode,
	radiusE5ToFeet
} from '$lib/utils/lockout';

export function deriveLockoutConfigFromStatus(data, lockoutConfigInitialized, lockoutConfigDirty) {
	const lockout = data?.lockout;
	if (!lockout) return null;
	if (lockoutConfigInitialized && lockoutConfigDirty) return null;

	const learnerPromotionHits =
		typeof lockout.learnerPromotionHits === 'number'
			? lockout.learnerPromotionHits
			: typeof data?.lockoutLearnerPromotionHits === 'number'
				? data.lockoutLearnerPromotionHits
				: LEARNER_PROMOTION_HITS_DEFAULT;
	const learnerRadiusE5 =
		typeof lockout.learnerRadiusE5 === 'number'
			? lockout.learnerRadiusE5
			: typeof data?.lockoutLearnerRadiusE5 === 'number'
				? data.lockoutLearnerRadiusE5
				: LEARNER_RADIUS_E5_DEFAULT;
	const learnerFreqToleranceMHz =
		typeof lockout.learnerFreqToleranceMHz === 'number'
			? lockout.learnerFreqToleranceMHz
			: typeof data?.lockoutLearnerFreqToleranceMHz === 'number'
				? data.lockoutLearnerFreqToleranceMHz
				: LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT;
	const learnerLearnIntervalHours =
		typeof lockout.learnerLearnIntervalHours === 'number'
			? lockout.learnerLearnIntervalHours
			: typeof data?.lockoutLearnerLearnIntervalHours === 'number'
				? data.lockoutLearnerLearnIntervalHours
				: 0;
	const learnerUnlearnIntervalHours =
		typeof lockout.learnerUnlearnIntervalHours === 'number'
			? lockout.learnerUnlearnIntervalHours
			: typeof data?.lockoutLearnerUnlearnIntervalHours === 'number'
				? data.lockoutLearnerUnlearnIntervalHours
				: 0;
	const learnerUnlearnCount =
		typeof lockout.learnerUnlearnCount === 'number'
			? lockout.learnerUnlearnCount
			: typeof data?.lockoutLearnerUnlearnCount === 'number'
				? data.lockoutLearnerUnlearnCount
				: LEARNER_UNLEARN_COUNT_DEFAULT;
	const manualDemotionMissCount =
		typeof lockout.manualDemotionMissCount === 'number'
			? lockout.manualDemotionMissCount
			: typeof data?.lockoutManualDemotionMissCount === 'number'
				? data.lockoutManualDemotionMissCount
				: 0;
	const kaLearningEnabled =
		typeof lockout.kaLearningEnabled === 'boolean'
			? lockout.kaLearningEnabled
			: typeof data?.lockoutKaLearningEnabled === 'boolean'
				? data.lockoutKaLearningEnabled
				: typeof data?.gpsLockoutKaLearningEnabled === 'boolean'
					? data.gpsLockoutKaLearningEnabled
					: false;
	const kLearningEnabled =
		typeof lockout.kLearningEnabled === 'boolean'
			? lockout.kLearningEnabled
			: true;
	const xLearningEnabled =
		typeof lockout.xLearningEnabled === 'boolean'
			? lockout.xLearningEnabled
			: true;
	const preQuiet =
		typeof lockout.preQuiet === 'boolean'
			? lockout.preQuiet
			: typeof data?.lockoutPreQuiet === 'boolean'
				? data.lockoutPreQuiet
				: typeof data?.gpsLockoutPreQuiet === 'boolean'
					? data.gpsLockoutPreQuiet
					: false;
	const maxHdopX10 =
		typeof lockout.maxHdopX10 === 'number'
			? lockout.maxHdopX10
			: typeof data?.lockoutMaxHdopX10 === 'number'
				? data.lockoutMaxHdopX10
				: typeof data?.gpsLockoutMaxHdopX10 === 'number'
					? data.gpsLockoutMaxHdopX10
					: GPS_MAX_HDOP_X10_DEFAULT;
	const minLearnerSpeedMph =
		typeof lockout.minLearnerSpeedMph === 'number'
			? lockout.minLearnerSpeedMph
			: typeof data?.lockoutMinLearnerSpeedMph === 'number'
				? data.lockoutMinLearnerSpeedMph
				: typeof data?.gpsLockoutMinLearnerSpeedMph === 'number'
					? data.gpsLockoutMinLearnerSpeedMph
					: GPS_MIN_LEARNER_SPEED_MPH_DEFAULT;

	return {
		modeRaw: (() => {
			const raw = typeof lockout.modeRaw === 'number' ? lockout.modeRaw : 0;
			return raw === 2 ? 1 : raw;
		})(),
		coreGuardEnabled: !!lockout.coreGuardEnabled,
		maxQueueDrops: typeof lockout.maxQueueDrops === 'number' ? lockout.maxQueueDrops : 0,
		maxPerfDrops: typeof lockout.maxPerfDrops === 'number' ? lockout.maxPerfDrops : 0,
		maxEventBusDrops: typeof lockout.maxEventBusDrops === 'number' ? lockout.maxEventBusDrops : 0,
		learnerPromotionHits: clampLearnerPromotionHits(learnerPromotionHits),
		learnerRadiusFt: radiusE5ToFeet(learnerRadiusE5),
		learnerFreqToleranceMHz: clampLearnerFreqToleranceMHz(learnerFreqToleranceMHz),
		learnerLearnIntervalHours: clampIntervalHours(learnerLearnIntervalHours),
		learnerUnlearnIntervalHours: clampIntervalHours(learnerUnlearnIntervalHours),
		learnerUnlearnCount: clampUnlearnCount(learnerUnlearnCount),
		manualDemotionMissCount: clampManualDemotionMissCount(manualDemotionMissCount),
		kaLearningEnabled: !!kaLearningEnabled,
		kLearningEnabled: !!kLearningEnabled,
		xLearningEnabled: !!xLearningEnabled,
		preQuiet: !!preQuiet,
		preQuietBufferE5: typeof lockout.preQuietBufferE5 === 'number' ? lockout.preQuietBufferE5 : 0,
		maxHdopX10: clampHdopX10(maxHdopX10),
		minLearnerSpeedMph: clampMinLearnerSpeed(minLearnerSpeedMph)
	};
}

export function buildZoneEditorFromZone(zone) {
	const slot = Number(zone?.slot);
	if (!Number.isInteger(slot) || slot < 0) {
		return null;
	}

	const headingIsSet = typeof zone?.headingDeg === 'number' && Number.isFinite(zone.headingDeg);
	return {
		slot,
		editor: {
			latitude: typeof zone?.latitude === 'number' ? zone.latitude.toFixed(5) : '',
			longitude: typeof zone?.longitude === 'number' ? zone.longitude.toFixed(5) : '',
			radiusFt:
				typeof zone?.radiusE5 === 'number'
					? radiusE5ToFeet(zone.radiusE5)
					: radiusE5ToFeet(LEARNER_RADIUS_E5_DEFAULT),
			bandMask: clampInt(zone?.bandMask, 1, 255, 0x04),
			frequencyMHz:
				typeof zone?.frequencyMHz === 'number' && zone.frequencyMHz > 0 ? String(zone.frequencyMHz) : '',
			frequencyToleranceMHz: clampInt(
				zone?.frequencyToleranceMHz,
				0,
				65535,
				LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT
			),
			confidence: clampInt(zone?.confidence, 0, 255, 100),
			directionMode: normalizeDirectionMode(zone?.directionMode),
			headingDeg: headingIsSet ? String(Math.round(zone.headingDeg)) : '',
			headingToleranceDeg: clampInt(zone?.headingToleranceDeg, 0, 90, 45)
		}
	};
}

export function buildZoneEditorFromObservation(event) {
	if (!event?.locationValid) {
		return { error: 'Cannot create zone: observation has no GPS fix.' };
	}

	return {
		editor: {
			latitude: typeof event.latitude === 'number' ? event.latitude.toFixed(5) : '',
			longitude: typeof event.longitude === 'number' ? event.longitude.toFixed(5) : '',
			radiusFt: radiusE5ToFeet(LEARNER_RADIUS_E5_DEFAULT),
			bandMask: bandNameToMask(event.band),
			frequencyMHz:
				typeof event.frequencyMHz === 'number' && event.frequencyMHz > 0
					? String(Math.round(event.frequencyMHz))
					: '',
			frequencyToleranceMHz: LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT,
			confidence: 100,
			directionMode: 'all',
			headingDeg: '',
			headingToleranceDeg: 45
		}
	};
}

export function buildZoneEditorPayload(zoneEditor) {
	const latitude = Number(zoneEditor.latitude);
	if (!Number.isFinite(latitude) || latitude < -90 || latitude > 90) {
		return { error: 'Latitude must be between -90 and 90.' };
	}
	const longitude = Number(zoneEditor.longitude);
	if (!Number.isFinite(longitude) || longitude < -180 || longitude > 180) {
		return { error: 'Longitude must be between -180 and 180.' };
	}
	const directionMode = normalizeDirectionMode(zoneEditor.directionMode);
	const payload = {
		latitude,
		longitude,
		bandMask: clampInt(zoneEditor.bandMask, 1, 255, 0x04),
		radiusE5: feetToRadiusE5(zoneEditor.radiusFt),
		frequencyToleranceMHz: clampInt(
			zoneEditor.frequencyToleranceMHz,
			0,
			65535,
			LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT
		),
		confidence: clampInt(zoneEditor.confidence, 0, 255, 100),
		directionMode,
		headingToleranceDeg: clampInt(zoneEditor.headingToleranceDeg, 0, 90, 45)
	};

	const frequencyMHz = Number(zoneEditor.frequencyMHz);
	if (!Number.isFinite(frequencyMHz) || frequencyMHz <= 0) {
		return { error: 'Frequency must be a positive MHz value.' };
	}
	payload.frequencyMHz = clampInt(frequencyMHz, 1, 65535, 0);

	if (directionMode === 'all') {
		payload.headingDeg = null;
	} else {
		const headingDeg = Number(zoneEditor.headingDeg);
		if (!Number.isFinite(headingDeg) || headingDeg < 0 || headingDeg >= 360) {
			return { error: 'Heading must be between 0 and 359 for directional zones.' };
		}
		payload.headingDeg = Math.round(headingDeg);
	}

	return { payload };
}

export function lockoutConfigMatchesRuntime(lockoutConfig, runtime) {
	if (!runtime) return false;
	const runtimeRadiusE5 =
		typeof runtime.learnerRadiusE5 === 'number'
			? clampLearnerRadiusE5(runtime.learnerRadiusE5)
			: feetToRadiusE5(runtime.learnerRadiusFt);
	return (
		Math.max(0, Math.min(3, Number(lockoutConfig.modeRaw) || 0)) ===
			Math.max(0, Math.min(3, Number(runtime.modeRaw) || 0)) &&
		!!lockoutConfig.coreGuardEnabled === !!runtime.coreGuardEnabled &&
		clampU16(lockoutConfig.maxQueueDrops) === clampU16(runtime.maxQueueDrops) &&
		clampU16(lockoutConfig.maxPerfDrops) === clampU16(runtime.maxPerfDrops) &&
		clampU16(lockoutConfig.maxEventBusDrops) === clampU16(runtime.maxEventBusDrops) &&
		clampLearnerPromotionHits(lockoutConfig.learnerPromotionHits) ===
			clampLearnerPromotionHits(runtime.learnerPromotionHits) &&
		feetToRadiusE5(lockoutConfig.learnerRadiusFt) === runtimeRadiusE5 &&
		clampLearnerFreqToleranceMHz(lockoutConfig.learnerFreqToleranceMHz) ===
			clampLearnerFreqToleranceMHz(runtime.learnerFreqToleranceMHz) &&
		clampIntervalHours(lockoutConfig.learnerLearnIntervalHours) ===
			clampIntervalHours(runtime.learnerLearnIntervalHours) &&
		clampIntervalHours(lockoutConfig.learnerUnlearnIntervalHours) ===
			clampIntervalHours(runtime.learnerUnlearnIntervalHours) &&
		clampUnlearnCount(lockoutConfig.learnerUnlearnCount) ===
			clampUnlearnCount(runtime.learnerUnlearnCount) &&
		clampManualDemotionMissCount(lockoutConfig.manualDemotionMissCount) ===
			clampManualDemotionMissCount(runtime.manualDemotionMissCount) &&
		!!lockoutConfig.kaLearningEnabled === !!runtime.kaLearningEnabled &&
		!!lockoutConfig.kLearningEnabled === !!runtime.kLearningEnabled &&
		!!lockoutConfig.xLearningEnabled === !!runtime.xLearningEnabled &&
		(Number(lockoutConfig.preQuietBufferE5) || 0) === (Number(runtime.preQuietBufferE5) || 0) &&
		clampHdopX10(lockoutConfig.maxHdopX10) === clampHdopX10(runtime.maxHdopX10) &&
		clampMinLearnerSpeed(lockoutConfig.minLearnerSpeedMph) === clampMinLearnerSpeed(runtime.minLearnerSpeedMph)
	);
}

export function stageLearnerPresetValues(lockoutConfig, preset) {
	lockoutConfig.learnerPromotionHits = clampLearnerPromotionHits(preset.learnerPromotionHits);
	lockoutConfig.learnerLearnIntervalHours = clampIntervalHours(preset.learnerLearnIntervalHours);
	lockoutConfig.learnerFreqToleranceMHz = clampLearnerFreqToleranceMHz(preset.learnerFreqToleranceMHz);
	lockoutConfig.learnerRadiusFt = radiusE5ToFeet(clampLearnerRadiusE5(preset.learnerRadiusE5));
	lockoutConfig.learnerUnlearnCount = clampUnlearnCount(preset.learnerUnlearnCount);
	lockoutConfig.learnerUnlearnIntervalHours = clampIntervalHours(preset.learnerUnlearnIntervalHours);
	lockoutConfig.manualDemotionMissCount = clampManualDemotionMissCount(preset.manualDemotionMissCount);
}

export function describeZoneTotals(lockoutZonesStats) {
	const zoneCount = lockoutZonesStats.activeCount || 0;
	const pendingCount = lockoutZonesStats.pendingCount || 0;
	const parts = [];
	if (zoneCount > 0) parts.push(`${zoneCount} active`);
	if (pendingCount > 0) parts.push(`${pendingCount} pending`);
	const desc = parts.length > 0 ? parts.join(' + ') : '0';
	return { zoneCount, pendingCount, desc };
}

export function resetZoneEditorState() {
	return defaultZoneEditorState();
}
