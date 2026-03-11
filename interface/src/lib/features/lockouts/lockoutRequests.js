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

function areaStructuralFingerprint(area) {
	return [
		area?.lat ?? area?.latitude ?? '',
		area?.lon ?? area?.longitude ?? '',
		area?.rad ?? area?.radiusE5 ?? ''
	].join(',');
}

function signatureStructuralFingerprint(area, signature) {
	return [
		areaStructuralFingerprint(area),
		signature?.band ?? signature?.bandMask ?? '',
		signature?.freq ?? signature?.frequencyMHz ?? 0,
		signature?.ftol ?? signature?.frequencyToleranceMHz ?? 0,
		signature?.fmin ?? signature?.frequencyWindowMinMHz ?? signature?.freq ?? signature?.frequencyMHz ?? 0,
		signature?.fmax ?? signature?.frequencyWindowMaxMHz ?? signature?.freq ?? signature?.frequencyMHz ?? 0,
		normalizeDirectionIdentity(signature?.dir ?? signature?.directionMode),
		signature?.hdg ?? signature?.headingDeg ?? -1,
		signature?.htol ?? signature?.headingToleranceDeg ?? 45
	].join(',');
}

function cloneJsonValue(value) {
	return JSON.parse(JSON.stringify(value));
}

function countLockoutSignatures(doc) {
	if (Array.isArray(doc?.areas)) {
		return doc.areas.reduce(
			(total, area) => total + (Array.isArray(area?.signatures) ? area.signatures.length : 0),
			0
		);
	}
	if (Array.isArray(doc?.zones)) {
		return doc.zones.length;
	}
	return 0;
}

function normalizeV2ImportDoc(doc) {
	if (!doc || typeof doc !== 'object') {
		return null;
	}
	if (doc._type !== 'v1simple_lockout_zones' || Number(doc._version) !== 2 || !Array.isArray(doc.areas)) {
		return null;
	}

	const mergedAreas = [];
	const areaIndexByKey = new Map();
	const seenSignatures = new Set();

	for (const rawArea of doc.areas) {
		if (!rawArea || typeof rawArea !== 'object' || !Array.isArray(rawArea.signatures)) {
			return null;
		}
		const areaKey = areaStructuralFingerprint(rawArea);
		let areaIndex = areaIndexByKey.get(areaKey);
		if (areaIndex === undefined) {
			areaIndex = mergedAreas.length;
			areaIndexByKey.set(areaKey, areaIndex);
			mergedAreas.push({
				id: areaIndex + 1,
				lat: rawArea?.lat ?? rawArea?.latitude ?? null,
				lon: rawArea?.lon ?? rawArea?.longitude ?? null,
				rad: rawArea?.rad ?? rawArea?.radiusE5 ?? null,
				signatures: []
			});
		}

		for (const rawSignature of rawArea.signatures) {
			if (!rawSignature || typeof rawSignature !== 'object') {
				return null;
			}
			const signatureKey = signatureStructuralFingerprint(rawArea, rawSignature);
			if (seenSignatures.has(signatureKey)) {
				continue;
			}
			seenSignatures.add(signatureKey);
			mergedAreas[areaIndex].signatures.push(cloneJsonValue(rawSignature));
		}
	}

	return {
		_type: 'v1simple_lockout_zones',
		_version: 2,
		areas: mergedAreas
	};
}

function mergeV2ImportDocs(currentDoc, fileDoc) {
	const merged = normalizeV2ImportDoc(currentDoc);
	const incoming = normalizeV2ImportDoc(fileDoc);
	if (!merged || !incoming) {
		return null;
	}

	const areaIndexByKey = new Map();
	const seenSignatures = new Set();
	let existingCount = 0;

	for (let index = 0; index < merged.areas.length; ++index) {
		const area = merged.areas[index];
		const areaKey = areaStructuralFingerprint(area);
		areaIndexByKey.set(areaKey, index);
		for (const signature of area.signatures) {
			seenSignatures.add(signatureStructuralFingerprint(area, signature));
			++existingCount;
		}
	}

	let addedCount = 0;
	let skippedCount = 0;
	for (const incomingArea of incoming.areas) {
		const areaKey = areaStructuralFingerprint(incomingArea);
		let targetIndex = areaIndexByKey.get(areaKey);
		if (targetIndex === undefined) {
			targetIndex = merged.areas.length;
			areaIndexByKey.set(areaKey, targetIndex);
			merged.areas.push({
				id: targetIndex + 1,
				lat: incomingArea.lat,
				lon: incomingArea.lon,
				rad: incomingArea.rad,
				signatures: []
			});
		}
		const targetArea = merged.areas[targetIndex];
		for (const signature of incomingArea.signatures) {
			const signatureKey = signatureStructuralFingerprint(incomingArea, signature);
			if (seenSignatures.has(signatureKey)) {
				skippedCount++;
				continue;
			}
			seenSignatures.add(signatureKey);
			targetArea.signatures.push(cloneJsonValue(signature));
			addedCount++;
		}
	}

	for (let index = 0; index < merged.areas.length; ++index) {
		merged.areas[index].id = index + 1;
	}

	return {
		payload: merged,
		existingCount,
		addedCount,
		skippedCount
	};
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
		const signatureCount = countLockoutSignatures(parsed);
		if (signatureCount > 0) zoneCount = ` (${signatureCount} signatures)`;
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
	const normalizedImport = normalizeV2ImportDoc(parsed);
	if (!normalizedImport) {
		return {
			ok: false,
			error: 'Lockout import files must use the current v2 area export format.'
		};
	}

	const fileZoneCount = countLockoutSignatures(normalizedImport);
	const mergeChoice = confirmFn(
		`File contains ${fileZoneCount} signatures.\n\n` +
			`OK = MERGE (add to existing lockouts)\n` +
			`Cancel = go back`
	);
	if (!mergeChoice) {
		const replaceChoice = confirmFn(
			`Replace ALL current lockouts with ${fileZoneCount} signatures from ${file.name}?\n\n` +
				`This will delete all existing learned lockouts first.`
		);
		if (!replaceChoice) {
			return { ok: false, cancelled: true };
		}
		const replaceRes = await fetchWithTimeout('/api/lockouts/zones/import', {
			method: 'POST',
			headers: { 'Content-Type': 'application/json' },
			body: JSON.stringify(normalizedImport)
		});
		const replaceData = await replaceRes.json().catch(() => ({}));
		if (!replaceRes.ok) {
			return { ok: false, error: replaceData.message || `Failed to import lockout zones (${replaceRes.status})` };
		}
		const importedCount = typeof replaceData.entriesImported === 'number' ? replaceData.entriesImported : fileZoneCount;
		const warning =
			typeof replaceData.droppedManualCount === 'number' && replaceData.droppedManualCount > 0
				? `Dropped ${replaceData.droppedManualCount} legacy manual lockout ${
						replaceData.droppedManualCount === 1 ? 'entry' : 'entries'
					} during import.`
				: '';
		return {
			ok: true,
			message: warning
				? `Replaced with ${importedCount} lockout signatures. ${warning}`
				: `Replaced with ${importedCount} lockout signatures.`,
			droppedManualCount:
				typeof replaceData.droppedManualCount === 'number' ? replaceData.droppedManualCount : 0
		};
	}

	const exportRes = await fetchWithTimeout('/api/lockouts/zones/export');
	if (!exportRes.ok) {
		return { ok: false, error: `Failed to fetch current zones for merge (${exportRes.status})` };
	}
	const currentData = await exportRes.json().catch(() => ({}));
	const merged = mergeV2ImportDocs(currentData, normalizedImport);
	if (!merged) {
		return { ok: false, error: 'Failed to merge lockout zones from the exported v2 area format.' };
	}
	const mergeRes = await fetchWithTimeout('/api/lockouts/zones/import', {
		method: 'POST',
		headers: { 'Content-Type': 'application/json' },
		body: JSON.stringify(merged.payload)
	});
	const mergeData = await mergeRes.json().catch(() => ({}));
	if (!mergeRes.ok) {
		return { ok: false, error: mergeData.message || `Failed to merge lockout zones (${mergeRes.status})` };
	}
	const skippedMsg = merged.skippedCount > 0 ? ` (${merged.skippedCount} duplicates skipped)` : '';
	const warning =
		typeof mergeData.droppedManualCount === 'number' && mergeData.droppedManualCount > 0
			? ` Dropped ${mergeData.droppedManualCount} legacy manual lockout ${
					mergeData.droppedManualCount === 1 ? 'entry' : 'entries'
				} during import.`
			: '';
	return {
		ok: true,
		message: `Merged ${merged.addedCount} new signatures into ${merged.existingCount} existing${skippedMsg}.${warning}`,
		droppedManualCount: typeof mergeData.droppedManualCount === 'number' ? mergeData.droppedManualCount : 0
	};
}

export async function clearAllZonesRequest(fetchWithTimeout, pendingCount) {
	const empty = JSON.stringify({ _type: 'v1simple_lockout_zones', _version: 2, areas: [] });
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
