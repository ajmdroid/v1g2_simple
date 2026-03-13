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
			include: ['src/**/*.{js,ts,svelte}'],
			exclude: [
				'src/**/*.test.{js,ts}',
				'src/**/*.spec.{js,ts}',
				'src/test/**'
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
