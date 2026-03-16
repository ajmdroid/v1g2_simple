import { fetchWithTimeout } from '$lib/utils/poll';

export async function postSettingsForm(formData, endpoint = '/api/settings') {
	let response;
	const legacyFallbackPath = endpoint === '/api/settings' ? '/settings' : null;
	try {
		response = await fetchWithTimeout(endpoint, {
			method: 'POST',
			body: formData
		});
	} catch (error) {
		if (!legacyFallbackPath) {
			throw error;
		}
		return fetchWithTimeout(legacyFallbackPath, {
			method: 'POST',
			body: formData
		});
	}

	if (legacyFallbackPath && !response.ok && (response.status === 404 || response.status === 405)) {
		return fetchWithTimeout(legacyFallbackPath, {
			method: 'POST',
			body: formData
		});
	}

	return response;
}
