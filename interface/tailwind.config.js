/** @type {import('tailwindcss').Config} */
export default {
	content: ['./src/**/*.{html,js,svelte,ts}'],
	theme: {
		extend: {}
	},
	plugins: [require('daisyui')],
	daisyui: {
		themes: [
			{
				v1dark: {
					'primary': '#00d4ff',
					'secondary': '#ff6b35',
					'accent': '#00ff88',
					'neutral': '#1a1a2e',
					'base-100': '#0f0f1a',
					'base-200': '#16213e',
					'base-300': '#1a1a2e',
					'info': '#00d4ff',
					'success': '#00ff88',
					'warning': '#ffcc00',
					'error': '#ff4444'
				}
			},
			'light'
		],
		darkTheme: 'v1dark'
	}
};
