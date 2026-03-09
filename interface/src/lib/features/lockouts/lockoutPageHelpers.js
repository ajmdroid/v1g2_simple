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

function extractResponseMessage(data, fallback) {
	if (!data || typeof data !== 'object') {
		return fallback;
	}
	return data.message || data.error || fallback;
}

async function readResponsePayload(res) {
	const contentType = res.headers.get('content-type') || '';
	if (contentType.includes('application/json')) {
		return await res.json().catch(() => ({}));
	}
	const text = await res.text().catch(() => '');
	return text ? { message: text } : {};
}

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

export function lockoutConfigMatchesRuntime(lockoutConfig, runtime) {
	if (!runtime) return false;
	const runtimeRadiusE5 = clampLearnerRadiusE5(runtime.learnerRadiusE5);
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

export async function saveLockoutConfigRequest(fetchWithTimeout, lockoutConfig) {
	const modeRaw = Math.max(0, Math.min(3, Number(lockoutConfig.modeRaw) || 0));
	const learnerPromotionHits = clampLearnerPromotionHits(lockoutConfig.learnerPromotionHits);
	const learnerRadiusE5 = feetToRadiusE5(lockoutConfig.learnerRadiusFt);
	const learnerFreqToleranceMHz = clampLearnerFreqToleranceMHz(lockoutConfig.learnerFreqToleranceMHz);
	const learnerLearnIntervalHours = clampIntervalHours(lockoutConfig.learnerLearnIntervalHours);
	const learnerUnlearnIntervalHours = clampIntervalHours(lockoutConfig.learnerUnlearnIntervalHours);
	const learnerUnlearnCount = clampUnlearnCount(lockoutConfig.learnerUnlearnCount);
	const manualDemotionMissCount = clampManualDemotionMissCount(lockoutConfig.manualDemotionMissCount);
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

	const data = await readResponsePayload(res);
	if (!res.ok) {
		return {
			ok: false,
			error: extractResponseMessage(data, `Failed to update lockout settings (${res.status})`)
		};
	}

	return {
		ok: true,
		normalized: {
			learnerPromotionHits,
			learnerRadiusFt: radiusE5ToFeet(learnerRadiusE5),
			learnerFreqToleranceMHz,
			learnerLearnIntervalHours,
			learnerUnlearnIntervalHours,
			learnerUnlearnCount,
			manualDemotionMissCount,
			kaLearningEnabled,
			preQuiet: !!lockoutConfig.preQuiet,
			maxHdopX10,
			minLearnerSpeedMph
		}
	};
}

export async function saveZoneEditorRequest(fetchWithTimeout, zoneEditor, zoneEditorSlot) {
	const { payload, error } = buildZoneEditorPayload(zoneEditor);
	if (error) {
		return { ok: false, error };
	}

	const creating = zoneEditorSlot === null;
	const requestPayload = creating ? payload : { slot: zoneEditorSlot, ...payload };
	const res = await fetchWithTimeout(creating ? '/api/lockouts/zones/create' : '/api/lockouts/zones/update', {
		method: 'POST',
		headers: { 'Content-Type': 'application/json' },
		body: JSON.stringify(requestPayload)
	});
	const data = await res.json().catch(() => ({}));
	if (!res.ok) {
		return {
			ok: false,
			error: data.message || `Failed to ${creating ? 'create' : 'update'} lockout zone (${res.status})`
		};
	}

	const slotText = typeof data.slot === 'number' ? ` ${data.slot}` : zoneEditorSlot === null ? '' : ` ${zoneEditorSlot}`;
	return { ok: true, message: `${creating ? 'Created' : 'Updated'} lockout zone${slotText}` };
}

export async function deleteZoneRequest(fetchWithTimeout, zone, label) {
	const slot = Number(zone?.slot);
	if (!Number.isInteger(slot) || slot < 0) {
		return { ok: false, error: 'Invalid zone slot.' };
	}

	const res = await fetchWithTimeout('/api/lockouts/zones/delete', {
		method: 'POST',
		headers: { 'Content-Type': 'application/json' },
		body: JSON.stringify({ slot })
	});
	const data = await res.json().catch(() => ({}));
	if (!res.ok) {
		return { ok: false, error: data.message || 'Failed to delete lockout zone' };
	}

	return {
		ok: true,
		slot,
		message: `Deleted ${label(zone)} lockout zone ${slot}`
	};
}

export async function exportLockoutZonesRequest(fetchWithTimeout) {
	const res = await fetchWithTimeout('/api/lockouts/zones/export');
	const payload = await res.text().catch(() => '');
	if (!res.ok || !payload) {
		return { ok: false, error: `Failed to export lockout zones (${res.status})` };
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

	return { ok: true, message: `Exported lockout zones${zoneCount}.` };
}

export async function importLockoutZonesFromFile(fetchWithTimeout, file, confirmFn) {
	const payload = await file.text();
	let parsed;
	try {
		parsed = JSON.parse(payload);
	} catch {
		return { ok: false, error: 'Invalid JSON file.' };
	}

	const fileZoneCount = Array.isArray(parsed?.zones) ? parsed.zones.length : '?';
	const mergeChoice = confirmFn(
		`File contains ${fileZoneCount} zones.\n\n` +
			`OK = MERGE (add to existing zones)\n` +
			`Cancel = go back`
	);
	if (!mergeChoice) {
		const replaceChoice = confirmFn(
			`Replace ALL current zones with ${fileZoneCount} zones from ${file.name}?\n\n` +
				`This will delete all existing zones first.`
		);
		if (!replaceChoice) {
			return { ok: false, cancelled: true };
		}
		const replaceRes = await fetchWithTimeout('/api/lockouts/zones/import', {
			method: 'POST',
			headers: { 'Content-Type': 'application/json' },
			body: payload
		});
		const replaceData = await replaceRes.json().catch(() => ({}));
		if (!replaceRes.ok) {
			return { ok: false, error: replaceData.message || `Failed to import lockout zones (${replaceRes.status})` };
		}
		const importedCount = typeof replaceData.entriesImported === 'number' ? replaceData.entriesImported : fileZoneCount;
		return { ok: true, message: `Replaced with ${importedCount} lockout zones.` };
	}

	const exportRes = await fetchWithTimeout('/api/lockouts/zones/export');
	if (!exportRes.ok) {
		return { ok: false, error: `Failed to fetch current zones for merge (${exportRes.status})` };
	}
	const currentData = await exportRes.json().catch(() => ({}));
	const currentZones = Array.isArray(currentData?.zones) ? currentData.zones : [];
	const fileZones = Array.isArray(parsed?.zones) ? parsed.zones : [];
	const existingKeys = new Set(currentZones.map((zone) => `${zone.lat},${zone.lon},${zone.band}`));
	let addedCount = 0;
	for (const zone of fileZones) {
		const key = `${zone.lat},${zone.lon},${zone.band}`;
		if (!existingKeys.has(key)) {
			currentZones.push(zone);
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
		return { ok: false, error: mergeData.message || `Failed to merge lockout zones (${mergeRes.status})` };
	}
	const skipped = fileZones.length - addedCount;
	const skippedMsg = skipped > 0 ? ` (${skipped} duplicates skipped)` : '';
	return {
		ok: true,
		message: `Merged ${addedCount} new zones into ${currentZones.length - addedCount} existing${skippedMsg}.`
	};
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

export async function clearAllZonesRequest(fetchWithTimeout, pendingCount) {
	const empty = JSON.stringify({ _type: 'v1simple_lockout_zones', _version: 1, zones: [] });
	const res = await fetchWithTimeout('/api/lockouts/zones/import', {
		method: 'POST',
		headers: { 'Content-Type': 'application/json' },
		body: empty
	});
	if (!res.ok) {
		const data = await res.json().catch(() => ({}));
		return { ok: false, error: data.message || `Failed to clear zones (${res.status})` };
	}

	if (pendingCount > 0) {
		try {
			await fetchWithTimeout('/api/lockouts/pending/clear', { method: 'POST' });
		} catch {
			// Best effort.
		}
	}

	return { ok: true };
}

export function resetZoneEditorState() {
	return defaultZoneEditorState();
}
