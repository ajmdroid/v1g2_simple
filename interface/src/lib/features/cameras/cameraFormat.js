export const RANGE_MIN_CM = 16093;
export const RANGE_MAX_CM = 160934;

const CM_PER_MILE = 160934;
const CM_PER_FOOT = 30.48;

export function cmToMiles(cm) {
	return (cm / CM_PER_MILE).toFixed(2);
}

export function cmToFeet(cm) {
	return Math.round(cm / CM_PER_FOOT);
}

export function formatFeet(cm) {
	return `${cmToFeet(cm).toLocaleString()} ft`;
}

export function formatMiles(cm) {
	return `${cmToMiles(cm)} mi`;
}

export function formatMilesInput(cm) {
	return (cm / CM_PER_MILE).toFixed(2);
}

export function parseMilesInput(rawMiles, minCm, maxCm) {
	const miles = Number.parseFloat(rawMiles);
	if (!Number.isFinite(miles)) {
		return null;
	}

	const rangeCm = Math.round(miles * CM_PER_MILE);
	return Math.max(minCm, Math.min(rangeCm, maxCm));
}
