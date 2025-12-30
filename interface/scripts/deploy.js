/**
 * Deploy script - copies built SvelteKit files to ../data/ for LittleFS
 */
import { cpSync, rmSync, existsSync, mkdirSync, readdirSync, statSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const buildDir = join(__dirname, '..', 'build');
const dataDir = join(__dirname, '..', '..', 'data');

console.log('🚀 Deploying SvelteKit build to LittleFS data folder...');

// Check if build exists
if (!existsSync(buildDir)) {
    console.error('❌ Build folder not found! Run "npm run build" first.');
    process.exit(1);
}

// Clear data folder (except any non-web files we want to keep)
if (existsSync(dataDir)) {
    console.log('🧹 Clearing existing data folder...');
    rmSync(dataDir, { recursive: true });
}
mkdirSync(dataDir, { recursive: true });

// Copy build to data
console.log('📁 Copying build files...');
cpSync(buildDir, dataDir, { recursive: true });

// List deployed files with sizes
function listFiles(dir, prefix = '') {
    const files = readdirSync(dir);
    let totalSize = 0;
    
    for (const file of files) {
        const filePath = join(dir, file);
        const stat = statSync(filePath);
        
        if (stat.isDirectory()) {
            totalSize += listFiles(filePath, prefix + file + '/');
        } else {
            const size = stat.size;
            totalSize += size;
            const sizeStr = size > 1024 ? `${(size / 1024).toFixed(1)} KB` : `${size} B`;
            console.log(`   ${prefix}${file} (${sizeStr})`);
        }
    }
    return totalSize;
}

console.log('\n📄 Deployed files:');
const totalSize = listFiles(dataDir);
console.log(`\n✅ Total size: ${(totalSize / 1024).toFixed(1)} KB`);
console.log('\n💡 Next steps:');
console.log('   cd .. && pio run -t buildfs && pio run -t uploadfs');
