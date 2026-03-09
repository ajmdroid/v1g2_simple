export const RANGE_MIN_CM = 16093;
export const RANGE_MAX_CM = 160934;
export const NEAR_RANGE_MIN_CM = 8047;

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

export function formatType(type) {
	switch (type) {
		case 'speed':
			return 'Speed';
		case 'red_light':
			return 'Red Light';
		case 'bus_lane':
			return 'Bus Lane';
		case 'alpr':
			return 'ALPR';
		default:
			return 'None';
	}
}

export function rgb565ToHex(rgb565) {
	const val = typeof rgb565 === 'number' ? rgb565 : 0;
	const r = ((val >> 11) & 0x1f) << 3;
	const g = ((val >> 5) & 0x3f) << 2;
	const b = (val & 0x1f) << 3;
	return `#${[r, g, b].map((x) => x.toString(16).padStart(2, '0')).join('')}`;
}

export function rgb565ToHexStr(rgb565) {
	const val = typeof rgb565 === 'number' ? rgb565 : 0;
	return val.toString(16).toUpperCase().padStart(4, '0');
}

export function parseColorInput(input) {
	let clean = input.trim().toUpperCase();

	if (clean.startsWith('0X')) clean = clean.slice(2);
	if (clean.startsWith('#')) clean = clean.slice(1);
	if (!/^[0-9A-F]+$/.test(clean)) return null;

	if (clean.length <= 5) {
		const value = parseInt(clean, 16);
		return value <= 0xffff ? value : null;
	}

	if (clean.length === 6) {
		const r = parseInt(clean.slice(0, 2), 16) >> 3;
		const g = parseInt(clean.slice(2, 4), 16) >> 2;
		const b = parseInt(clean.slice(4, 6), 16) >> 3;
		return (r << 11) | (g << 5) | b;
	}

	return null;
}

export function rgb565ToChannels(rgb565) {
	const value = typeof rgb565 === 'number' ? rgb565 : 0;
	return {
		r: ((value >> 11) & 0x1f) << 3,
		g: ((value >> 5) & 0x3f) << 2,
		b: (value & 0x1f) << 3
	};
}
