#!/usr/bin/env node

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const DIST_DIR = path.join(ROOT, 'dist');
const PAYLOAD_DIR = path.join(DIST_DIR, 'payload');
const ELECTRON_HEADERS = 'https://electronjs.org/headers';
const TARGETS = [{ arch: 'x64', machine: 0x8664 }];
const ELECTRON_TARGET_PATTERN = /^\d+\.\d+\.\d+$/;

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

function readLocalCommit() {
    const result = spawnSync('git', ['rev-parse', 'HEAD'], { cwd: ROOT, encoding: 'utf8' });
    if (result.status !== 0 || !result.stdout) {
        throw new Error('unable to resolve a commit: not running in GitHub Actions and not a git checkout');
    }
    return result.stdout.trim();
}

function readProvenance(env) {
    const commit = env.GITHUB_SHA || readLocalCommit();
    if (!/^[0-9a-f]{40}$/.test(commit)) {
        throw new Error(`commit is not a 40-character sha: ${commit}`);
    }
    return { commit };
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

function buildManifest(electronTarget, provenance, artifacts) {
    return {
        schemaVersion: 2,
        electronTarget,
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
    const provenance = readProvenance(env);

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
    console.log(`[gpu-metrics] wrote manifest for electron ${manifest.electronTarget} (${manifest.source.commit.slice(0, 7)})`);
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
    buildManifest,
    TARGETS,
    ROOT,
    DIST_DIR,
    PAYLOAD_DIR
};
