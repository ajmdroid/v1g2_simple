import {
	clampHdopX10,
	clampIntervalHours,
	clampLearnerFreqToleranceMHz,
	clampLearnerPromotionHits,
	clampManualDemotionMissCount,
	clampMinLearnerSpeed,
	clampU16,
	clampUnlearnCount,
	feetToRadiusE5,
	radiusE5ToFeet
} from '$lib/utils/lockout';

import { buildZoneEditorPayload } from './lockoutValidation.js';

function extractResponseMessage(data, fallback) {
	if (!data || typeof data !== 'object') {
		return fallback;
	}
	return data.message || data.error || fallback;
}

function normalizeDirectionIdentity(value) {
	if (value === 1 || value === '1' || value === 'forward') return 1;
	if (value === 2 || value === '2' || value === 'reverse') return 2;
	return 0;
}

function zoneStructuralFingerprint(zone) {
	return [
		zone?.lat ?? zone?.latitude ?? '',
		zone?.lon ?? zone?.longitude ?? '',
		zone?.rad ?? zone?.radiusE5 ?? '',
		zone?.band ?? zone?.bandMask ?? '',
		zone?.freq ?? zone?.frequencyMHz ?? 0,
		zone?.ftol ?? zone?.frequencyToleranceMHz ?? 0,
		normalizeDirectionIdentity(zone?.dir ?? zone?.directionMode),
		zone?.hdg ?? zone?.headingDeg ?? -1,
		zone?.htol ?? zone?.headingToleranceDeg ?? 45
	].join(',');
}

async function readResponsePayload(res) {
	const contentType = res.headers.get('content-type') || '';
	if (contentType.includes('application/json')) {
		return await res.json().catch(() => ({}));
	}
	const text = await res.text().catch(() => '');
	return text ? { message: text } : {};
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
	let payload;
	try {
		payload = await file.text();
	} catch {
		return { ok: false, error: 'Failed to read lockout zones file.' };
	}
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
	const existingKeys = new Set(currentZones.map((zone) => zoneStructuralFingerprint(zone)));
	let addedCount = 0;
	for (const zone of fileZones) {
		const key = zoneStructuralFingerprint(zone);
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
