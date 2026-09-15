#!/usr/bin/env node

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const DIST_DIR = path.join(ROOT, 'dist');
const PAYLOAD_DIR = path.join(DIST_DIR, 'payload');
const ELECTRON_HEADERS = 'https://electronjs.org/headers';
const SOURCE_FILES = ['binding.gyp', 'gpu_metrics.cc'];
const TARGETS = [{ arch: 'x64', machine: 0x8664 }];
const ELECTRON_TARGET_PATTERN = /^\d+\.\d+\.\d+$/;
const PROVENANCE_VARIABLES = ['GITHUB_REPOSITORY', 'GITHUB_SHA', 'GITHUB_SERVER_URL', 'GITHUB_RUN_ID'];

function parseArgs(argv) {
    let electronTarget = null;
    for (const argument of argv) {
        const matched = /^--electron-target=(.+)$/.exec(argument);
        if (!matched) throw new Error(`unknown argument: ${argument}`);
        if (electronTarget !== null) throw new Error('--electron-target given more than once');
        electronTarget = matched[1];
    }
    if (electronTarget === null) {
        throw new Error('missing required argument: --electron-target=<x.y.z>');
    }
    if (!ELECTRON_TARGET_PATTERN.test(electronTarget)) {
        throw new Error(`invalid --electron-target: ${electronTarget} (expected x.y.z)`);
    }
    return { electronTarget };
}

function readProvenance(env, electronTarget) {
    const missing = PROVENANCE_VARIABLES.filter((name) => !env[name]);
    if (missing.length > 0) {
        throw new Error(
            `missing GitHub Actions environment (${missing.join(', ')}); ` +
                'a local build cannot produce a releasable manifest'
        );
    }
    if (!/^[0-9a-f]{40}$/.test(env.GITHUB_SHA)) {
        throw new Error(`GITHUB_SHA is not a 40-character commit: ${env.GITHUB_SHA}`);
    }
    return {
        repo: env.GITHUB_REPOSITORY,
        commit: env.GITHUB_SHA,
        releaseTag: `electron-${electronTarget}-${env.GITHUB_SHA.slice(0, 7)}`,
        runUrl: `${env.GITHUB_SERVER_URL}/${env.GITHUB_REPOSITORY}/actions/runs/${env.GITHUB_RUN_ID}`
    };
}

function assertPe(file, expectedMachine) {
    const binary = fs.readFileSync(file);
    if (binary.length < 64 || binary.readUInt16LE(0) !== 0x5a4d) throw new Error(`${file} is not a PE binary`);
    const peOffset = binary.readUInt32LE(0x3c);
    if (peOffset + 6 > binary.length || binary.toString('ascii', peOffset, peOffset + 4) !== 'PE\u0000\u0000') {
        throw new Error(`${file} has no valid PE header`);
    }
    const actual = binary.readUInt16LE(peOffset + 4);
    if (actual !== expectedMachine) {
        throw new Error(`PE machine mismatch: expected 0x${expectedMachine.toString(16)}, got 0x${actual.toString(16)}`);
    }
}

function sha256File(file) {
    return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
}

function sourceSha256() {
    const hash = crypto.createHash('sha256');
    for (const relativePath of SOURCE_FILES) {
        hash.update(relativePath + '\u0000');
        hash.update(fs.readFileSync(path.join(ROOT, relativePath)));
        hash.update('\u0000');
    }
    return hash.digest('hex');
}

function buildManifest(electronTarget, provenance, artifacts) {
    return {
        schemaVersion: 2,
        electronTarget,
        sourceSha256: sourceSha256(),
        sourceFiles: SOURCE_FILES,
        source: provenance,
        artifacts
    };
}

function compile(electronTarget, arch) {
    console.log(`[gpu-metrics] building win32-${arch} for Electron ${electronTarget}`);
    const result = spawnSync(
        process.execPath,
        [
            require.resolve('node-gyp/bin/node-gyp.js'),
            'rebuild',
            `--arch=${arch}`,
            `--target=${electronTarget}`,
            `--dist-url=${ELECTRON_HEADERS}`
        ],
        { cwd: ROOT, stdio: 'inherit', shell: false }
    );
    if (result.error) console.error(`[gpu-metrics] failed to launch node-gyp: ${result.error.message}`);
    if (result.status !== 0) process.exit(result.status ?? 1);
}

function main(argv, env) {
    const { electronTarget } = parseArgs(argv);
    if (process.platform !== 'win32') throw new Error('Windows is required to build the native addon');
    const provenance = readProvenance(env, electronTarget);

    fs.rmSync(DIST_DIR, { recursive: true, force: true });
    fs.mkdirSync(PAYLOAD_DIR, { recursive: true });

    const artifacts = {};
    for (const { arch, machine } of TARGETS) {
        compile(electronTarget, arch);
        const built = path.join(ROOT, 'build', 'Release', 'gpu-metrics.node');
        if (!fs.existsSync(built)) throw new Error(`expected output is missing: ${built}`);
        assertPe(built, machine);

        const relativePath = `win32-${arch}/gpu-metrics.node`;
        const output = path.join(PAYLOAD_DIR, relativePath);
        fs.mkdirSync(path.dirname(output), { recursive: true });
        fs.copyFileSync(built, output);

        artifacts[`win32-${arch}`] = {
            path: relativePath,
            sizeBytes: fs.statSync(output).size,
            sha256: sha256File(output),
            peMachine: `0x${machine.toString(16)}`
        };
        console.log(`[gpu-metrics] win32-${arch} ${artifacts[`win32-${arch}`].sha256}`);
    }

    const manifest = buildManifest(electronTarget, provenance, artifacts);
    fs.writeFileSync(path.join(PAYLOAD_DIR, 'manifest.json'), JSON.stringify(manifest, null, 4) + '\n');
    console.log(`[gpu-metrics] wrote manifest for ${manifest.source.releaseTag}`);
}

if (require.main === module) {
    try {
        main(process.argv.slice(2), process.env);
    } catch (error) {
        console.error(`[gpu-metrics] ${error.message}`);
        process.exit(1);
    }
}

module.exports = {
    parseArgs,
    readProvenance,
    assertPe,
    sha256File,
    sourceSha256,
    buildManifest,
    TARGETS,
    SOURCE_FILES,
    ROOT,
    DIST_DIR,
    PAYLOAD_DIR
};
