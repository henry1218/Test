const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { test } = require('node:test');

const build = require('./build');

const EXPECTED_SOURCE_SHA256 = 'edeb66ad8739fcae5c8c17b29d74b843de932ad60b9e4750205e175f92920cb0';

function makePeBuffer(machine) {
    const buffer = Buffer.alloc(128);
    buffer.writeUInt16LE(0x5a4d, 0);
    buffer.writeUInt32LE(64, 0x3c);
    buffer.write('PE\u0000\u0000', 64, 'ascii');
    buffer.writeUInt16LE(machine, 68);
    return buffer;
}

function writeTemp(buffer) {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'gpu-metrics-test-'));
    const file = path.join(directory, 'sample.node');
    fs.writeFileSync(file, buffer);
    return file;
}

function validEnv() {
    return {
        GITHUB_REPOSITORY: 'A5Labs-Prime/wptg-gpu-metrics-addon',
        GITHUB_SHA: '806d0be0000000000000000000000000000000ab',
        GITHUB_SERVER_URL: 'https://github.com',
        GITHUB_RUN_ID: '123456'
    };
}

test('sourceSha256 matches the fingerprint shipped from the client', () => {
    assert.equal(build.sourceSha256(), EXPECTED_SOURCE_SHA256);
});

test('sourceSha256 hashes binding.gyp before gpu_metrics.cc', () => {
    assert.deepEqual(build.SOURCE_FILES, ['binding.gyp', 'gpu_metrics.cc']);
});

test('assertPe accepts a well-formed x64 PE', () => {
    const file = writeTemp(makePeBuffer(0x8664));
    assert.doesNotThrow(() => build.assertPe(file, 0x8664));
});

test('assertPe rejects a file shorter than a PE header', () => {
    const file = writeTemp(Buffer.alloc(16));
    assert.throws(() => build.assertPe(file, 0x8664), /is not a PE binary/);
});

test('assertPe rejects a missing MZ signature', () => {
    const buffer = makePeBuffer(0x8664);
    buffer.writeUInt16LE(0x0000, 0);
    const file = writeTemp(buffer);
    assert.throws(() => build.assertPe(file, 0x8664), /is not a PE binary/);
});

test('assertPe rejects a missing PE signature', () => {
    const buffer = makePeBuffer(0x8664);
    buffer.write('XX\u0000\u0000', 64, 'ascii');
    const file = writeTemp(buffer);
    assert.throws(() => build.assertPe(file, 0x8664), /has no valid PE header/);
});

test('assertPe reports both expected and actual machine on mismatch', () => {
    const file = writeTemp(makePeBuffer(0x014c));
    assert.throws(() => build.assertPe(file, 0x8664), /expected 0x8664, got 0x14c/);
});

test('parseArgs accepts a well-formed target', () => {
    assert.deepEqual(build.parseArgs(['--electron-target=44.3.0']), { electronTarget: '44.3.0' });
});

test('parseArgs rejects a missing target', () => {
    assert.throws(() => build.parseArgs([]), /missing required argument/);
});

test('parseArgs rejects a two-component version', () => {
    assert.throws(() => build.parseArgs(['--electron-target=44.3']), /invalid --electron-target/);
});

test('parseArgs rejects a v-prefixed version', () => {
    assert.throws(() => build.parseArgs(['--electron-target=v44.3.0']), /invalid --electron-target/);
});

test('parseArgs rejects an unknown argument', () => {
    assert.throws(() => build.parseArgs(['--rebuild']), /unknown argument/);
});

test('parseArgs rejects a repeated target', () => {
    assert.throws(
        () => build.parseArgs(['--electron-target=44.3.0', '--electron-target=44.4.0']),
        /given more than once/
    );
});

test('readProvenance builds the release tag from the target and short commit', () => {
    const provenance = build.readProvenance(validEnv(), '44.3.0');
    assert.equal(provenance.releaseTag, 'electron-44.3.0-806d0be');
    assert.equal(provenance.repo, 'A5Labs-Prime/wptg-gpu-metrics-addon');
    assert.equal(provenance.commit, '806d0be0000000000000000000000000000000ab');
    assert.equal(
        provenance.runUrl,
        'https://github.com/A5Labs-Prime/wptg-gpu-metrics-addon/actions/runs/123456'
    );
});

test('readProvenance rejects a missing environment variable', () => {
    const env = validEnv();
    delete env.GITHUB_RUN_ID;
    assert.throws(() => build.readProvenance(env, '44.3.0'), /GITHUB_RUN_ID/);
});

test('readProvenance rejects a short commit', () => {
    const env = validEnv();
    env.GITHUB_SHA = '806d0be';
    assert.throws(() => build.readProvenance(env, '44.3.0'), /not a 40-character commit/);
});

test('buildManifest emits schemaVersion 2 without packageVersion', () => {
    const artifacts = {
        'win32-x64': {
            path: 'win32-x64/gpu-metrics.node',
            sizeBytes: 308224,
            sha256: 'a'.repeat(64),
            peMachine: '0x8664'
        }
    };
    const manifest = build.buildManifest('44.3.0', build.readProvenance(validEnv(), '44.3.0'), artifacts);
    assert.equal(manifest.schemaVersion, 2);
    assert.equal(manifest.electronTarget, '44.3.0');
    assert.equal(manifest.sourceSha256, EXPECTED_SOURCE_SHA256);
    assert.deepEqual(manifest.sourceFiles, ['binding.gyp', 'gpu_metrics.cc']);
    assert.equal(manifest.source.releaseTag, 'electron-44.3.0-806d0be');
    assert.deepEqual(manifest.artifacts, artifacts);
    assert.equal('packageVersion' in manifest, false);
});

test('the CLI refuses to run off Windows and leaves dist untouched', { skip: process.platform === 'win32' }, () => {
    const script = path.join(__dirname, 'build.js');
    const result = spawnSync(process.execPath, [script, '--electron-target=44.3.0'], {
        cwd: build.ROOT,
        env: { ...process.env, ...validEnv() },
        encoding: 'utf8'
    });
    assert.equal(result.status, 1);
    assert.match(result.stderr, /Windows is required/);
    assert.equal(fs.existsSync(build.DIST_DIR), false);
});
