<script>
	import { onMount } from 'svelte';
	import CardSectionHead from '$lib/components/CardSectionHead.svelte';
	import PageHeader from '$lib/components/PageHeader.svelte';

	let loading = $state(true);
	let message = $state(null);
	let statusPoll = $state(null);
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
	let importFileInput;

	const STATUS_POLL_INTERVAL_MS = 2500;
	const LOCKOUT_EVENTS_LIMIT = 24;
	const LOCKOUT_ZONES_LIMIT = 32;
	const FEET_PER_METER = 3.28084;
	const METERS_PER_RADIUS_E5 = 1.11;
	const FEET_PER_RADIUS_E5 = METERS_PER_RADIUS_E5 * FEET_PER_METER;
	const LEARNER_PROMOTION_HITS_DEFAULT = 3;
	const LEARNER_PROMOTION_HITS_MIN = 2;
	const LEARNER_PROMOTION_HITS_MAX = 6;
	const LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT = 10;
	const LEARNER_FREQ_TOLERANCE_MHZ_MIN = 2;
	const LEARNER_FREQ_TOLERANCE_MHZ_MAX = 20;
	const LEARNER_RADIUS_E5_DEFAULT = 135;
	const LEARNER_RADIUS_E5_MIN = 45;
	const LEARNER_RADIUS_E5_MAX = 360;
	const LOCKOUT_INTERVAL_OPTIONS = [0, 1, 4, 12, 24];
	const LEARNER_UNLEARN_COUNT_DEFAULT = 0;
	const LEARNER_UNLEARN_COUNT_MIN = 0;
	const LEARNER_UNLEARN_COUNT_MAX = 10;
	const MANUAL_DEMOTION_OPTIONS = [0, 10, 25, 50];
	const DIRECTION_MODE_OPTIONS = [
		{ value: 'all', label: 'All Directions' },
		{ value: 'forward', label: 'Forward Only' },
		{ value: 'reverse', label: 'Reverse Only' }
	];
	const LOCKOUT_BAND_OPTIONS = [
		{ value: 0x04, label: 'K Band' },
		{ value: 0x02, label: 'Ka Band' },
		{ value: 0x08, label: 'X Band' },
		{ value: 0x01, label: 'Laser' },
		{ value: 0x06, label: 'K + Ka' }
	];
	const LOCKOUT_PRESET_LEGACY_SAFE = {
		name: 'Legacy Safe',
		learnerPromotionHits: 3,
		learnerLearnIntervalHours: 0,
		learnerFreqToleranceMHz: 10,
		learnerRadiusE5: 135,
		learnerUnlearnCount: 0,
		learnerUnlearnIntervalHours: 0,
		manualDemotionMissCount: 0
	};
	const LOCKOUT_PRESET_JBV1_BLEND = {
		name: 'JBV1 Blend',
		learnerPromotionHits: 3,
		learnerLearnIntervalHours: 4,
		learnerFreqToleranceMHz: 10,
		learnerRadiusE5: 135,
		learnerUnlearnCount: 5,
		learnerUnlearnIntervalHours: 4,
		manualDemotionMissCount: 25
	};

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
		preQuiet: false
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
		statusPoll = setInterval(async () => {
			await fetchGpsStatus();
		}, STATUS_POLL_INTERVAL_MS);
		return () => {
			if (statusPoll) clearInterval(statusPoll);
		};
	});

	function setMsg(type, text) {
		message = { type, text };
	}

	function formatFrequencyMhz(mhz) {
		if (typeof mhz !== 'number' || !Number.isFinite(mhz) || mhz <= 0) return '—';
		return `${Math.round(mhz)} MHz`;
	}

	function formatCoordinate(value) {
		if (typeof value !== 'number' || !Number.isFinite(value)) return '—';
		return value.toFixed(5);
	}

	function formatEpochMs(epochMs) {
		if (typeof epochMs !== 'number' || !Number.isFinite(epochMs) || epochMs <= 0) return '—';
		return new Date(epochMs).toLocaleString();
	}

	function formatFixAgeMs(value) {
		if (typeof value !== 'number' || !Number.isFinite(value)) return '—';
		return `${(value / 1000).toFixed(1)}s`;
	}

	function formatBootTime(tsMs) {
		if (typeof tsMs !== 'number' || !Number.isFinite(tsMs)) return '—';
		return `${(tsMs / 1000).toFixed(1)}s`;
	}

	function formatHdop(value) {
		if (typeof value !== 'number' || !Number.isFinite(value)) return '—';
		return value.toFixed(1);
	}

	function formatBandMask(mask) {
		if (typeof mask !== 'number' || !Number.isFinite(mask)) return '—';
		const parts = [];
		if (mask & 0x01) parts.push('Laser');
		if (mask & 0x02) parts.push('Ka');
		if (mask & 0x04) parts.push('K');
		if (mask & 0x08) parts.push('X');
		return parts.length > 0 ? parts.join('+') : '—';
	}

	function formatRadiusFeet(radiusM) {
		if (typeof radiusM !== 'number' || !Number.isFinite(radiusM) || radiusM <= 0) return '—';
		return `${Math.round(radiusM * FEET_PER_METER)} ft`;
	}

	function signalMapHref(event) {
		if (!event?.locationValid) return '';
		if (typeof event.latitude !== 'number' || typeof event.longitude !== 'number') return '';
		return `https://maps.google.com/?q=${event.latitude},${event.longitude}`;
	}

	function clampU16(value) {
		const parsed = Number(value);
		if (!Number.isFinite(parsed)) return 0;
		return Math.max(0, Math.min(65535, Math.round(parsed)));
	}

	function clampInt(value, min, max, fallback) {
		const parsed = Number(value);
		if (!Number.isFinite(parsed)) return fallback;
		return Math.max(min, Math.min(max, Math.round(parsed)));
	}

	function clampLearnerPromotionHits(value) {
		return clampInt(
			value,
			LEARNER_PROMOTION_HITS_MIN,
			LEARNER_PROMOTION_HITS_MAX,
			LEARNER_PROMOTION_HITS_DEFAULT
		);
	}

	function clampLearnerRadiusE5(value) {
		return clampInt(value, LEARNER_RADIUS_E5_MIN, LEARNER_RADIUS_E5_MAX, LEARNER_RADIUS_E5_DEFAULT);
	}

	function clampLearnerFreqToleranceMHz(value) {
		return clampInt(
			value,
			LEARNER_FREQ_TOLERANCE_MHZ_MIN,
			LEARNER_FREQ_TOLERANCE_MHZ_MAX,
			LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT
		);
	}

	function clampIntervalHours(value) {
		const parsed = Number(value);
		if (!Number.isFinite(parsed)) return 0;
		if (parsed <= 0) return 0;
		if (parsed <= 1) return 1;
		if (parsed <= 4) return 4;
		if (parsed <= 12) return 12;
		return 24;
	}

	function clampUnlearnCount(value) {
		return clampInt(value, LEARNER_UNLEARN_COUNT_MIN, LEARNER_UNLEARN_COUNT_MAX, LEARNER_UNLEARN_COUNT_DEFAULT);
	}

	function clampManualDemotionMissCount(value) {
		const parsed = Number(value);
		if (!Number.isFinite(parsed) || parsed <= 0) return 0;
		if (parsed <= 10) return 10;
		if (parsed <= 25) return 25;
		return 50;
	}

	function radiusE5ToFeet(radiusE5) {
		return Math.round(clampLearnerRadiusE5(radiusE5) * FEET_PER_RADIUS_E5);
	}

	function feetToRadiusE5(radiusFeet) {
		const parsedFeet = Number(radiusFeet);
		if (!Number.isFinite(parsedFeet)) return LEARNER_RADIUS_E5_DEFAULT;
		return clampLearnerRadiusE5(Math.round(parsedFeet / FEET_PER_RADIUS_E5));
	}

	function normalizeLearnerRadiusFeet(radiusFeet) {
		return radiusE5ToFeet(feetToRadiusE5(radiusFeet));
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
		const preQuiet =
			typeof lockout.preQuiet === 'boolean'
				? lockout.preQuiet
				: typeof data?.lockoutPreQuiet === 'boolean'
					? data.lockoutPreQuiet
					: typeof data?.gpsLockoutPreQuiet === 'boolean'
						? data.gpsLockoutPreQuiet
						: false;
		lockoutConfig = {
			modeRaw: typeof lockout.modeRaw === 'number' ? lockout.modeRaw : 0,
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
			preQuiet: !!preQuiet
		};
		lockoutConfigInitialized = true;
	}

	function markLockoutDirty() {
		lockoutConfigDirty = true;
	}

	function formatZoneRadiusFeet(zone) {
		if (typeof zone?.radiusE5 === 'number' && Number.isFinite(zone.radiusE5) && zone.radiusE5 > 0) {
			return `${radiusE5ToFeet(zone.radiusE5)} ft`;
		}
		return formatRadiusFeet(zone?.radiusM);
	}

	function defaultZoneEditorState() {
		return {
			latitude: '',
			longitude: '',
			radiusFt: radiusE5ToFeet(LEARNER_RADIUS_E5_DEFAULT),
			bandMask: 0x04,
			frequencyMHz: '',
			frequencyToleranceMHz: LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT,
			confidence: 100,
			directionMode: 'all',
			headingDeg: '',
			headingToleranceDeg: 45
		};
	}

	function lockoutZoneSourceLabel(zone) {
		if (zone?.manual && zone?.learned) return 'manual+learned';
		if (zone?.manual) return 'manual';
		if (zone?.learned) return 'learned';
		return 'active';
	}

	function normalizeDirectionMode(value) {
		const token = typeof value === 'string' ? value.trim().toLowerCase() : '';
		if (token === 'forward' || token === 'reverse') return token;
		return 'all';
	}

	function formatDirectionSummary(zone) {
		const mode = normalizeDirectionMode(zone?.directionMode);
		if (mode === 'all') return 'All';
		const heading = typeof zone?.headingDeg === 'number' ? `${Math.round(zone.headingDeg)}°` : '—';
		const tolerance =
			typeof zone?.headingToleranceDeg === 'number' ? Math.round(zone.headingToleranceDeg) : 45;
		return `${mode === 'forward' ? 'Forward' : 'Reverse'} ${heading} ±${tolerance}°`;
	}

	function mapHrefFromZone(zone) {
		if (typeof zone?.latitude !== 'number' || typeof zone?.longitude !== 'number') return '';
		return `https://maps.google.com/?q=${zone.latitude},${zone.longitude}`;
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

	function formatIntervalLabel(hours) {
		const clamped = clampIntervalHours(hours);
		return clamped > 0 ? `${clamped}h` : 'disabled';
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
			!!lockoutConfig.kaLearningEnabled === !!runtime.kaLearningEnabled
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
			const res = await fetch('/api/gps/status');
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
			const res = await fetch(`/api/lockouts/events?limit=${LOCKOUT_EVENTS_LIMIT}`);
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
			const res = await fetch(
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
				lockoutPreQuiet: !!lockoutConfig.preQuiet
			};
			const res = await fetch('/api/gps/config', {
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
			const res = await fetch(creating ? '/api/lockouts/zones/create' : '/api/lockouts/zones/update', {
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
			const res = await fetch('/api/lockouts/zones/delete', {
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
			const res = await fetch('/api/lockouts/zones/export');
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
			setMsg('success', 'Exported lockout zones.');
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
		if (!confirm(`Import lockout zones from ${file.name}? This replaces current in-memory zones.`)) {
			input.value = '';
			return;
		}
		importingZones = true;
		try {
			const payload = await file.text();
			JSON.parse(payload);
			const res = await fetch('/api/lockouts/zones/import', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: payload
			});
			const data = await res.json().catch(() => ({}));
			if (!res.ok) {
				setMsg('error', data.message || `Failed to import lockout zones (${res.status})`);
				return;
			}
			zoneEditorOpen = false;
			zoneEditorSlot = null;
			zoneEditor = defaultZoneEditorState();
			const importedCount = typeof data.entriesImported === 'number' ? data.entriesImported : null;
			setMsg(
				'success',
				importedCount === null ? 'Imported lockout zones.' : `Imported ${importedCount} lockout zones.`
			);
			await fetchLockoutZones({ silent: true });
		} catch (e) {
			setMsg('error', e?.message ? `Failed to import lockout zones (${e.message})` : 'Failed to import lockout zones');
		} finally {
			importingZones = false;
			input.value = '';
		}
	}
</script>

<div class="page-stack">
	<PageHeader
		title="Lockouts"
		subtitle="Dedicated lockout controls and observability with safety-gated runtime and learner tuning."
	>
		<div class="flex gap-2">
			<a href="/integrations" class="btn btn-outline btn-sm">OBD & GPS</a>
			<button class="btn btn-outline btn-sm" onclick={refreshAll}>Refresh All</button>
		</div>
	</PageHeader>

	{#if message}
		<div
			class="surface-alert alert-{message.type === 'error' ? 'error' : message.type === 'success' ? 'success' : 'info'}"
			role="status"
			aria-live="polite"
		>
			<span>{message.text}</span>
		</div>
	{/if}

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
				Use caution in `Enforce` mode. Bad lockout settings can mute real threats.
			</div>
		</div>
		</div>

		<div class="surface-card">
		<div class="card-body gap-3">
			<CardSectionHead
				title="Lockout Runtime Controls"
				subtitle="Live runtime controls currently available in firmware."
			>
				<div class="flex gap-2">
					{#if lockoutConfigDirty}
						<div class="badge badge-warning badge-sm">staged changes</div>
					{:else if lockoutConfigMatchesBackend()}
						<div class="badge badge-success badge-sm">backend synced</div>
					{:else}
						<div class="badge badge-ghost badge-sm">awaiting runtime sync</div>
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

			<div class="grid grid-cols-1 md:grid-cols-2 gap-3">
				<label class="form-control">
					<span class="label-text text-sm">Mode</span>
					<select
						class="select select-bordered select-sm"
						bind:value={lockoutConfig.modeRaw}
						onchange={markLockoutDirty}
						disabled={!advancedUnlocked}
					>
						<option value={0}>Off</option>
						<option value={1}>Shadow (read-only)</option>
						<option value={2}>Advisory (read-only)</option>
						<option value={3}>Enforce (risk: can mute alerts)</option>
					</select>
				</label>
				<label class="label cursor-pointer justify-start gap-3 py-0">
					<span class="label-text text-sm">Core guard</span>
					<input
						type="checkbox"
						class="toggle toggle-primary toggle-sm"
						checked={!!lockoutConfig.coreGuardEnabled}
						disabled={!advancedUnlocked}
						onchange={(e) => {
							lockoutConfig.coreGuardEnabled = e.currentTarget.checked;
							markLockoutDirty();
						}}
					/>
				</label>
				{#if lockoutConfig.modeRaw === 3}
				<label class="label cursor-pointer justify-start gap-3 py-0">
					<div>
						<span class="label-text text-sm">Pre-quiet in lockout zones</span>
						<p class="copy-caption">Drop V1 volume to mute-volume when entering a lockout zone. Restores instantly on real alerts.</p>
					</div>
					<input
						type="checkbox"
						class="toggle toggle-primary toggle-sm"
						checked={!!lockoutConfig.preQuiet}
						disabled={!advancedUnlocked}
						onchange={(e) => {
							lockoutConfig.preQuiet = e.currentTarget.checked;
							markLockoutDirty();
						}}
					/>
				</label>
				{/if}
				<label class="form-control">
					<span class="label-text text-sm">Max queue drops (0 = strictest)</span>
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
					<span class="label-text text-sm">Max perf drops (0 = strictest)</span>
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
					<span class="label-text text-sm">Max event-bus drops (0 = strictest)</span>
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
			<div class="copy-meta">
				Core guard thresholds: <code>0</code> trips guard on the first drop event.
			</div>

			<div class="copy-meta">
				Current mode: {gpsStatus?.lockout?.mode || 'off'} · enforce allowed:{' '}
				{gpsStatus?.lockout?.enforceAllowed ? 'yes' : 'no'} · core guard:{' '}
				{gpsStatus?.lockout?.coreGuardTripped ? 'tripped' : 'clear'}
				{#if gpsStatus?.lockout?.coreGuardReason}
					· reason: {gpsStatus.lockout.coreGuardReason}
				{/if}
			</div>
		</div>
	</div>

		<div class="surface-card">
			<div class="card-body gap-3">
				<CardSectionHead
					title="Learner Settings"
					subtitle="Writes apply immediately and persist in settings. Use conservative values to reduce false muting."
				/>
				<div class="flex flex-wrap gap-2">
					<button
						class="btn btn-outline btn-xs"
						onclick={() => stageLearnerPreset(LOCKOUT_PRESET_LEGACY_SAFE)}
						disabled={!advancedUnlocked}
					>
						Stage Legacy Safe
					</button>
					<button
						class="btn btn-outline btn-xs"
						onclick={() => stageLearnerPreset(LOCKOUT_PRESET_JBV1_BLEND)}
						disabled={!advancedUnlocked}
					>
						Stage JBV1 Blend
					</button>
				</div>
				<div class="copy-meta">
					JBV1 blend stages: 3 hits · 4h learn interval · ±10 MHz · 492 ft · unlearn 5 misses / 4h · manual delete 25 misses.
				</div>
				<div class="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-4 gap-3">
					<label class="form-control">
						<span class="label-text text-sm">Hits to promote</span>
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
						<span class="label-text text-sm">Learn interval (hours, 0 = disabled)</span>
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
							{#each LOCKOUT_INTERVAL_OPTIONS as option}
								<option value={option}>{option === 0 ? 'Disabled' : `${option} hours`}</option>
							{/each}
						</select>
					</label>
					<label class="form-control">
						<span class="label-text text-sm">Drift tolerance (MHz)</span>
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
						<span class="label-text text-sm">Lockout radius (ft)</span>
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
						<span class="label-text text-sm">Unlearn count (0 = legacy)</span>
						<input
							type="number"
							min={LEARNER_UNLEARN_COUNT_MIN}
							max={LEARNER_UNLEARN_COUNT_MAX}
							class="input input-bordered input-sm"
							value={lockoutConfig.learnerUnlearnCount}
							disabled={!advancedUnlocked}
							onchange={(e) => {
								lockoutConfig.learnerUnlearnCount = clampUnlearnCount(e.currentTarget.value);
								markLockoutDirty();
							}}
						/>
					</label>
					<label class="form-control">
						<span class="label-text text-sm">Unlearn interval (hours, 0 = disabled)</span>
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
							{#each LOCKOUT_INTERVAL_OPTIONS as option}
								<option value={option}>{option === 0 ? 'Disabled' : `${option} hours`}</option>
							{/each}
						</select>
					</label>
					<label class="form-control">
						<span class="label-text text-sm">Manual delete misses (0 = disabled)</span>
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
							{#each MANUAL_DEMOTION_OPTIONS as option}
								<option value={option}>{option === 0 ? 'Disabled' : `${option} misses`}</option>
							{/each}
						</select>
					</label>
				</div>
				<label class="label cursor-pointer justify-start gap-3 py-0">
					<span class="label-text text-sm">Ka lockout learning (high risk)</span>
					<input
						type="checkbox"
						class="toggle toggle-warning toggle-sm"
						checked={!!lockoutConfig.kaLearningEnabled}
						disabled={!advancedUnlocked}
						onchange={(e) => {
							requestKaLearningToggle(e.currentTarget.checked);
						}}
					/>
				</label>
				<div class="copy-caption-soft text-warning">
					Disabled by default. Enabling Ka learning can suppress real Ka threats if lockouts are wrong.
				</div>
				<div class="copy-meta">
					Runtime learner: {runtimeLearnerHits()} hits · interval {formatIntervalLabel(runtimeLearnerLearnIntervalHours())}
					· ±{runtimeLearnerFreqToleranceMHz()} MHz · {runtimeLearnerRadiusFeetText()}
					· unlearn {runtimeLearnerUnlearnCount()} misses / {formatIntervalLabel(runtimeLearnerUnlearnIntervalHours())}
					· manual delete {runtimeManualDemotionMissCount() || 'disabled'}
					· Ka learning {gpsStatus?.lockout?.kaLearningEnabled ? 'enabled' : 'disabled'}
					· candidate expiry: 7 days
				</div>
			</div>
		</div>

	<div class="surface-card">
		<div class="card-body gap-3">
			<CardSectionHead
				title="Lockout Zones"
				subtitle="Review active lockouts and pending learner candidates. Manual zones can be created, edited, exported, or imported from JSON."
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
				<div class="surface-alert alert-warning py-2" role="status">
					<span>{lockoutZonesError}</span>
				</div>
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
										<th>Demote</th>
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
											<td class="whitespace-nowrap">{formatFrequencyMhz(zone.frequencyMHz)}</td>
											<td>{typeof zone.confidence === 'number' ? zone.confidence : '—'}</td>
											<td class="whitespace-nowrap">{formatZoneRadiusFeet(zone)}</td>
											<td class="text-xs whitespace-nowrap">{formatDirectionSummary(zone)}</td>
											<td class="text-xs">
												{#if typeof zone.demotionMissThreshold === 'number'}
													{zone.missCount ?? 0}/{zone.demotionMissThreshold}
													{#if typeof zone.demotionMissesRemaining === 'number'}
														({zone.demotionMissesRemaining} left)
													{/if}
												{:else}
													legacy decay
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
												<td class="whitespace-nowrap">{formatFrequencyMhz(zone.frequencyMHz)}</td>
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
				title="Lockout Candidates"
				subtitle="Recent signal observations for post-drive lockout review."
			>
				<button class="btn btn-outline btn-sm" onclick={() => fetchLockoutEvents()} disabled={lockoutLoading}>
					{#if lockoutLoading}
						<span class="loading loading-spinner loading-xs"></span>
					{/if}
					Refresh
				</button>
			</CardSectionHead>

			{#if lockoutError}
				<div class="surface-alert alert-warning py-2" role="status">
					<span>{lockoutError}</span>
				</div>
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
							? `${formatFrequencyMhz(lockoutEvents[0].frequencyMHz)} • fix age ${formatFixAgeMs(lockoutEvents[0].fixAgeMs)}`
							: 'no samples yet'}
					</div>
				</div>
			</div>

			<div class="copy-meta">
				SD {lockoutSd.enabled ? 'enabled' : 'disabled'} · writes {lockoutSd.written} · deduped {lockoutSd.deduped}
				· queue drops {lockoutSd.queueDrops} · write fail {lockoutSd.writeFail} · rotations {lockoutSd.rotations}
				{#if lockoutSd.path}
					· <span class="font-mono">{lockoutSd.path}</span>
				{/if}
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
							</tr>
						</thead>
						<tbody>
							{#each lockoutEvents as event}
								<tr>
									<td class="font-mono text-xs">{formatBootTime(event.tsMs)}</td>
									<td>{event.band || 'UNK'}</td>
									<td>{formatFrequencyMhz(event.frequencyMHz)}</td>
									<td>{typeof event.strength === 'number' ? event.strength : '—'}</td>
									<td>{formatFixAgeMs(event.fixAgeMs)}</td>
									<td>{typeof event.satellites === 'number' ? event.satellites : '—'}</td>
									<td>{formatHdop(event.hdop)}</td>
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
								</tr>
							{/each}
						</tbody>
					</table>
				</div>
			{/if}
		</div>
	</div>

	{#if zoneEditorOpen}
		<div class="modal modal-open">
			<div class="modal-box surface-modal max-w-3xl">
				<h3 class="font-bold text-lg">
					{zoneEditorSlot === null ? 'Create Manual Lockout Zone' : `Edit Lockout Zone ${zoneEditorSlot}`}
				</h3>
				<p class="py-2 copy-subtle">
					Use conservative values. Invalid coordinates or heading values are rejected by the firmware.
				</p>

				<div class="grid grid-cols-1 md:grid-cols-2 gap-3">
					<label class="form-control">
						<span class="label-text text-sm">Latitude</span>
						<input
							type="number"
							step="0.00001"
							min="-90"
							max="90"
							class="input input-bordered input-sm"
							value={zoneEditor.latitude}
							onchange={(e) => {
								zoneEditor.latitude = e.currentTarget.value;
							}}
						/>
					</label>
					<label class="form-control">
						<span class="label-text text-sm">Longitude</span>
						<input
							type="number"
							step="0.00001"
							min="-180"
							max="180"
							class="input input-bordered input-sm"
							value={zoneEditor.longitude}
							onchange={(e) => {
								zoneEditor.longitude = e.currentTarget.value;
							}}
						/>
					</label>
					<label class="form-control">
						<span class="label-text text-sm">Band</span>
						<select
							class="select select-bordered select-sm"
							value={zoneEditor.bandMask}
							onchange={(e) => {
								zoneEditor.bandMask = clampInt(e.currentTarget.value, 1, 255, 0x04);
							}}
						>
							{#each LOCKOUT_BAND_OPTIONS as option}
								<option value={option.value}>{option.label}</option>
							{/each}
						</select>
					</label>
					<label class="form-control">
						<span class="label-text text-sm">Frequency (MHz, optional)</span>
						<input
							type="number"
							min="0"
							max="65535"
							class="input input-bordered input-sm"
							value={zoneEditor.frequencyMHz}
							onchange={(e) => {
								zoneEditor.frequencyMHz = e.currentTarget.value;
							}}
						/>
					</label>
					<label class="form-control">
						<span class="label-text text-sm">Frequency tolerance (MHz)</span>
						<input
							type="number"
							min="0"
							max="65535"
							class="input input-bordered input-sm"
							value={zoneEditor.frequencyToleranceMHz}
							onchange={(e) => {
								zoneEditor.frequencyToleranceMHz = clampInt(
									e.currentTarget.value,
									0,
									65535,
									LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT
								);
							}}
						/>
					</label>
					<label class="form-control">
						<span class="label-text text-sm">Radius (ft)</span>
						<input
							type="number"
							min={radiusE5ToFeet(LEARNER_RADIUS_E5_MIN)}
							max={radiusE5ToFeet(LEARNER_RADIUS_E5_MAX)}
							class="input input-bordered input-sm"
							value={zoneEditor.radiusFt}
							onchange={(e) => {
								zoneEditor.radiusFt = normalizeLearnerRadiusFeet(e.currentTarget.value);
							}}
						/>
					</label>
					<label class="form-control">
						<span class="label-text text-sm">Confidence</span>
						<input
							type="number"
							min="0"
							max="255"
							class="input input-bordered input-sm"
							value={zoneEditor.confidence}
							onchange={(e) => {
								zoneEditor.confidence = clampInt(e.currentTarget.value, 0, 255, 100);
							}}
						/>
					</label>
					<label class="form-control">
						<span class="label-text text-sm">Direction mode</span>
						<select
							class="select select-bordered select-sm"
							value={zoneEditor.directionMode}
							onchange={(e) => {
								zoneEditor.directionMode = normalizeDirectionMode(e.currentTarget.value);
								if (zoneEditor.directionMode === 'all') {
									zoneEditor.headingDeg = '';
								}
							}}
						>
							{#each DIRECTION_MODE_OPTIONS as option}
								<option value={option.value}>{option.label}</option>
							{/each}
						</select>
					</label>
					<label class="form-control">
						<span class="label-text text-sm">Heading tolerance (degrees)</span>
						<input
							type="number"
							min="0"
							max="90"
							class="input input-bordered input-sm"
							value={zoneEditor.headingToleranceDeg}
							onchange={(e) => {
								zoneEditor.headingToleranceDeg = clampInt(e.currentTarget.value, 0, 90, 45);
							}}
						/>
					</label>
					{#if zoneEditor.directionMode !== 'all'}
						<label class="form-control md:col-span-2">
							<span class="label-text text-sm">Heading (0-359 degrees)</span>
							<input
								type="number"
								min="0"
								max="359"
								class="input input-bordered input-sm"
								value={zoneEditor.headingDeg}
								onchange={(e) => {
									zoneEditor.headingDeg = e.currentTarget.value;
								}}
							/>
						</label>
					{/if}
				</div>

				<div class="modal-action">
					<button class="btn btn-outline btn-sm" onclick={closeZoneEditor} disabled={zoneEditorSaving}>
						Cancel
					</button>
					<button class="btn btn-primary btn-sm" onclick={saveZoneEditor} disabled={zoneEditorSaving}>
						{#if zoneEditorSaving}
							<span class="loading loading-spinner loading-xs"></span>
						{/if}
						Save Zone
					</button>
				</div>
			</div>
		</div>
	{/if}

	{#if showKaWarningModal}
		<div class="modal modal-open">
			<div class="modal-box surface-modal">
				<h3 class="font-bold text-lg">Ka Lockout Learning Warning</h3>
				<p class="py-3 copy-caption-soft text-warning">
					High risk: enabling Ka lockout learning can suppress real Ka threats.
				</p>
				<p class="copy-caption-soft">
					Ka lockouts can hide real threats. Keep this off unless you fully understand the risk.
				</p>
				<div class="modal-action">
					<button class="btn btn-outline btn-sm" onclick={cancelKaLearningEnable}>
						Cancel (recommended)
					</button>
					<button class="btn btn-warning btn-sm" onclick={confirmKaLearningEnable}>
						Enable anyway (high risk)
					</button>
				</div>
			</div>
		</div>
	{/if}
</div>
