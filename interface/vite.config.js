import { sveltekit } from '@sveltejs/kit/vite';
import { svelteTesting } from '@testing-library/svelte/vite';
import { defineConfig } from 'vitest/config';

export default defineConfig({
	plugins: [sveltekit(), process.env.VITEST ? svelteTesting() : null].filter(Boolean),
	build: {
		// Optimize for embedded systems
		minify: 'esbuild',
		cssMinify: true
	},
	test: {
		environment: 'jsdom',
		setupFiles: ['./src/test/setup.js'],
		include: ['src/**/*.{test,spec}.{js,ts}'],
		coverage: {
			provider: 'v8',
			reporter: ['text-summary', 'lcov', 'html'],
			reportsDirectory: './coverage',
			all: true,
			include: [
				'src/lib/utils/colors.js',
				'src/lib/utils/lockout.js',
				'src/lib/components/ToggleSetting.svelte',
				'src/routes/lockouts/+page.svelte',
				'src/routes/settings/+page.svelte',
				'src/routes/profiles/+page.svelte',
				'src/routes/cameras/+page.svelte',
				'src/routes/audio/+page.svelte',
				'src/routes/colors/+page.svelte',
				'src/lib/features/lockouts/LockoutsPage.svelte',
				'src/lib/features/settings/SettingsPage.svelte',
				'src/lib/features/profiles/ProfilesPage.svelte',
				'src/lib/features/cameras/CamerasPage.svelte'
			],
			thresholds: {
				lines: 60,
				branches: 40,
				functions: 60,
				statements: 60
			}
		}
	}
});
