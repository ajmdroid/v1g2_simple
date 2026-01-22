import { sveltekit } from '@sveltejs/kit/vite';
import { defineConfig } from 'vite';

export default defineConfig({
	plugins: [sveltekit()],
	build: {
		// Optimize for embedded systems
		minify: 'esbuild',
		cssMinify: true,
		rollupOptions: {
			output: {
				// Keep filenames short for LittleFS
				entryFileNames: 'js/[name].js',
				chunkFileNames: 'js/[name].js',
				assetFileNames: (assetInfo) => {
					if (assetInfo.name?.endsWith('.css')) {
						return 'css/[name][extname]';
					}
					return 'assets/[name][extname]';
				}
			}
		}
	}
});
