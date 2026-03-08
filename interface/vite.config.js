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
				'src/lib/components/ToggleSetting.svelte'
			]
		}
	}
});
