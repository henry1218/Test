const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { test } = require('node:test');

const build = require('./build');

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
        GITHUB_SHA: '806d0be0000000000000000000000000000000ab'
    };
}

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

test('readProvenance returns the GITHUB_SHA commit as-is', () => {
    const provenance = build.readProvenance(validEnv());
    assert.equal(provenance.commit, '806d0be0000000000000000000000000000000ab');
});

test('readProvenance falls back to the local git commit when GITHUB_SHA is absent', () => {
    const env = validEnv();
    delete env.GITHUB_SHA;
    const provenance = build.readProvenance(env);
    assert.match(provenance.commit, /^[0-9a-f]{40}$/);
});

test('readProvenance rejects a short commit', () => {
    const env = validEnv();
    env.GITHUB_SHA = '806d0be';
    assert.throws(() => build.readProvenance(env), /not a 40-character sha/);
});

test('buildManifest emits schemaVersion 2 without a source fingerprint', () => {
    const artifacts = {
        'win32-x64': {
            path: 'win32-x64/gpu-metrics.node',
            sizeBytes: 308224,
            sha256: 'a'.repeat(64),
            peMachine: '0x8664'
        }
    };
    const manifest = build.buildManifest('44.3.0', build.readProvenance(validEnv()), artifacts);
    assert.equal(manifest.schemaVersion, 2);
    assert.equal(manifest.electronTarget, '44.3.0');
    assert.equal(manifest.source.commit, '806d0be0000000000000000000000000000000ab');
    assert.deepEqual(manifest.artifacts, artifacts);
    assert.equal('sourceSha256' in manifest, false);
    assert.equal('sourceFiles' in manifest, false);
    assert.equal('releaseTag' in manifest.source, false);
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
