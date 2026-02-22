import { fetchWithTimeout } from '$lib/utils/poll';

export async function postSettingsForm(formData) {
	let response;
	try {
		response = await fetchWithTimeout('/api/settings', {
			method: 'POST',
			body: formData
		});
	} catch (error) {
		return fetchWithTimeout('/settings', {
			method: 'POST',
			body: formData
		});
	}

	if (!response.ok && (response.status === 404 || response.status === 405)) {
		return fetchWithTimeout('/settings', {
			method: 'POST',
			body: formData
		});
	}

	return response;
}
