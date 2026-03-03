<script>
	import { onMount } from 'svelte';
	import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
	import { formatFrequencyMhz } from '$lib/utils/format';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	import LockoutKaWarningModal from '$lib/components/LockoutKaWarningModal.svelte';
	import LockoutZoneEditorModal from '$lib/components/LockoutZoneEditorModal.svelte';
	import {
		STATUS_POLL_INTERVAL_MS,
		LOCKOUT_EVENTS_LIMIT,
		LOCKOUT_ZONES_LIMIT,
		FEET_PER_METER,
		FEET_PER_RADIUS_E5,
		LEARNER_PROMOTION_HITS_DEFAULT,
		LEARNER_PROMOTION_HITS_MIN,
		LEARNER_PROMOTION_HITS_MAX,
		LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT,
		LEARNER_FREQ_TOLERANCE_MHZ_MIN,
		LEARNER_FREQ_TOLERANCE_MHZ_MAX,
		LEARNER_RADIUS_E5_DEFAULT,
		LEARNER_RADIUS_E5_MIN,
		LEARNER_RADIUS_E5_MAX,
		LOCKOUT_INTERVAL_OPTIONS,
		LEARNER_UNLEARN_COUNT_DEFAULT,
		GPS_MAX_HDOP_X10_DEFAULT,
		GPS_MAX_HDOP_X10_MIN,
		GPS_MAX_HDOP_X10_MAX,
		GPS_MIN_LEARNER_SPEED_MPH_DEFAULT,
		GPS_MIN_LEARNER_SPEED_MPH_MIN,
		GPS_MIN_LEARNER_SPEED_MPH_MAX,
		GPS_MIN_SATELLITES,
		MANUAL_DEMOTION_OPTIONS,
		DIRECTION_MODE_OPTIONS,
		LOCKOUT_BAND_OPTIONS,
		LOCKOUT_PRESET_LEGACY_SAFE,
		LOCKOUT_PRESET_BALANCED_BLEND,
		clampU16,
		clampInt,
		clampLearnerPromotionHits,
		clampHdopX10,
		clampMinLearnerSpeed,
		clampLearnerRadiusE5,
		clampLearnerFreqToleranceMHz,
		clampIntervalHours,
		clampUnlearnCount,
		clampManualDemotionMissCount,
		radiusE5ToFeet,
		feetToRadiusE5,
		normalizeLearnerRadiusFeet,
		formatCoordinate,
		formatEpochMs,
		formatFixAgeMs,
		formatBootTime,
		formatHdop,
		formatBandMask,
		formatRadiusFeet,
		formatIntervalLabel,
		signalMapHref,
		formatZoneRadiusFeet,
		normalizeDirectionMode,
		formatDirectionSummary,
		mapHrefFromZone,
		lockoutZoneSourceLabel,
		bandNameToMask,
		defaultZoneEditorState
	} from '$lib/utils/lockout';

	let loading = $state(true);
	let message = $state(null);
	let gpsStatusFetchInFlight = false;
	let lockoutFetchInFlight = false;
	let lockoutZonesFetchInFlight = false;
	let lockoutLoading = $state(false);
	let lockoutError = $state('');
	let lockoutZonesLoading = $state(false);
	let lockoutZonesError = $state('');
	let savingLockoutConfig = $state(false);
	let lockoutConfigInitialized = false;
	let lockoutConfigDirty = $state(false);
	let advancedUnlocked = $state(false);
	let showKaWarningModal = $state(false);
	let deletingZoneSlot = $state(null);
	let zoneEditorOpen = $state(false);
	let zoneEditorSaving = $state(false);
	let zoneEditorSlot = $state(null);
	let exportingZones = $state(false);
	let importingZones = $state(false);
	let clearingAllZones = $state(false);
	let importFileInput;

	const statusPoll = createPoll(async () => {
		await fetchGpsStatus();
	}, STATUS_POLL_INTERVAL_MS);

	let gpsStatus = $state({
		enabled: false,
		runtimeEnabled: false,
		mode: 'scaffold',
		hasFix: false,
		satellites: 0,
		speedMph: null,
		moduleDetected: false,
		detectionTimedOut: false,
		parserActive: false,
		lockout: {
			mode: 'off',
			modeRaw: 0,
			coreGuardEnabled: true,
			maxQueueDrops: 0,
			maxPerfDrops: 0,
			maxEventBusDrops: 0,
			learnerPromotionHits: LEARNER_PROMOTION_HITS_DEFAULT,
			learnerRadiusE5: LEARNER_RADIUS_E5_DEFAULT,
			learnerFreqToleranceMHz: LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT,
			learnerLearnIntervalHours: 0,
			learnerUnlearnIntervalHours: 0,
			learnerUnlearnCount: LEARNER_UNLEARN_COUNT_DEFAULT,
			manualDemotionMissCount: 0,
			kaLearningEnabled: false,
			kLearningEnabled: true,
			xLearningEnabled: true,
			coreGuardTripped: false,
			coreGuardReason: '',
			enforceAllowed: false
		}
	});

	let lockoutEvents = $state([]);
	let lockoutStats = $state({
		published: 0,
		drops: 0,
		size: 0,
		capacity: 0
	});
	let lockoutSd = $state({
		enabled: false,
		path: '',
		enqueued: 0,
		queueDrops: 0,
		deduped: 0,
		written: 0,
		writeFail: 0,
		rotations: 0
	});
	let lockoutConfig = $state({
		modeRaw: 0,
		coreGuardEnabled: true,
		maxQueueDrops: 0,
		maxPerfDrops: 0,
		maxEventBusDrops: 0,
		learnerPromotionHits: LEARNER_PROMOTION_HITS_DEFAULT,
		learnerRadiusFt: Math.round(LEARNER_RADIUS_E5_DEFAULT * FEET_PER_RADIUS_E5),
		learnerFreqToleranceMHz: LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT,
		learnerLearnIntervalHours: 0,
		learnerUnlearnIntervalHours: 0,
		learnerUnlearnCount: LEARNER_UNLEARN_COUNT_DEFAULT,
		manualDemotionMissCount: 0,
		kaLearningEnabled: false,
		kLearningEnabled: true,
		xLearningEnabled: true,
		preQuiet: false,
		preQuietBufferE5: 0,
		maxHdopX10: GPS_MAX_HDOP_X10_DEFAULT,
		minLearnerSpeedMph: GPS_MIN_LEARNER_SPEED_MPH_DEFAULT
	});
	let lockoutZonesStats = $state({
		activeCount: 0,
		activeCapacity: 0,
		activeReturned: 0,
		pendingCount: 0,
		pendingCapacity: 0,
		pendingReturned: 0,
		promotionHits: 0,
		promotionRadiusE5: 0,
		promotionFreqToleranceMHz: 0,
		learnIntervalHours: 0,
		unlearnIntervalHours: 0,
		unlearnCount: LEARNER_UNLEARN_COUNT_DEFAULT,
		manualDemotionMissCount: 0
	});
	let activeLockoutZones = $state([]);
	let pendingLockoutZones = $state([]);
	let zoneEditor = $state(defaultZoneEditorState());

	onMount(async () => {
		await refreshAll();
		statusPoll.start();
		return () => {
			statusPoll.stop();
		};
	});

	function setMsg(type, text) {
		message = { type, text };
	}

	function formatRoundedFrequencyMhz(mhz) {
		return formatFrequencyMhz(mhz, { roundMhz: true });
	}

	function applyLockoutStatus(data) {
		const lockout = data?.lockout;
		if (!lockout) return;
		if (lockoutConfigInitialized && lockoutConfigDirty) return;
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
		lockoutConfig = {
			modeRaw: (() => {
				const raw = typeof lockout.modeRaw === 'number' ? lockout.modeRaw : 0;
				// Advisory (2) is now merged with Log Only (1)
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
		lockoutConfigInitialized = true;
	}

	function markLockoutDirty() {
		lockoutConfigDirty = true;
	}

	function closeZoneEditor() {
		if (zoneEditorSaving) return;
		zoneEditorOpen = false;
		zoneEditorSlot = null;
		zoneEditor = defaultZoneEditorState();
	}

	function openZoneCreateEditor() {
		if (!advancedUnlocked) {
			setMsg('error', 'Unlock advanced writes before creating manual lockout zones.');
			return;
		}
		zoneEditorSlot = null;
		zoneEditor = defaultZoneEditorState();
		zoneEditorOpen = true;
	}

	function openZoneEditEditor(zone) {
		const slot = Number(zone?.slot);
		if (!Number.isInteger(slot) || slot < 0) return;
		if (!advancedUnlocked) {
			setMsg('error', 'Unlock advanced writes before editing lockout zones.');
			return;
		}
		const headingIsSet = typeof zone?.headingDeg === 'number' && Number.isFinite(zone.headingDeg);
		zoneEditorSlot = slot;
		zoneEditor = {
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
		};
		zoneEditorOpen = true;
	}

	function openZoneFromObservation(event) {
		if (!advancedUnlocked) {
			setMsg('error', 'Unlock advanced writes before creating lockout zones.');
			return;
		}
		if (!event?.locationValid) {
			setMsg('error', 'Cannot create zone: observation has no GPS fix.');
			return;
		}
		zoneEditorSlot = null;
		zoneEditor = {
			latitude: typeof event.latitude === 'number' ? event.latitude.toFixed(5) : '',
			longitude: typeof event.longitude === 'number' ? event.longitude.toFixed(5) : '',
			radiusFt: radiusE5ToFeet(LEARNER_RADIUS_E5_DEFAULT),
			bandMask: bandNameToMask(event.band),
			frequencyMHz: typeof event.frequencyMHz === 'number' && event.frequencyMHz > 0 ? String(Math.round(event.frequencyMHz)) : '',
			frequencyToleranceMHz: LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT,
			confidence: 100,
			directionMode: 'all',
			headingDeg: '',
			headingToleranceDeg: 45
		};
		zoneEditorOpen = true;
	}

	function buildZoneEditorPayload() {
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
		if (Number.isFinite(frequencyMHz) && frequencyMHz > 0) {
			payload.frequencyMHz = clampInt(frequencyMHz, 1, 65535, 0);
		}

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

	function runtimeLearnerHits() {
		if (typeof gpsStatus?.lockout?.learnerPromotionHits === 'number') {
			return clampLearnerPromotionHits(gpsStatus.lockout.learnerPromotionHits);
		}
		if (typeof lockoutZonesStats.promotionHits === 'number' && lockoutZonesStats.promotionHits > 0) {
			return clampLearnerPromotionHits(lockoutZonesStats.promotionHits);
		}
		return '—';
	}

	function runtimeLearnerFreqToleranceMHz() {
		if (typeof gpsStatus?.lockout?.learnerFreqToleranceMHz === 'number') {
			return clampLearnerFreqToleranceMHz(gpsStatus.lockout.learnerFreqToleranceMHz);
		}
		if (
			typeof lockoutZonesStats.promotionFreqToleranceMHz === 'number' &&
			lockoutZonesStats.promotionFreqToleranceMHz > 0
		) {
			return clampLearnerFreqToleranceMHz(lockoutZonesStats.promotionFreqToleranceMHz);
		}
		return '—';
	}

	function runtimeLearnerRadiusFeetText() {
		if (typeof gpsStatus?.lockout?.learnerRadiusE5 === 'number') {
			return `${radiusE5ToFeet(gpsStatus.lockout.learnerRadiusE5)} ft`;
		}
		if (typeof lockoutZonesStats.promotionRadiusE5 === 'number' && lockoutZonesStats.promotionRadiusE5 > 0) {
			return `${radiusE5ToFeet(lockoutZonesStats.promotionRadiusE5)} ft`;
		}
		return '—';
	}

	function runtimeLearnerLearnIntervalHours() {
		if (typeof gpsStatus?.lockout?.learnerLearnIntervalHours === 'number') {
			return clampIntervalHours(gpsStatus.lockout.learnerLearnIntervalHours);
		}
		if (typeof lockoutZonesStats.learnIntervalHours === 'number') {
			return clampIntervalHours(lockoutZonesStats.learnIntervalHours);
		}
		return 0;
	}

	function runtimeLearnerUnlearnIntervalHours() {
		if (typeof gpsStatus?.lockout?.learnerUnlearnIntervalHours === 'number') {
			return clampIntervalHours(gpsStatus.lockout.learnerUnlearnIntervalHours);
		}
		if (typeof lockoutZonesStats.unlearnIntervalHours === 'number') {
			return clampIntervalHours(lockoutZonesStats.unlearnIntervalHours);
		}
		return 0;
	}

	function runtimeLearnerUnlearnCount() {
		if (typeof gpsStatus?.lockout?.learnerUnlearnCount === 'number') {
			return clampUnlearnCount(gpsStatus.lockout.learnerUnlearnCount);
		}
		if (typeof lockoutZonesStats.unlearnCount === 'number') {
			return clampUnlearnCount(lockoutZonesStats.unlearnCount);
		}
		return LEARNER_UNLEARN_COUNT_DEFAULT;
	}

	function runtimeManualDemotionMissCount() {
		if (typeof gpsStatus?.lockout?.manualDemotionMissCount === 'number') {
			return clampManualDemotionMissCount(gpsStatus.lockout.manualDemotionMissCount);
		}
		if (typeof lockoutZonesStats.manualDemotionMissCount === 'number') {
			return clampManualDemotionMissCount(lockoutZonesStats.manualDemotionMissCount);
		}
		return 0;
	}

	function lockoutConfigMatchesBackend() {
		const runtime = gpsStatus?.lockout;
		if (!runtime) return false;
		const runtimeRadiusE5 = clampLearnerRadiusE5(runtime.learnerRadiusE5);
		return (
			Math.max(0, Math.min(3, Number(lockoutConfig.modeRaw) || 0)) ===
				(Math.max(0, Math.min(3, Number(runtime.modeRaw) || 0))) &&
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

	function stageLearnerPreset(preset) {
		lockoutConfig.learnerPromotionHits = clampLearnerPromotionHits(preset.learnerPromotionHits);
		lockoutConfig.learnerLearnIntervalHours = clampIntervalHours(preset.learnerLearnIntervalHours);
		lockoutConfig.learnerFreqToleranceMHz = clampLearnerFreqToleranceMHz(preset.learnerFreqToleranceMHz);
		lockoutConfig.learnerRadiusFt = radiusE5ToFeet(clampLearnerRadiusE5(preset.learnerRadiusE5));
		lockoutConfig.learnerUnlearnCount = clampUnlearnCount(preset.learnerUnlearnCount);
		lockoutConfig.learnerUnlearnIntervalHours = clampIntervalHours(preset.learnerUnlearnIntervalHours);
		lockoutConfig.manualDemotionMissCount = clampManualDemotionMissCount(preset.manualDemotionMissCount);
		lockoutConfigDirty = true;
		setMsg('info', `${preset.name} preset staged. Review values, then Save.`);
	}

	function requestKaLearningToggle(nextEnabled) {
		if (!advancedUnlocked) return;
		if (!nextEnabled) {
			if (lockoutConfig.kaLearningEnabled) {
				lockoutConfig.kaLearningEnabled = false;
				markLockoutDirty();
			}
			return;
		}
		if (lockoutConfig.kaLearningEnabled) {
			return;
		}
		showKaWarningModal = true;
	}

	function cancelKaLearningEnable() {
		showKaWarningModal = false;
	}

	function confirmKaLearningEnable() {
		lockoutConfig.kaLearningEnabled = true;
		showKaWarningModal = false;
		markLockoutDirty();
	}

	async function refreshAll() {
		await Promise.all([fetchGpsStatus(), fetchLockoutEvents(), fetchLockoutZones()]);
		loading = false;
	}

	async function fetchGpsStatus() {
		if (gpsStatusFetchInFlight) return;
		gpsStatusFetchInFlight = true;
		try {
			const res = await fetchWithTimeout('/api/gps/status');
			if (!res.ok) return;
			const data = await res.json();
			gpsStatus = { ...gpsStatus, ...data };
			applyLockoutStatus(data);
		} catch (e) {
			// Polling should fail silently.
		} finally {
			gpsStatusFetchInFlight = false;
		}
	}

	async function fetchLockoutEvents(options = {}) {
		const { silent = false } = options;
		if (lockoutFetchInFlight) return;
		lockoutFetchInFlight = true;
		if (!silent) {
			lockoutLoading = true;
		}
		lockoutError = '';
		try {
			const res = await fetchWithTimeout(`/api/lockouts/events?limit=${LOCKOUT_EVENTS_LIMIT}`);
			if (!res.ok) {
				if (!silent) lockoutError = 'Failed to load lockout candidates';
				return;
			}
			const data = await res.json();
			lockoutEvents = Array.isArray(data.events) ? data.events : [];
			lockoutStats = {
				published: typeof data.published === 'number' ? data.published : 0,
				drops: typeof data.drops === 'number' ? data.drops : 0,
				size: typeof data.size === 'number' ? data.size : lockoutEvents.length,
				capacity: typeof data.capacity === 'number' ? data.capacity : lockoutStats.capacity
			};
			lockoutSd = {
				enabled: !!data?.sd?.enabled,
				path: typeof data?.sd?.path === 'string' ? data.sd.path : '',
				enqueued: typeof data?.sd?.enqueued === 'number' ? data.sd.enqueued : 0,
				queueDrops: typeof data?.sd?.queueDrops === 'number' ? data.sd.queueDrops : 0,
				deduped: typeof data?.sd?.deduped === 'number' ? data.sd.deduped : 0,
				written: typeof data?.sd?.written === 'number' ? data.sd.written : 0,
				writeFail: typeof data?.sd?.writeFail === 'number' ? data.sd.writeFail : 0,
				rotations: typeof data?.sd?.rotations === 'number' ? data.sd.rotations : 0
			};
		} catch (e) {
			if (!silent) lockoutError = 'Failed to load lockout candidates';
		} finally {
			lockoutFetchInFlight = false;
			lockoutLoading = false;
		}
	}

	async function fetchLockoutZones(options = {}) {
		const { silent = false } = options;
		if (lockoutZonesFetchInFlight) return;
		lockoutZonesFetchInFlight = true;
		if (!silent) lockoutZonesLoading = true;
		lockoutZonesError = '';
		try {
			const res = await fetchWithTimeout(
				`/api/lockouts/zones?activeLimit=${LOCKOUT_ZONES_LIMIT}&pendingLimit=${LOCKOUT_ZONES_LIMIT}`
			);
			if (!res.ok) {
				if (!silent) lockoutZonesError = 'Failed to load lockout zones';
				return;
			}
			const data = await res.json();
			activeLockoutZones = Array.isArray(data.activeZones) ? data.activeZones : [];
			pendingLockoutZones = Array.isArray(data.pendingZones) ? data.pendingZones : [];
			lockoutZonesStats = {
				activeCount: typeof data.activeCount === 'number' ? data.activeCount : 0,
				activeCapacity: typeof data.activeCapacity === 'number' ? data.activeCapacity : 0,
				activeReturned:
					typeof data.activeReturned === 'number' ? data.activeReturned : activeLockoutZones.length,
				pendingCount: typeof data.pendingCount === 'number' ? data.pendingCount : 0,
				pendingCapacity: typeof data.pendingCapacity === 'number' ? data.pendingCapacity : 0,
				pendingReturned:
					typeof data.pendingReturned === 'number' ? data.pendingReturned : pendingLockoutZones.length,
				promotionHits: typeof data.promotionHits === 'number' ? data.promotionHits : 0,
				promotionRadiusE5:
					typeof data.promotionRadiusE5 === 'number' ? data.promotionRadiusE5 : 0,
				promotionFreqToleranceMHz:
					typeof data.promotionFreqToleranceMHz === 'number'
						? data.promotionFreqToleranceMHz
						: 0,
				learnIntervalHours:
					typeof data.learnIntervalHours === 'number' ? data.learnIntervalHours : 0,
				unlearnIntervalHours:
					typeof data.unlearnIntervalHours === 'number' ? data.unlearnIntervalHours : 0,
				unlearnCount: typeof data.unlearnCount === 'number' ? data.unlearnCount : 0,
				manualDemotionMissCount:
					typeof data.manualDemotionMissCount === 'number' ? data.manualDemotionMissCount : 0
			};
		} catch (e) {
			if (!silent) lockoutZonesError = 'Failed to load lockout zones';
		} finally {
			lockoutZonesFetchInFlight = false;
			lockoutZonesLoading = false;
		}
	}

	async function saveLockoutConfig() {
		if (!advancedUnlocked) {
			setMsg('error', 'Unlock advanced controls before applying lockout changes.');
			return;
		}
		if (!confirm('Apply lockout runtime changes? Incorrect values can suppress real alerts.')) {
			return;
		}
		if (savingLockoutConfig) return;
		savingLockoutConfig = true;
		try {
			const modeRaw = Math.max(0, Math.min(3, Number(lockoutConfig.modeRaw) || 0));
			const learnerPromotionHits = clampLearnerPromotionHits(lockoutConfig.learnerPromotionHits);
			const learnerRadiusE5 = feetToRadiusE5(lockoutConfig.learnerRadiusFt);
			const learnerFreqToleranceMHz = clampLearnerFreqToleranceMHz(
				lockoutConfig.learnerFreqToleranceMHz
			);
			const learnerLearnIntervalHours = clampIntervalHours(lockoutConfig.learnerLearnIntervalHours);
			const learnerUnlearnIntervalHours = clampIntervalHours(lockoutConfig.learnerUnlearnIntervalHours);
			const learnerUnlearnCount = clampUnlearnCount(lockoutConfig.learnerUnlearnCount);
			const manualDemotionMissCount = clampManualDemotionMissCount(
				lockoutConfig.manualDemotionMissCount
			);
			const kaLearningEnabled = !!lockoutConfig.kaLearningEnabled;
			const kLearningEnabled = !!lockoutConfig.kLearningEnabled;
			const xLearningEnabled = !!lockoutConfig.xLearningEnabled;
			const maxHdopX10 = clampHdopX10(lockoutConfig.maxHdopX10);
			const minLearnerSpeedMph = clampMinLearnerSpeed(lockoutConfig.minLearnerSpeedMph);
			const payload = {
				lockoutMode: modeRaw,
				lockoutCoreGuardEnabled: !!lockoutConfig.coreGuardEnabled,
				lockoutMaxQueueDrops: clampU16(lockoutConfig.maxQueueDrops),
				lockoutMaxPerfDrops: clampU16(lockoutConfig.maxPerfDrops),
				lockoutMaxEventBusDrops: clampU16(lockoutConfig.maxEventBusDrops),
				lockoutLearnerPromotionHits: learnerPromotionHits,
				lockoutLearnerRadiusE5: learnerRadiusE5,
				lockoutLearnerFreqToleranceMHz: learnerFreqToleranceMHz,
				lockoutLearnerLearnIntervalHours: learnerLearnIntervalHours,
				lockoutLearnerUnlearnIntervalHours: learnerUnlearnIntervalHours,
				lockoutLearnerUnlearnCount: learnerUnlearnCount,
				lockoutManualDemotionMissCount: manualDemotionMissCount,
				lockoutKaLearningEnabled: kaLearningEnabled,
				lockoutKLearningEnabled: kLearningEnabled,
				lockoutXLearningEnabled: xLearningEnabled,
				lockoutPreQuiet: !!lockoutConfig.preQuiet,
				lockoutPreQuietBufferE5: Number(lockoutConfig.preQuietBufferE5) || 0,
				lockoutMaxHdopX10: maxHdopX10,
				lockoutMinLearnerSpeedMph: minLearnerSpeedMph
			};
			const res = await fetchWithTimeout('/api/gps/config', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify(payload)
			});
			let data = {};
			const contentType = res.headers.get('content-type') || '';
			if (contentType.includes('application/json')) {
				data = await res.json().catch(() => ({}));
			} else {
				const text = await res.text().catch(() => '');
				if (text) data = { message: text };
			}
			if (!res.ok) {
				setMsg('error', data.message || data.error || `Failed to update lockout settings (${res.status})`);
				return;
			}
			lockoutConfig.learnerPromotionHits = learnerPromotionHits;
			lockoutConfig.learnerRadiusFt = radiusE5ToFeet(learnerRadiusE5);
			lockoutConfig.learnerFreqToleranceMHz = learnerFreqToleranceMHz;
			lockoutConfig.learnerLearnIntervalHours = learnerLearnIntervalHours;
			lockoutConfig.learnerUnlearnIntervalHours = learnerUnlearnIntervalHours;
			lockoutConfig.learnerUnlearnCount = learnerUnlearnCount;
			lockoutConfig.manualDemotionMissCount = manualDemotionMissCount;
			lockoutConfig.kaLearningEnabled = kaLearningEnabled;
			lockoutConfig.preQuiet = !!lockoutConfig.preQuiet;
			lockoutConfig.maxHdopX10 = maxHdopX10;
			lockoutConfig.minLearnerSpeedMph = minLearnerSpeedMph;
			lockoutConfigDirty = false;
			setMsg('success', 'Lockout runtime settings updated');
			await Promise.all([fetchGpsStatus(), fetchLockoutZones({ silent: true })]);
		} catch (e) {
			setMsg(
				'error',
				e?.message ? `Failed to update lockout settings (${e.message})` : 'Failed to update lockout settings'
			);
		} finally {
			savingLockoutConfig = false;
		}
	}

	async function saveZoneEditor() {
		if (zoneEditorSaving) return;
		if (!advancedUnlocked) {
			setMsg('error', 'Unlock advanced writes before saving lockout zones.');
			return;
		}
		const { payload, error } = buildZoneEditorPayload();
		if (error) {
			setMsg('error', error);
			return;
		}
		zoneEditorSaving = true;
		try {
			const creating = zoneEditorSlot === null;
			const requestPayload = creating ? payload : { slot: zoneEditorSlot, ...payload };
			const res = await fetchWithTimeout(creating ? '/api/lockouts/zones/create' : '/api/lockouts/zones/update', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify(requestPayload)
			});
			const data = await res.json().catch(() => ({}));
			if (!res.ok) {
				setMsg(
					'error',
					data.message || `Failed to ${creating ? 'create' : 'update'} lockout zone (${res.status})`
				);
				return;
			}
			const slotText =
				typeof data.slot === 'number' ? ` ${data.slot}` : zoneEditorSlot === null ? '' : ` ${zoneEditorSlot}`;
			setMsg('success', `${creating ? 'Created' : 'Updated'} lockout zone${slotText}`);
			zoneEditorOpen = false;
			zoneEditorSlot = null;
			zoneEditor = defaultZoneEditorState();
			await fetchLockoutZones({ silent: true });
		} catch (e) {
			setMsg('error', e?.message ? `Failed to save lockout zone (${e.message})` : 'Failed to save lockout zone');
		} finally {
			zoneEditorSaving = false;
		}
	}

	async function deleteZone(zone) {
		const slot = Number(zone?.slot);
		if (!Number.isInteger(slot) || slot < 0) return;
		if (!advancedUnlocked) {
			setMsg('error', 'Unlock advanced writes before deleting lockout zones.');
			return;
		}
		if (!confirm(`Delete ${lockoutZoneSourceLabel(zone)} lockout zone in slot ${slot}?`)) return;
		deletingZoneSlot = slot;
		try {
			const res = await fetchWithTimeout('/api/lockouts/zones/delete', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ slot })
			});
			const data = await res.json().catch(() => ({}));
			if (!res.ok) {
				setMsg('error', data.message || 'Failed to delete lockout zone');
				return;
			}
			if (zoneEditorOpen && zoneEditorSlot === slot) {
				zoneEditorOpen = false;
				zoneEditorSlot = null;
				zoneEditor = defaultZoneEditorState();
			}
			setMsg('success', `Deleted ${lockoutZoneSourceLabel(zone)} lockout zone ${slot}`);
			await fetchLockoutZones({ silent: true });
		} catch (e) {
			setMsg('error', e?.message ? `Failed to delete lockout zone (${e.message})` : 'Failed to delete lockout zone');
		} finally {
			deletingZoneSlot = null;
		}
	}

	async function exportLockoutZones() {
		if (exportingZones) return;
		exportingZones = true;
		try {
			const res = await fetchWithTimeout('/api/lockouts/zones/export');
			const payload = await res.text().catch(() => '');
			if (!res.ok || !payload) {
				setMsg('error', `Failed to export lockout zones (${res.status})`);
				return;
			}
			const blob = new Blob([payload], { type: 'application/json' });
			const stamp = new Date().toISOString().replace(/[:.]/g, '-');
			const href = URL.createObjectURL(blob);
			const link = document.createElement('a');
			link.href = href;
			link.download = `v1-lockouts-${stamp}.json`;
			document.body.appendChild(link);
			link.click();
			link.remove();
			URL.revokeObjectURL(href);
			let zoneCount = '';
			try {
				const parsed = JSON.parse(payload);
				if (Array.isArray(parsed?.zones)) zoneCount = ` (${parsed.zones.length} zones)`;
			} catch {}
			setMsg('success', `Exported lockout zones${zoneCount}.`);
		} catch (e) {
			setMsg('error', e?.message ? `Failed to export lockout zones (${e.message})` : 'Failed to export lockout zones');
		} finally {
			exportingZones = false;
		}
	}

	function promptLockoutImport() {
		if (!advancedUnlocked) {
			setMsg('error', 'Unlock advanced writes before importing lockout zones.');
			return;
		}
		importFileInput?.click();
	}

	async function handleImportFileSelected(event) {
		const input = event.currentTarget;
		const file = input?.files?.[0];
		if (!file) return;
		if (!advancedUnlocked) {
			setMsg('error', 'Unlock advanced writes before importing lockout zones.');
			input.value = '';
			return;
		}
		importingZones = true;
		try {
			const payload = await file.text();
			let parsed;
			try {
				parsed = JSON.parse(payload);
			} catch {
				setMsg('error', 'Invalid JSON file.');
				input.value = '';
				return;
			}
			const fileZoneCount = Array.isArray(parsed?.zones) ? parsed.zones.length : '?';
			const mergeChoice = confirm(
				`File contains ${fileZoneCount} zones.\n\n` +
				`OK = MERGE (add to existing zones)\n` +
				`Cancel = go back`
			);
			if (!mergeChoice) {
				// Ask if they want to replace instead
				const replaceChoice = confirm(
					`Replace ALL current zones with ${fileZoneCount} zones from ${file.name}?\n\n` +
					`This will delete all existing zones first.`
				);
				if (!replaceChoice) {
					input.value = '';
					return;
				}
				// Replace mode — import file directly
				const res = await fetchWithTimeout('/api/lockouts/zones/import', {
					method: 'POST',
					headers: { 'Content-Type': 'application/json' },
					body: payload
				});
				const data = await res.json().catch(() => ({}));
				if (!res.ok) {
					setMsg('error', data.message || `Failed to import lockout zones (${res.status})`);
					return;
				}
				const importedCount = typeof data.entriesImported === 'number' ? data.entriesImported : fileZoneCount;
				setMsg('success', `Replaced with ${importedCount} lockout zones.`);
			} else {
				// Merge mode — fetch current, combine, import
				const exportRes = await fetchWithTimeout('/api/lockouts/zones/export');
				if (!exportRes.ok) {
					setMsg('error', `Failed to fetch current zones for merge (${exportRes.status})`);
					return;
				}
				const currentData = await exportRes.json().catch(() => ({}));
				const currentZones = Array.isArray(currentData?.zones) ? currentData.zones : [];
				const fileZones = Array.isArray(parsed?.zones) ? parsed.zones : [];
				// Deduplicate by lat+lon+band — keep existing zones, add new ones
				const existingKeys = new Set(
					currentZones.map((z) => `${z.lat},${z.lon},${z.band}`)
				);
				let addedCount = 0;
				for (const z of fileZones) {
					const key = `${z.lat},${z.lon},${z.band}`;
					if (!existingKeys.has(key)) {
						currentZones.push(z);
						existingKeys.add(key);
						addedCount++;
					}
				}
				const merged = {
					_type: 'v1simple_lockout_zones',
					_version: 1,
					zones: currentZones
				};
				const mergeRes = await fetchWithTimeout('/api/lockouts/zones/import', {
					method: 'POST',
					headers: { 'Content-Type': 'application/json' },
					body: JSON.stringify(merged)
				});
				const mergeData = await mergeRes.json().catch(() => ({}));
				if (!mergeRes.ok) {
					setMsg('error', mergeData.message || `Failed to merge lockout zones (${mergeRes.status})`);
					return;
				}
				const skipped = fileZones.length - addedCount;
				const skippedMsg = skipped > 0 ? ` (${skipped} duplicates skipped)` : '';
				setMsg('success', `Merged ${addedCount} new zones into ${currentZones.length - addedCount} existing${skippedMsg}.`);
			}
			zoneEditorOpen = false;
			zoneEditorSlot = null;
			zoneEditor = defaultZoneEditorState();
			await fetchLockoutZones({ silent: true });
		} catch (e) {
			setMsg('error', e?.message ? `Failed to import lockout zones (${e.message})` : 'Failed to import lockout zones');
		} finally {
			importingZones = false;
			input.value = '';
		}
	}

	async function clearAllZones() {
		if (clearingAllZones) return;
		if (!advancedUnlocked) {
			setMsg('error', 'Unlock advanced writes before clearing zones.');
			return;
		}
		const zoneCount = lockoutZonesStats.activeCount || 0;
		if (!confirm(`Delete ALL ${zoneCount} lockout zones? This cannot be undone.\n\nConsider exporting first.`)) {
			return;
		}
		clearingAllZones = true;
		try {
			const empty = JSON.stringify({ _type: 'v1simple_lockout_zones', _version: 1, zones: [] });
			const res = await fetchWithTimeout('/api/lockouts/zones/import', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: empty
			});
			if (!res.ok) {
				const data = await res.json().catch(() => ({}));
				setMsg('error', data.message || `Failed to clear zones (${res.status})`);
				return;
			}
			zoneEditorOpen = false;
			zoneEditorSlot = null;
			zoneEditor = defaultZoneEditorState();
			setMsg('success', `Cleared ${zoneCount} lockout zones.`);
			await fetchLockoutZones({ silent: true });
		} catch (e) {
			setMsg('error', e?.message ? `Failed to clear zones (${e.message})` : 'Failed to clear zones');
		} finally {
			clearingAllZones = false;
		}
	}
</script>

<div class="page-stack">
	<PageHeader
		title="Lockouts"
		subtitle="Manage signal lockout zones and learning rules."
	>
		<div class="flex gap-2">
			<a href="/integrations" class="btn btn-outline btn-sm">GPS</a>
			<button class="btn btn-outline btn-sm" onclick={refreshAll}>Refresh All</button>
		</div>
	</PageHeader>

	<StatusAlert {message} />

	<div class="surface-card">
		<div class="card-body gap-3">
			<CardSectionHead
				title="Safety Gate"
				subtitle="Advanced lockout writes stay disabled until explicitly unlocked for this session."
			>
				<label class="label cursor-pointer justify-start gap-3 py-0">
					<span class="label-text">Unlock advanced writes</span>
					<input
						type="checkbox"
						class="toggle toggle-warning"
						checked={advancedUnlocked}
						onchange={(e) => {
							advancedUnlocked = e.currentTarget.checked;
						}}
					/>
				</label>
			</CardSectionHead>
			<div class="copy-caption-soft">
				Changing lockout settings can affect threat detection. Use caution in Enforce mode.
			</div>
		</div>
		</div>

		<div class="surface-card">
		<div class="card-body gap-3">
			<CardSectionHead
				title="GPS Quality"
				subtitle="Live GPS fix quality determines whether lockout evaluation and learning are active."
			/>
			<div class="surface-stats">
				<div class="stat py-3 px-4">
					<div class="stat-title">Satellites</div>
					<div class="stat-value text-base" class:text-error={typeof gpsStatus?.satellites === 'number' && gpsStatus.satellites < GPS_MIN_SATELLITES} class:text-success={typeof gpsStatus?.satellites === 'number' && gpsStatus.satellites >= GPS_MIN_SATELLITES}>
						{typeof gpsStatus?.satellites === 'number' ? gpsStatus.satellites : '—'}
					</div>
					<div class="stat-desc">min {GPS_MIN_SATELLITES} required</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">HDOP</div>
					<div class="stat-value text-base" class:text-error={typeof gpsStatus?.hdop === 'number' && gpsStatus.hdop > lockoutConfig.maxHdopX10 / 10} class:text-success={typeof gpsStatus?.hdop === 'number' && gpsStatus.hdop <= lockoutConfig.maxHdopX10 / 10}>
						{formatHdop(gpsStatus?.hdop)}
					</div>
					<div class="stat-desc">max {(lockoutConfig.maxHdopX10 / 10).toFixed(1)} allowed</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Speed</div>
					<div class="stat-value text-base" class:text-warning={typeof gpsStatus?.speedMph === 'number' && lockoutConfig.minLearnerSpeedMph > 0 && gpsStatus.speedMph < lockoutConfig.minLearnerSpeedMph}>
						{typeof gpsStatus?.speedMph === 'number' ? `${Math.round(gpsStatus.speedMph)} mph` : '—'}
					</div>
					<div class="stat-desc">{lockoutConfig.minLearnerSpeedMph > 0 ? `min ${lockoutConfig.minLearnerSpeedMph} mph for learning` : 'no speed gate'}</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Fix</div>
					<div class="stat-value text-base" class:text-success={gpsStatus?.hasFix} class:text-error={!gpsStatus?.hasFix}>
						{gpsStatus?.hasFix ? 'Yes' : 'No'}
					</div>
					<div class="stat-desc">{gpsStatus?.locationValid ? 'location valid' : 'no position'}</div>
				</div>
			</div>
		</div>
		</div>

		<div class="surface-card">
		<div class="card-body gap-3">
			<CardSectionHead
				title="Lockout Mode"
				subtitle="How the system handles known false signals."
			>
				<div class="flex gap-2">
					{#if lockoutConfigDirty}
						<div class="badge badge-warning badge-sm">unsaved</div>
					{:else if lockoutConfigMatchesBackend()}
						<div class="badge badge-success badge-sm">synced</div>
					{:else}
						<div class="badge badge-ghost badge-sm">syncing</div>
					{/if}
					<button class="btn btn-outline btn-sm" onclick={() => fetchGpsStatus()}>Reload</button>
					<button
						class="btn btn-primary btn-sm"
						onclick={saveLockoutConfig}
						disabled={!advancedUnlocked || savingLockoutConfig || !lockoutConfigDirty}
					>
						{#if savingLockoutConfig}
							<span class="loading loading-spinner loading-xs"></span>
						{/if}
						Save
					</button>
				</div>
			</CardSectionHead>

			<div class="space-y-4">
				<div class="form-control">
					<label class="label" for="lockout-mode">
						<span class="label-text font-medium">Mode</span>
					</label>
					<select
						id="lockout-mode"
						class="select select-bordered w-full"
						bind:value={lockoutConfig.modeRaw}
						onchange={markLockoutDirty}
						disabled={!advancedUnlocked}
					>
						<option value={0}>Off — lockouts disabled</option>
						<option value={1}>Log Only — evaluate and log matches, no muting</option>
						<option value={3}>Enforce — mute locked-out signals</option>
					</select>
					{#if lockoutConfig.modeRaw === 3}
						<p class="copy-warning mt-1">⚠ Enforce will mute alerts matching lockout zones</p>
					{/if}
				</div>
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text font-medium">Safety Circuit Breaker</span>
							<p class="copy-caption-soft">Automatically disable enforcement if system health degrades</p>
						</div>
						<input
							type="checkbox"
							class="toggle toggle-primary"
							checked={!!lockoutConfig.coreGuardEnabled}
							disabled={!advancedUnlocked}
							onchange={(e) => {
								lockoutConfig.coreGuardEnabled = e.currentTarget.checked;
								markLockoutDirty();
							}}
						/>
					</label>
					{#if !lockoutConfig.coreGuardEnabled}
						<p class="copy-warning mt-1">⚠ Enforcement continues even during system issues</p>
					{/if}
				</div>
				{#if lockoutConfig.modeRaw === 3}
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text font-medium">Lower Volume in Lockout Zones</span>
							<p class="copy-caption-soft">Reduce V1 volume when approaching a zone, restore instantly on real alerts</p>
						</div>
						<input
							type="checkbox"
							class="toggle toggle-primary"
							checked={!!lockoutConfig.preQuiet}
							disabled={!advancedUnlocked}
							onchange={(e) => {
								lockoutConfig.preQuiet = e.currentTarget.checked;
								markLockoutDirty();
							}}
						/>
					</label>
				</div>
				{#if lockoutConfig.preQuiet}
				<div class="form-control">
					<label class="label" for="pre-quiet-buffer">
						<span class="label-text font-medium">Pre-Quiet Approach Distance</span>
					</label>
					<select
						id="pre-quiet-buffer"
						class="select select-bordered w-full"
						bind:value={lockoutConfig.preQuietBufferE5}
						onchange={markLockoutDirty}
						disabled={!advancedUnlocked}
					>
						<option value={0}>Same as zone — drop volume at zone edge</option>
						<option value={45}>+50m (~150 ft) — drop volume slightly before zone</option>
						<option value={90}>+100m (~300 ft) — drop volume well before zone</option>
						<option value={135}>+150m (~500 ft) — drop volume early, no surprise beeps</option>
					</select>
				</div>
				{/if}
				{/if}
				{#if lockoutConfig.coreGuardEnabled}
				<div class="surface-subsection">
					<p class="copy-caption-soft mb-2">Circuit breaker trip thresholds (0 = trip on first drop):</p>
					<div class="grid grid-cols-1 md:grid-cols-3 gap-3">
						<label class="form-control">
							<span class="label-text">Alert Queue Drops</span>
							<input
								type="number"
								min="0"
								max="65535"
								class="input input-bordered input-sm"
								value={lockoutConfig.maxQueueDrops}
								disabled={!advancedUnlocked}
								onchange={(e) => {
									lockoutConfig.maxQueueDrops = clampU16(e.currentTarget.value);
									markLockoutDirty();
								}}
							/>
						</label>
						<label class="form-control">
							<span class="label-text">Performance Drops</span>
							<input
								type="number"
								min="0"
								max="65535"
								class="input input-bordered input-sm"
								value={lockoutConfig.maxPerfDrops}
								disabled={!advancedUnlocked}
								onchange={(e) => {
									lockoutConfig.maxPerfDrops = clampU16(e.currentTarget.value);
									markLockoutDirty();
								}}
							/>
						</label>
						<label class="form-control">
							<span class="label-text">Event Bus Drops</span>
							<input
								type="number"
								min="0"
								max="65535"
								class="input input-bordered input-sm"
								value={lockoutConfig.maxEventBusDrops}
								disabled={!advancedUnlocked}
								onchange={(e) => {
									lockoutConfig.maxEventBusDrops = clampU16(e.currentTarget.value);
									markLockoutDirty();
								}}
							/>
						</label>
					</div>
				</div>
				{/if}
			</div>

		</div>
	</div>

		<div class="surface-card">
			<div class="card-body gap-3">
				<CardSectionHead
					title="Learning Rules"
					subtitle="How the system learns and forgets lockout zones from repeated signal sightings."
				/>
				<div class="flex flex-wrap gap-2">
					<button
						class="btn btn-outline btn-xs"
						onclick={() => stageLearnerPreset(LOCKOUT_PRESET_LEGACY_SAFE)}
						disabled={!advancedUnlocked}
					>
						Preset: Conservative
					</button>
					<button
						class="btn btn-outline btn-xs"
						onclick={() => stageLearnerPreset(LOCKOUT_PRESET_BALANCED_BLEND)}
						disabled={!advancedUnlocked}
					>
						Preset: Balanced
					</button>
				</div>

				<div class="grid grid-cols-1 md:grid-cols-2 gap-3">
					<label class="form-control">
						<span class="label-text font-medium">Sightings Before Lockout</span>
						<input
							type="number"
							min={LEARNER_PROMOTION_HITS_MIN}
							max={LEARNER_PROMOTION_HITS_MAX}
							class="input input-bordered input-sm"
							value={lockoutConfig.learnerPromotionHits}
							disabled={!advancedUnlocked}
							onchange={(e) => {
								lockoutConfig.learnerPromotionHits = clampLearnerPromotionHits(e.currentTarget.value);
								markLockoutDirty();
							}}
						/>
					</label>
					<label class="form-control">
						<span class="label-text font-medium">Time Between Sightings</span>
						<select
							class="select select-bordered select-sm"
							value={lockoutConfig.learnerLearnIntervalHours}
							disabled={!advancedUnlocked}
							onchange={(e) => {
								lockoutConfig.learnerLearnIntervalHours = clampIntervalHours(
									e.currentTarget.value
								);
								markLockoutDirty();
							}}
						>
							<option value={0}>Count every pass</option>
							<option value={1}>At least 1 hour apart</option>
							<option value={4}>At least 4 hours apart</option>
							<option value={12}>At least 12 hours apart</option>
							<option value={24}>At least 24 hours apart</option>
						</select>
					</label>
					<label class="form-control">
						<span class="label-text font-medium">Frequency Match Window (±MHz)</span>
						<input
							type="number"
							min={LEARNER_FREQ_TOLERANCE_MHZ_MIN}
							max={LEARNER_FREQ_TOLERANCE_MHZ_MAX}
							class="input input-bordered input-sm"
							value={lockoutConfig.learnerFreqToleranceMHz}
							disabled={!advancedUnlocked}
							onchange={(e) => {
								lockoutConfig.learnerFreqToleranceMHz = clampLearnerFreqToleranceMHz(
									e.currentTarget.value
								);
								markLockoutDirty();
							}}
						/>
					</label>
					<label class="form-control">
						<span class="label-text font-medium">Zone Radius (ft)</span>
						<input
							type="number"
							min={radiusE5ToFeet(LEARNER_RADIUS_E5_MIN)}
							max={radiusE5ToFeet(LEARNER_RADIUS_E5_MAX)}
							class="input input-bordered input-sm"
							value={lockoutConfig.learnerRadiusFt}
							disabled={!advancedUnlocked}
							onchange={(e) => {
								lockoutConfig.learnerRadiusFt = normalizeLearnerRadiusFeet(e.currentTarget.value);
								markLockoutDirty();
							}}
						/>
					</label>
					<label class="form-control">
						<span class="label-text font-medium">Auto-Remove Learned Zones</span>
						<select
							class="select select-bordered select-sm"
							value={lockoutConfig.learnerUnlearnCount}
							disabled={!advancedUnlocked}
							onchange={(e) => {
								lockoutConfig.learnerUnlearnCount = clampUnlearnCount(e.currentTarget.value);
								markLockoutDirty();
							}}
						>
							<option value={0}>Never — use confidence decay</option>
							<option value={3}>After 3 drives without signal</option>
							<option value={5}>After 5 drives without signal</option>
							<option value={7}>After 7 drives without signal</option>
							<option value={10}>After 10 drives without signal</option>
						</select>
					</label>
					<label class="form-control">
						<span class="label-text font-medium">Time Between Removal Checks</span>
						<select
							class="select select-bordered select-sm"
							value={lockoutConfig.learnerUnlearnIntervalHours}
							disabled={!advancedUnlocked}
							onchange={(e) => {
								lockoutConfig.learnerUnlearnIntervalHours = clampIntervalHours(
									e.currentTarget.value
								);
								markLockoutDirty();
							}}
						>
							<option value={0}>Check every pass</option>
							<option value={1}>At least 1 hour apart</option>
							<option value={4}>At least 4 hours apart</option>
							<option value={12}>At least 12 hours apart</option>
							<option value={24}>At least 24 hours apart</option>
						</select>
					</label>
					<label class="form-control">
						<span class="label-text font-medium">Auto-Remove Manual Zones</span>
						<select
							class="select select-bordered select-sm"
							value={lockoutConfig.manualDemotionMissCount}
							disabled={!advancedUnlocked}
							onchange={(e) => {
								lockoutConfig.manualDemotionMissCount = clampManualDemotionMissCount(
									e.currentTarget.value
								);
								markLockoutDirty();
							}}
						>
							<option value={0}>Never — persist forever</option>
							<option value={10}>After 10 drives without signal</option>
							<option value={25}>After 25 drives without signal</option>
							<option value={50}>After 50 drives without signal</option>
						</select>
					</label>
				</div>
				<div class="divider text-xs my-1">Band Learning</div>
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text font-medium">K Band Learning</span>
							<p class="copy-caption-soft">Learn and lock out K-band false alerts (door openers, speed signs) — the primary use case for lockouts</p>
						</div>
						<input
							type="checkbox"
							class="toggle toggle-primary"
							checked={!!lockoutConfig.kLearningEnabled}
							disabled={!advancedUnlocked}
							onchange={(e) => {
								lockoutConfig.kLearningEnabled = e.currentTarget.checked;
								markLockoutDirty();
							}}
						/>
					</label>
					{#if !lockoutConfig.kLearningEnabled}
						<p class="copy-warning mt-1">⚠ K learning disabled — most false alerts will not be locked out</p>
					{/if}
				</div>
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text font-medium">X Band Learning</span>
							<p class="copy-caption-soft">Learn and lock out X-band false alerts — less common but still present in some areas</p>
						</div>
						<input
							type="checkbox"
							class="toggle toggle-primary"
							checked={!!lockoutConfig.xLearningEnabled}
							disabled={!advancedUnlocked}
							onchange={(e) => {
								lockoutConfig.xLearningEnabled = e.currentTarget.checked;
								markLockoutDirty();
							}}
						/>
					</label>
				</div>
				<div class="form-control">
					<label class="label cursor-pointer">
						<div>
							<span class="label-text font-medium">Ka Band Learning</span>
							<p class="copy-caption-soft">Allow the learner to lock out Ka-band signals — where real radar threats live</p>
						</div>
						<input
							type="checkbox"
							class="toggle toggle-warning"
							checked={!!lockoutConfig.kaLearningEnabled}
							disabled={!advancedUnlocked}
							onchange={(e) => {
								requestKaLearningToggle(e.currentTarget.checked);
							}}
						/>
					</label>
					{#if lockoutConfig.kaLearningEnabled}
						<p class="copy-warning mt-1">⚠ Ka learning active — lockouts can suppress real radar threats</p>
					{/if}
				</div>

				<div class="divider text-xs my-1">GPS Quality Gates</div>
				<div class="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-3 gap-3">
					<label class="form-control">
						<span class="label-text font-medium">GPS Accuracy Limit</span>
						<input
							type="number"
							min={GPS_MAX_HDOP_X10_MIN}
							max={GPS_MAX_HDOP_X10_MAX}
							class="input input-bordered input-sm"
							value={lockoutConfig.maxHdopX10}
							disabled={!advancedUnlocked}
							onchange={(e) => {
								lockoutConfig.maxHdopX10 = clampHdopX10(e.currentTarget.value);
								markLockoutDirty();
							}}
						/>
					</label>
					<label class="form-control">
						<span class="label-text font-medium">Minimum Speed for Learning</span>
						<input
							type="number"
							min={GPS_MIN_LEARNER_SPEED_MPH_MIN}
							max={GPS_MIN_LEARNER_SPEED_MPH_MAX}
							class="input input-bordered input-sm"
							value={lockoutConfig.minLearnerSpeedMph}
							disabled={!advancedUnlocked}
							onchange={(e) => {
								lockoutConfig.minLearnerSpeedMph = clampMinLearnerSpeed(e.currentTarget.value);
								markLockoutDirty();
							}}
						/>
					</label>
					<div class="form-control">
						<span class="label-text font-medium">Minimum Satellites</span>
						<div class="input input-bordered input-sm input-disabled flex items-center">{GPS_MIN_SATELLITES}</div>
					</div>
				</div>

			</div>
		</div>

	<div class="surface-card">
		<div class="card-body gap-3">
			<CardSectionHead
				title="Lockout Zones"
				subtitle="Active lockouts and pending candidates. Create, edit, export, or import zones."
			>
				<div class="flex flex-wrap gap-2">
					<button
						class="btn btn-primary btn-sm"
						onclick={openZoneCreateEditor}
						disabled={!advancedUnlocked || zoneEditorSaving || importingZones}
					>
						New Manual Zone
					</button>
					<button
						class="btn btn-outline btn-sm"
						onclick={promptLockoutImport}
						disabled={!advancedUnlocked || importingZones || zoneEditorSaving}
					>
						{#if importingZones}
							<span class="loading loading-spinner loading-xs"></span>
						{/if}
						Import
					</button>
					<button class="btn btn-outline btn-sm" onclick={exportLockoutZones} disabled={exportingZones}>
						{#if exportingZones}
							<span class="loading loading-spinner loading-xs"></span>
						{/if}
						Export
					</button>
					<button class="btn btn-outline btn-sm" onclick={() => fetchLockoutZones()} disabled={lockoutZonesLoading}>
						{#if lockoutZonesLoading}
							<span class="loading loading-spinner loading-xs"></span>
						{/if}
						Refresh
					</button>
					<button
						class="btn btn-outline btn-error btn-sm"
						onclick={clearAllZones}
						disabled={!advancedUnlocked || clearingAllZones || importingZones || (lockoutZonesStats.activeCount === 0)}
					>
						{#if clearingAllZones}
							<span class="loading loading-spinner loading-xs"></span>
						{/if}
						Clear All
					</button>
					<input
						type="file"
						accept=".json,application/json"
						class="hidden"
						bind:this={importFileInput}
						onchange={handleImportFileSelected}
					/>
				</div>
			</CardSectionHead>

			{#if lockoutZonesError}
				<StatusAlert message={lockoutZonesError} fallbackType="warning" />
			{/if}

			<div class="surface-stats">
				<div class="stat py-3 px-4">
					<div class="stat-title">Active</div>
					<div class="stat-value text-base">
						{lockoutZonesStats.activeReturned}/{lockoutZonesStats.activeCount}
					</div>
					<div class="stat-desc">showing/total zones</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Pending</div>
					<div class="stat-value text-base">
						{lockoutZonesStats.pendingReturned}/{lockoutZonesStats.pendingCount}
					</div>
					<div class="stat-desc">showing/total candidates</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Promotion Hits</div>
					<div class="stat-value text-base">{lockoutZonesStats.promotionHits || '—'}</div>
					<div class="stat-desc">candidate threshold</div>
				</div>
			</div>

			{#if lockoutZonesLoading}
				<div class="state-loading tight">
					<span class="loading loading-spinner loading-md"></span>
				</div>
			{:else}
				<div class="grid grid-cols-1 gap-4">
					<div class="surface-table-wrap">
						<div class="copy-mini-title">Active Zones</div>
						{#if activeLockoutZones.length === 0}
							<div class="state-empty">No active lockout zones.</div>
						{:else}
							<table class="table table-sm min-w-[1120px]">
								<thead>
									<tr>
										<th>Slot</th>
										<th>Source</th>
										<th>Controls</th>
										<th>Band</th>
										<th>Freq</th>
										<th>Conf</th>
										<th>Radius</th>
										<th>Direction</th>
										<th>Auto-Remove</th>
										<th>Location</th>
									</tr>
								</thead>
								<tbody>
									{#each activeLockoutZones as zone}
										<tr>
											<td class="font-mono text-xs">{zone.slot}</td>
											<td class="text-xs">
												<div class="flex flex-wrap gap-1">
													{#if zone.manual}
														<div class="badge badge-outline badge-xs">manual</div>
													{/if}
													{#if zone.learned}
														<div class="badge badge-info badge-outline badge-xs">learned</div>
													{/if}
													{#if !zone.manual && !zone.learned}
														<div class="badge badge-ghost badge-xs">active</div>
													{/if}
												</div>
											</td>
											<td class="text-xs">
												<div class="flex flex-wrap gap-1">
													<button
														class="btn btn-xs btn-outline"
														onclick={() => openZoneEditEditor(zone)}
														disabled={!advancedUnlocked || zoneEditorSaving || importingZones}
													>
														Edit
													</button>
													<button
														class="btn btn-xs btn-error btn-outline"
														onclick={() => deleteZone(zone)}
														disabled={!advancedUnlocked || deletingZoneSlot === zone.slot}
													>
														{#if deletingZoneSlot === zone.slot}
															<span class="loading loading-spinner loading-xs"></span>
														{/if}
														Delete
													</button>
												</div>
											</td>
											<td>{formatBandMask(zone.bandMask)}</td>
											<td class="whitespace-nowrap">{formatRoundedFrequencyMhz(zone.frequencyMHz)}</td>
											<td>{typeof zone.confidence === 'number' ? zone.confidence : '—'}</td>
											<td class="whitespace-nowrap">{formatZoneRadiusFeet(zone)}</td>
											<td class="text-xs whitespace-nowrap">{formatDirectionSummary(zone)}</td>
											<td class="text-xs">
												{#if typeof zone.demotionMissThreshold === 'number'}
													{zone.missCount ?? 0}/{zone.demotionMissThreshold} misses
													{#if typeof zone.demotionMissesRemaining === 'number'}
														({zone.demotionMissesRemaining} to remove)
													{/if}
												{:else}
													manual only
												{/if}
											</td>
											<td>
												<div class="font-mono text-xs">
													{formatCoordinate(zone.latitude)}, {formatCoordinate(zone.longitude)}
												</div>
												{#if mapHrefFromZone(zone)}
													<a
														class="link link-primary text-xs"
														href={mapHrefFromZone(zone)}
														target="_blank"
														rel="noopener noreferrer"
													>
														map
													</a>
												{/if}
											</td>
										</tr>
									{/each}
								</tbody>
							</table>
						{/if}
					</div>

					<div class="surface-table-wrap">
						<div class="copy-mini-title">Pending Candidates</div>
						{#if pendingLockoutZones.length === 0}
							<div class="state-empty">No pending candidates.</div>
						{:else}
							<table class="table table-sm min-w-[860px]">
								<thead>
									<tr>
										<th>Slot</th>
										<th>Band</th>
										<th>Freq</th>
										<th>Hits</th>
										<th>Remaining</th>
										<th>Last Seen</th>
										<th>Next Hit</th>
										<th>Location</th>
									</tr>
								</thead>
								<tbody>
									{#each pendingLockoutZones as zone}
										<tr>
											<td class="font-mono text-xs">{zone.slot}</td>
											<td>{zone.band || 'UNK'}</td>
												<td class="whitespace-nowrap">{formatRoundedFrequencyMhz(zone.frequencyMHz)}</td>
											<td>{typeof zone.hitCount === 'number' ? zone.hitCount : '—'}</td>
											<td>{typeof zone.hitsRemaining === 'number' ? zone.hitsRemaining : '—'}</td>
											<td class="text-xs">{formatEpochMs(zone.lastSeenMs)}</td>
											<td class="text-xs">{formatEpochMs(zone.nextEligibleHitMs)}</td>
											<td>
												<div class="font-mono text-xs">
													{formatCoordinate(zone.latitude)}, {formatCoordinate(zone.longitude)}
												</div>
												<a
													class="link link-primary text-xs"
													href={`https://maps.google.com/?q=${zone.latitude},${zone.longitude}`}
													target="_blank"
													rel="noopener noreferrer"
												>
													map
												</a>
											</td>
										</tr>
									{/each}
								</tbody>
							</table>
						{/if}
					</div>
				</div>
			{/if}
		</div>
	</div>

	<div class="surface-card">
		<div class="card-body gap-3">
			<CardSectionHead
				title="Signal Observations"
				subtitle="Recent signals seen during driving. Use observations to create manual lockout zones."
			>
				<button class="btn btn-outline btn-sm" onclick={() => fetchLockoutEvents()} disabled={lockoutLoading}>
					{#if lockoutLoading}
						<span class="loading loading-spinner loading-xs"></span>
					{/if}
					Refresh
				</button>
			</CardSectionHead>

			{#if lockoutError}
				<StatusAlert message={lockoutError} fallbackType="warning" />
			{/if}

			<div class="surface-stats">
				<div class="stat py-3 px-4">
					<div class="stat-title">Buffer</div>
					<div class="stat-value text-base">
						{lockoutStats.size}/{lockoutStats.capacity || '—'}
					</div>
					<div class="stat-desc">recent observations</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Published</div>
					<div class="stat-value text-base">{lockoutStats.published}</div>
					<div class="stat-desc">since boot</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Drops</div>
					<div class="stat-value text-base">{lockoutStats.drops}</div>
					<div class="stat-desc">oldest overwritten</div>
				</div>
				<div class="stat py-3 px-4">
					<div class="stat-title">Latest</div>
					<div class="stat-value text-base">
						{lockoutEvents[0]?.band || '—'}
					</div>
					<div class="stat-desc">
						{lockoutEvents[0]
							? `${formatRoundedFrequencyMhz(lockoutEvents[0].frequencyMHz)} • fix age ${formatFixAgeMs(lockoutEvents[0].fixAgeMs)}`
							: 'no samples yet'}
					</div>
				</div>
			</div>


			{#if loading || lockoutLoading}
				<div class="state-loading tight">
					<span class="loading loading-spinner loading-md"></span>
				</div>
			{:else if lockoutEvents.length === 0}
				<div class="state-empty">
					No candidates logged yet. Run a drive test, then refresh this card.
				</div>
			{:else}
				<div class="surface-table-wrap">
					<table class="table table-sm">
						<thead>
							<tr>
								<th>Seen (boot)</th>
								<th>Band</th>
								<th>Frequency</th>
								<th>Strength</th>
								<th>Fix Age</th>
								<th>Sats</th>
								<th>HDOP</th>
								<th>Location</th>
								<th>Action</th>
							</tr>
						</thead>
						<tbody>
							{#each lockoutEvents as event}
								<tr>
									<td class="font-mono text-xs">{formatBootTime(event.tsMs)}</td>
									<td>{event.band || 'UNK'}</td>
									<td>{formatRoundedFrequencyMhz(event.frequencyMHz)}</td>
									<td>{typeof event.strength === 'number' ? event.strength : '—'}</td>
									<td>{formatFixAgeMs(event.fixAgeMs)}</td>
									<td class:text-error={typeof event.satellites === 'number' && event.satellites < GPS_MIN_SATELLITES}>{typeof event.satellites === 'number' ? event.satellites : '—'}</td>
									<td class:text-error={typeof event.hdop === 'number' && event.hdop > lockoutConfig.maxHdopX10 / 10}>{formatHdop(event.hdop)}</td>
									<td>
										{#if event.locationValid}
											<div class="font-mono text-xs">
												{formatCoordinate(event.latitude)}, {formatCoordinate(event.longitude)}
											</div>
											{#if signalMapHref(event)}
												<a class="link link-primary text-xs" href={signalMapHref(event)} target="_blank" rel="noopener noreferrer">
													map
												</a>
											{/if}
										{:else}
											<span class="copy-caption">no fix</span>
										{/if}
									</td>
									<td>
										{#if event.locationValid}
											<button
												class="btn btn-xs btn-primary btn-outline"
												onclick={() => openZoneFromObservation(event)}
												disabled={!advancedUnlocked || zoneEditorSaving}
											>
												Lock Out
											</button>
										{:else}
											<span class="copy-caption">—</span>
										{/if}
									</td>
								</tr>
							{/each}
						</tbody>
					</table>
				</div>
			{/if}
		</div>
	</div>

	<LockoutZoneEditorModal
		open={zoneEditorOpen}
		zoneSlot={zoneEditorSlot}
		bind:editor={zoneEditor}
		saving={zoneEditorSaving}
		onclose={closeZoneEditor}
		onsave={saveZoneEditor}
	/>

	<LockoutKaWarningModal show={showKaWarningModal} oncancel={cancelKaLearningEnable} onconfirm={confirmKaLearningEnable} />
</div>
