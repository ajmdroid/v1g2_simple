import { fetchWithTimeout } from '$lib/utils/poll';

export async function postSettingsForm(formData, endpoint = '/api/settings') {
	return fetchWithTimeout(endpoint, {
		method: 'POST',
		body: formData
	});
}
