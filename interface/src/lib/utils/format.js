export function formatBytes(bytes) {
	if (typeof bytes !== 'number' || !Number.isFinite(bytes) || bytes <= 0) return '0 B';
	if (bytes < 1024) return `${bytes} B`;
	if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
	return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

export function formatFrequencyMhz(mhz, options = {}) {
	const {
		allowGhz = false,
		ghzThresholdMhz = 1000,
		ghzDecimals = 3,
		mhzDecimals = 1,
		roundMhz = false
	} = options;

	if (typeof mhz !== 'number' || !Number.isFinite(mhz) || mhz <= 0) return '—';
	if (allowGhz && mhz >= ghzThresholdMhz) return `${(mhz / 1000).toFixed(ghzDecimals)} GHz`;
	if (roundMhz) return `${Math.round(mhz)} MHz`;
	return `${mhz.toFixed(mhzDecimals)} MHz`;
}
