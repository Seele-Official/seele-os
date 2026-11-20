import { spawn } from 'node:child_process';
import { mkdir } from 'node:fs/promises';
import { dirname } from 'node:path';

import cac from 'cac';

import { DISK_MARKER, DISK_SIZE, createDisk } from './lib/disk.ts';
import { buildPaths, type BuildPaths } from './lib/paths.ts';
import { assertArtifacts, qemuArgs, qemuBinary } from './lib/qemu.ts';

interface CommonOptions {
    readonly buildDir: unknown;
}

interface QemuOptions extends CommonOptions {
    readonly harts: unknown;
}

const cli = cac('seele-os');

cli.option('--build-dir <dir>', 'build directory, relative to the repository root', {
    default: 'build',
});

cli.command('disk', 'Create the 1 MiB virtio test disk in buildDir')
    .example('$ seele-os disk --build-dir build-debug')
    .action((options: CommonOptions) => createDiskImage(options));

cli.command('qemu', 'Boot kernel.elf from buildDir in QEMU')
    .option('--harts <n>', 'number of QEMU harts, a positive integer', { default: 1 })
    .example('$ seele-os qemu --harts 4 --build-dir build-debug')
    .action((options: QemuOptions) => runQemu(options));

cli.help();

try {
    const parsed = cli.parse(process.argv, { run: false });
    if (cli.matchedCommand) {
        await cli.runMatchedCommand();
    } else if (!parsed.options.help) {
        cli.outputHelp();
    }
} catch (error) {
    console.error(`[seele-os] ${error instanceof Error ? error.message : String(error)}`);
    process.exitCode = 1;
}

async function createDiskImage(options: CommonOptions): Promise<void> {
    const paths = resolveBuildPaths(options);

    await mkdir(dirname(paths.diskImage), { recursive: true });
    await createDisk(paths.diskImage);

    console.log(`[seele-os] disk: ${paths.diskImage}`);
    console.log(
        `[seele-os] size: ${DISK_SIZE} bytes, LBA 0: ${JSON.stringify(DISK_MARKER.trimEnd())}`,
    );
}

async function runQemu(options: QemuOptions): Promise<void> {
    const paths = resolveBuildPaths(options);
    const harts = requireHarts(options.harts);

    assertArtifacts(paths);

    console.log(`[seele-os] qemu: ${harts} hart(s), build: ${paths.buildDir}`);

    const child = spawn(qemuBinary(), qemuArgs({ harts, paths }), { stdio: 'inherit' });

    for (const signal of ['SIGINT', 'SIGTERM'] as const) {
        process.on(signal, () => {
            if (!child.killed) {
                child.kill(signal);
            }
        });
    }

    const exitCode = await new Promise<number>((resolve, reject) => {
        child.once('error', reject);
        child.once('exit', (code, signal) => {
            if (signal) {
                resolve(signal === 'SIGINT' ? 130 : 143);
            } else {
                resolve(code ?? 1);
            }
        });
    });

    process.exitCode = exitCode;
}

function resolveBuildPaths(options: CommonOptions): BuildPaths {
    const buildDir = options.buildDir;
    if (typeof buildDir !== 'string' || buildDir.trim() === '') {
        throw new Error('--build-dir must be a non-empty directory path');
    }
    return buildPaths(buildDir);
}

function requireHarts(value: unknown): number {
    if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < 1) {
        throw new Error(`--harts must be a positive integer, got '${String(value)}'`);
    }
    return value;
}
