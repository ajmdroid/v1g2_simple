function pad2(value) {
	return String(value).padStart(2, '0');
}

function formatOffset(minutes) {
	const sign = minutes >= 0 ? '+' : '-';
	const absMinutes = Math.abs(minutes);
	const hours = Math.floor(absMinutes / 60);
	const mins = absMinutes % 60;
	return `${sign}${pad2(hours)}:${pad2(mins)}`;
}

export function getTimeSourceLabel(source) {
	switch (source) {
		case 1:
			return 'CLIENT_AP';
		case 3:
			return 'SNTP';
		case 4:
			return 'RTC';
		default:
			return 'NONE';
	}
}

export function getTimeConfidenceLabel(confidence) {
	switch (confidence) {
		case 2:
			return 'ACCURATE';
		case 1:
			return 'ESTIMATED';
		default:
			return 'NONE';
	}
}

export function projectEpochMs(timeStatus, clientNowMs) {
	if (!timeStatus.valid || !timeStatus.epochMs || !timeStatus.sampleClientMs) return 0;
	const deltaMs = Math.max(0, clientNowMs - timeStatus.sampleClientMs);
	return timeStatus.epochMs + deltaMs;
}

export function projectAgeMs(timeStatus, clientNowMs) {
	if (!timeStatus.valid || !timeStatus.sampleClientMs) return 0;
	const deltaMs = Math.max(0, clientNowMs - timeStatus.sampleClientMs);
	return (timeStatus.ageMs || 0) + deltaMs;
}

export function formatDeviceDateTime(timeStatus, clientNowMs) {
	const projectedEpochMs = projectEpochMs(timeStatus, clientNowMs);
	if (!projectedEpochMs) return '—';
	const tzOffsetMs = (timeStatus.tzOffsetMin || 0) * 60000;
	const date = new Date(projectedEpochMs + tzOffsetMs);
	return `${date.getUTCFullYear()}-${pad2(date.getUTCMonth() + 1)}-${pad2(date.getUTCDate())} ${pad2(date.getUTCHours())}:${pad2(date.getUTCMinutes())}:${pad2(date.getUTCSeconds())} (UTC${formatOffset(timeStatus.tzOffsetMin || 0)})`;
}

export function formatAgeMs(ms) {
	if (!ms || ms < 0) return '0s';
	const totalSeconds = Math.floor(ms / 1000);
	const hours = Math.floor(totalSeconds / 3600);
	const minutes = Math.floor((totalSeconds % 3600) / 60);
	const seconds = totalSeconds % 60;
	if (hours > 0) return `${hours}h ${minutes}m ${seconds}s`;
	if (minutes > 0) return `${minutes}m ${seconds}s`;
	return `${seconds}s`;
}
