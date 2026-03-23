// Lockout constants and pure utility functions.
// Extracted from lockouts/+page.svelte for reuse and readability.

// --- Polling / limits ---
export const STATUS_POLL_INTERVAL_MS = 2500;
export const LOCKOUT_EVENTS_LIMIT = 24;
export const LOCKOUT_ZONES_LIMIT = 32;

// --- Unit conversions ---
const FEET_PER_METER = 3.28084;
const METERS_PER_RADIUS_E5 = 1.11;
export const FEET_PER_RADIUS_E5 = METERS_PER_RADIUS_E5 * FEET_PER_METER;

// --- Learner defaults / ranges ---
export const LEARNER_PROMOTION_HITS_DEFAULT = 3;
export const LEARNER_PROMOTION_HITS_MIN = 2;
export const LEARNER_PROMOTION_HITS_MAX = 6;
export const LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT = 10;
export const LEARNER_FREQ_TOLERANCE_MHZ_MIN = 2;
export const LEARNER_FREQ_TOLERANCE_MHZ_MAX = 20;
export const LEARNER_RADIUS_E5_DEFAULT = 135;
export const LEARNER_RADIUS_E5_MIN = 45;
export const LEARNER_RADIUS_E5_MAX = 360;
export const LEARNER_UNLEARN_COUNT_DEFAULT = 0;
const LEARNER_UNLEARN_COUNT_MIN = 0;
const LEARNER_UNLEARN_COUNT_MAX = 10;

// --- GPS quality defaults / ranges ---
export const GPS_MAX_HDOP_X10_DEFAULT = 50;
export const GPS_MAX_HDOP_X10_MIN = 10;
export const GPS_MAX_HDOP_X10_MAX = 100;
export const GPS_MIN_LEARNER_SPEED_MPH_DEFAULT = 5;
export const GPS_MIN_LEARNER_SPEED_MPH_MIN = 0;
export const GPS_MIN_LEARNER_SPEED_MPH_MAX = 20;
export const GPS_MIN_SATELLITES = 4;

// --- Zone editor option arrays ---
export const DIRECTION_MODE_OPTIONS = [
	{ value: 'all', label: 'All Directions' },
	{ value: 'forward', label: 'Forward Only' },
	{ value: 'reverse', label: 'Reverse Only' }
];

export const LOCKOUT_BAND_OPTIONS = [
	{ value: 0x04, label: 'K Band' },
	{ value: 0x02, label: 'Ka Band' },
	{ value: 0x08, label: 'X Band' },
	{ value: 0x01, label: 'Laser' },
	{ value: 0x06, label: 'K + Ka' }
];

// --- Presets ---
export const LOCKOUT_PRESET_LEGACY_SAFE = {
	name: 'Legacy Safe',
	learnerPromotionHits: 3,
	learnerLearnIntervalHours: 0,
	learnerFreqToleranceMHz: 10,
	learnerRadiusE5: 135,
	learnerUnlearnCount: 0,
	learnerUnlearnIntervalHours: 0,
	manualDemotionMissCount: 0
};

export const LOCKOUT_PRESET_BALANCED_BLEND = {
	name: 'Balanced Blend',
	learnerPromotionHits: 3,
	learnerLearnIntervalHours: 4,
	learnerFreqToleranceMHz: 10,
	learnerRadiusE5: 135,
	learnerUnlearnCount: 5,
	learnerUnlearnIntervalHours: 4,
	manualDemotionMissCount: 25
};

// --- Clamping functions ---

export function clampU16(value) {
	const parsed = Number(value);
	if (!Number.isFinite(parsed)) return 0;
	return Math.max(0, Math.min(65535, Math.round(parsed)));
}

export function clampInt(value, min, max, fallback) {
	const parsed = Number(value);
	if (!Number.isFinite(parsed)) return fallback;
	return Math.max(min, Math.min(max, Math.round(parsed)));
}

export function clampLearnerPromotionHits(value) {
	return clampInt(value, LEARNER_PROMOTION_HITS_MIN, LEARNER_PROMOTION_HITS_MAX, LEARNER_PROMOTION_HITS_DEFAULT);
}

export function clampHdopX10(value) {
	return clampInt(value, GPS_MAX_HDOP_X10_MIN, GPS_MAX_HDOP_X10_MAX, GPS_MAX_HDOP_X10_DEFAULT);
}

export function clampMinLearnerSpeed(value) {
	return clampInt(value, GPS_MIN_LEARNER_SPEED_MPH_MIN, GPS_MIN_LEARNER_SPEED_MPH_MAX, GPS_MIN_LEARNER_SPEED_MPH_DEFAULT);
}

export function clampLearnerRadiusE5(value) {
	return clampInt(value, LEARNER_RADIUS_E5_MIN, LEARNER_RADIUS_E5_MAX, LEARNER_RADIUS_E5_DEFAULT);
}

export function clampLearnerFreqToleranceMHz(value) {
	return clampInt(value, LEARNER_FREQ_TOLERANCE_MHZ_MIN, LEARNER_FREQ_TOLERANCE_MHZ_MAX, LEARNER_FREQ_TOLERANCE_MHZ_DEFAULT);
}

export function clampIntervalHours(value) {
	const parsed = Number(value);
	if (!Number.isFinite(parsed)) return 0;
	if (parsed <= 0) return 0;
	if (parsed <= 1) return 1;
	if (parsed <= 4) return 4;
	if (parsed <= 12) return 12;
	return 24;
}

export function clampUnlearnCount(value) {
	return clampInt(value, LEARNER_UNLEARN_COUNT_MIN, LEARNER_UNLEARN_COUNT_MAX, LEARNER_UNLEARN_COUNT_DEFAULT);
}

export function clampManualDemotionMissCount(value) {
	const parsed = Number(value);
	if (!Number.isFinite(parsed) || parsed <= 0) return 0;
	if (parsed <= 10) return 10;
	if (parsed <= 25) return 25;
	return 50;
}

// --- Radius conversions ---

export function radiusE5ToFeet(radiusE5) {
	return Math.round(clampLearnerRadiusE5(radiusE5) * FEET_PER_RADIUS_E5);
}

export function feetToRadiusE5(radiusFeet) {
	const parsedFeet = Number(radiusFeet);
	if (!Number.isFinite(parsedFeet)) return LEARNER_RADIUS_E5_DEFAULT;
	return clampLearnerRadiusE5(Math.round(parsedFeet / FEET_PER_RADIUS_E5));
}

export function normalizeLearnerRadiusFeet(radiusFeet) {
	return radiusE5ToFeet(feetToRadiusE5(radiusFeet));
}

// --- Formatting helpers ---

export function formatCoordinate(value) {
	if (typeof value !== 'number' || !Number.isFinite(value)) return '—';
	return value.toFixed(5);
}

export function formatEpochMs(epochMs) {
	if (typeof epochMs !== 'number' || !Number.isFinite(epochMs) || epochMs <= 0) return '—';
	return new Date(epochMs).toLocaleString();
}

export function formatFixAgeMs(value) {
	if (typeof value !== 'number' || !Number.isFinite(value)) return '—';
	return `${(value / 1000).toFixed(1)}s`;
}

export function formatBootTime(tsMs) {
	if (typeof tsMs !== 'number' || !Number.isFinite(tsMs)) return '—';
	return `${(tsMs / 1000).toFixed(1)}s`;
}

export function formatHdop(value) {
	if (typeof value !== 'number' || !Number.isFinite(value)) return '—';
	return value.toFixed(1);
}

export function formatBandMask(mask) {
	if (typeof mask !== 'number' || !Number.isFinite(mask)) return '—';
	const parts = [];
	if (mask & 0x01) parts.push('Laser');
	if (mask & 0x02) parts.push('Ka');
	if (mask & 0x04) parts.push('K');
	if (mask & 0x08) parts.push('X');
	return parts.length > 0 ? parts.join('+') : '—';
}

export function formatRadiusFeet(radiusM) {
	if (typeof radiusM !== 'number' || !Number.isFinite(radiusM) || radiusM <= 0) return '—';
	return `${Math.round(radiusM * FEET_PER_METER)} ft`;
}

export function formatIntervalLabel(hours) {
	const clamped = clampIntervalHours(hours);
	return clamped > 0 ? `${clamped}h` : 'disabled';
}

export function signalMapHref(event) {
	if (!event?.locationValid) return '';
	if (typeof event.latitude !== 'number' || typeof event.longitude !== 'number') return '';
	return `https://maps.google.com/?q=${event.latitude},${event.longitude}`;
}

export function formatZoneRadiusFeet(zone) {
	if (typeof zone?.radiusE5 === 'number' && Number.isFinite(zone.radiusE5) && zone.radiusE5 > 0) {
		return `${radiusE5ToFeet(zone.radiusE5)} ft`;
	}
	return formatRadiusFeet(zone?.radiusM);
}

// --- Zone helpers ---

export function normalizeDirectionMode(value) {
	const token = typeof value === 'string' ? value.trim().toLowerCase() : '';
	if (token === 'forward' || token === 'reverse') return token;
	return 'all';
}

export function formatDirectionSummary(zone) {
	const mode = normalizeDirectionMode(zone?.directionMode);
	if (mode === 'all') return 'All';
	const heading = typeof zone?.headingDeg === 'number' ? `${Math.round(zone.headingDeg)}°` : '—';
	const tolerance =
		typeof zone?.headingToleranceDeg === 'number' ? Math.round(zone.headingToleranceDeg) : 45;
	return `${mode === 'forward' ? 'Forward' : 'Reverse'} ${heading} ±${tolerance}°`;
}

export function mapHrefFromZone(zone) {
	if (typeof zone?.latitude !== 'number' || typeof zone?.longitude !== 'number') return '';
	return `https://maps.google.com/?q=${zone.latitude},${zone.longitude}`;
}

export function lockoutZoneSourceLabel(zone) {
	if (zone?.manual && zone?.learned) return 'manual+learned';
	if (zone?.manual) return 'manual';
	if (zone?.learned) return 'learned';
	return 'active';
}

export function bandNameToMask(bandName) {
	if (!bandName || typeof bandName !== 'string') return 0x04;
	const name = bandName.toUpperCase().trim();
	if (name === 'KA' || name === 'KA BAND') return 0x02;
	if (name === 'K' || name === 'K BAND') return 0x04;
	if (name === 'X' || name === 'X BAND') return 0x08;
	if (name === 'LASER' || name === 'LA') return 0x01;
	return 0x04;
}

export function defaultZoneEditorState() {
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
