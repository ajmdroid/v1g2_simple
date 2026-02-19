export async function postSettingsForm(formData) {
	let response;
	try {
		response = await fetch('/api/settings', {
			method: 'POST',
			body: formData
		});
	} catch (error) {
		return fetch('/settings', {
			method: 'POST',
			body: formData
		});
	}

	if (!response.ok && (response.status === 404 || response.status === 405)) {
		return fetch('/settings', {
			method: 'POST',
			body: formData
		});
	}

	return response;
}
