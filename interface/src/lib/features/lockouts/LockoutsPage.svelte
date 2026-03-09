<script>
	import { onMount } from 'svelte';
	import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
	import PageHeader from '$lib/components/PageHeader.svelte';
	import StatusAlert from '$lib/components/StatusAlert.svelte';
	import LockoutKaWarningModal from '$lib/components/LockoutKaWarningModal.svelte';
	import LockoutZoneEditorModal from '$lib/components/LockoutZoneEditorModal.svelte';
	import LockoutGpsQualityCard from '$lib/components/lockouts/LockoutGpsQualityCard.svelte';
	import LockoutLearningRulesCard from '$lib/components/lockouts/LockoutLearningRulesCard.svelte';
	import LockoutModeCard from '$lib/components/lockouts/LockoutModeCard.svelte';
	import LockoutObservationsCard from '$lib/components/lockouts/LockoutObservationsCard.svelte';
	import LockoutSafetyGateCard from '$lib/components/lockouts/LockoutSafetyGateCard.svelte';
	import LockoutZonesCard from '$lib/components/lockouts/LockoutZonesCard.svelte';
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
		normalizeDirectionMode,
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
	let importFileInput = $state(null);

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
		const pendingCount = lockoutZonesStats.pendingCount || 0;
		const parts = [];
		if (zoneCount > 0) parts.push(`${zoneCount} active`);
		if (pendingCount > 0) parts.push(`${pendingCount} pending`);
		const desc = parts.length > 0 ? parts.join(' + ') : '0';
		if (!confirm(`Delete ALL ${desc} lockout zones? This cannot be undone.\n\nConsider exporting first.`)) {
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
			// Also clear pending learner candidates so they don't re-promote.
			if (pendingCount > 0) {
				try {
					await fetchWithTimeout('/api/lockouts/pending/clear', { method: 'POST' });
				} catch (_) {
					// Best-effort; active zones already cleared.
				}
			}
			zoneEditorOpen = false;
			zoneEditorSlot = null;
			zoneEditor = defaultZoneEditorState();
			setMsg('success', `Cleared ${desc} lockout zones.`);
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

	<LockoutSafetyGateCard bind:advancedUnlocked />

	<LockoutGpsQualityCard {gpsStatus} {lockoutConfig} />

	<LockoutModeCard
		{advancedUnlocked}
		bind:lockoutConfig
		{lockoutConfigDirty}
		backendSynced={lockoutConfigMatchesBackend()}
		{savingLockoutConfig}
		reloadStatus={fetchGpsStatus}
		saveConfig={saveLockoutConfig}
		markDirty={markLockoutDirty}
	/>

	<LockoutLearningRulesCard
		{advancedUnlocked}
		bind:lockoutConfig
		{stageLearnerPreset}
		{requestKaLearningToggle}
		markDirty={markLockoutDirty}
	/>

	<LockoutZonesCard
		{advancedUnlocked}
		{zoneEditorSaving}
		{importingZones}
		{exportingZones}
		{clearingAllZones}
		{lockoutZonesLoading}
		{lockoutZonesError}
		{lockoutZonesStats}
		{activeLockoutZones}
		{pendingLockoutZones}
		bind:importFileInput
		{openZoneCreateEditor}
		{promptLockoutImport}
		{exportLockoutZones}
		refreshZones={fetchLockoutZones}
		{clearAllZones}
		{handleImportFileSelected}
		{openZoneEditEditor}
		{deleteZone}
		{deletingZoneSlot}
	/>

	<LockoutObservationsCard
		{loading}
		{lockoutLoading}
		{lockoutError}
		{lockoutStats}
		{lockoutEvents}
		{lockoutConfig}
		{advancedUnlocked}
		{zoneEditorSaving}
		refreshEvents={fetchLockoutEvents}
		{openZoneFromObservation}
	/>

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
