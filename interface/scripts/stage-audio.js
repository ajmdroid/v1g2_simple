import { stageAudioFiles } from './audio-manifest.js';

const result = stageAudioFiles();

if (result.missing.length > 0) {
	console.error('Missing audio assets required by manifest:');
	for (const file of result.missing) {
		console.error(`  - ${file}`);
	}
	process.exit(1);
}

console.log(
	`Staged ${result.copied}/${result.expected} manifest-tracked audio clips to ${result.targetDir}`
);
