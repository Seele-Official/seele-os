import { spawn } from 'node:child_process';
import { mkdir } from 'node:fs/promises';
import { constants as osConstants } from 'node:os';
import { dirname } from 'node:path';

import cac from 'cac';

import { DISK_MARKER, DISK_SIZE, createDisk } from './lib/disk.ts';
import { buildPaths, type BuildPaths } from './lib/paths.ts';
import { assertArtifacts, qemuArgs, qemuBinary } from './lib/qemu.ts';

interface CommonOptions {
    readonly buildDir?: unknown;
}

interface QemuOptions extends CommonOptions {
    readonly harts: unknown;
    readonly append?: unknown;
}

interface TestOptions extends CommonOptions {
    readonly harts: unknown;
    readonly filter: unknown;
    readonly timeout: unknown;
}

const cli = cac('seele-os');

cli.option('--build-dir <dir>', 'build directory, relative to the repository root');

cli.command('disk', 'Create the 1 MiB virtio test disk in buildDir')
    .example('$ seele-os disk --build-dir build-debug')
    .action((options: CommonOptions) => createDiskImage(options));

cli.command('qemu', 'Boot kernel.elf from buildDir in QEMU')
    .option('--harts <n>', 'number of QEMU harts, a positive integer', { default: 1 })
    .option('--append <cmdline>', 'optional kernel command line (/chosen/bootargs)')
    .example('$ seele-os qemu --harts 4 --build-dir build-debug')
    .action((options: QemuOptions) => runQemu(options));

cli.command('test', 'Boot a test-enabled kernel.elf in QEMU and check its exit status')
    .option('--harts <n>', 'number of QEMU harts, a positive integer', { default: 4 })
    .option('--filter <suite>', 'suite or suite.case filter (or "all")', { default: 'all' })
    .option('--timeout <ms>', 'watchdog timeout in milliseconds', { default: 30000 })
    .example('$ seele-os test --harts 4 --filter mm --build-dir build-test')
    .action((options: TestOptions) => runTests(options));

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
    const paths = resolveBuildPaths(options, 'build');

    await mkdir(dirname(paths.diskImage), { recursive: true });
    await createDisk(paths.diskImage);

    console.log(`[seele-os] disk: ${paths.diskImage}`);
    console.log(
        `[seele-os] size: ${DISK_SIZE} bytes, LBA 0: ${JSON.stringify(DISK_MARKER.trimEnd())}`,
    );
}

async function runQemu(options: QemuOptions): Promise<void> {
    const paths = resolveBuildPaths(options, 'build');
    const harts = requirePositiveInteger('--harts', options.harts);
    const append = optionalString('--append', options.append);

    assertArtifacts(paths);

    console.log(`[seele-os] qemu: ${harts} hart(s), build: ${paths.buildDir}`);

    process.exitCode = await spawnQemu({ harts, paths, append });
}

async function runTests(options: TestOptions): Promise<void> {
    const paths = resolveBuildPaths(options, 'build-test');
    const harts = requirePositiveInteger('--harts', options.harts);
    const timeoutMs = requirePositiveInteger('--timeout', options.timeout);
    const filter = requireNonEmptyString('--filter', options.filter);

    assertArtifacts(paths);

    const append = `test=${filter}`;
    console.log(
        `[seele-os] test: ${harts} hart(s), filter: ${filter}, timeout: ${timeoutMs} ms, build: ${paths.buildDir}`,
    );

    process.exitCode = await spawnQemu({ harts, paths, append }, timeoutMs);
}

async function spawnQemu(
    command: { readonly harts: number; readonly paths: BuildPaths; readonly append?: string },
    timeoutMs?: number,
): Promise<number> {
    const child = spawn(qemuBinary(), qemuArgs(command), { stdio: 'inherit' });

    const forwardSignal = (signal: NodeJS.Signals) => {
        if (!child.killed) {
            child.kill(signal);
        }
    };
    process.on('SIGINT', forwardSignal);
    process.on('SIGTERM', forwardSignal);

    let timedOut = false;
    let killEscalation: NodeJS.Timeout | undefined;
    const timer =
        timeoutMs !== undefined
            ? setTimeout(() => {
                  timedOut = true;
                  console.error(
                      `\n[seele-os] test timed out after ${timeoutMs} ms; terminating QEMU (pid ${String(child.pid)})`,
                  );
                  child.kill('SIGTERM');
                  killEscalation = setTimeout(() => {
                      if (child.exitCode === null && child.signalCode === null) {
                          child.kill('SIGKILL');
                      }
                  }, 2000);
                  killEscalation.unref();
              }, timeoutMs)
            : undefined;

    try {
        const exitCode = await new Promise<number>((resolve, reject) => {
            child.once('error', reject);
            child.once('exit', (code, signal) => {
                if (timedOut) {
                    resolve(124);
                } else if (signal) {
                    const signum = osConstants.signals[signal];
                    resolve(signum ? 128 + signum : 1);
                } else {
                    resolve(code ?? 1);
                }
            });
        });
        return exitCode;
    } finally {
        if (timer !== undefined) {
            clearTimeout(timer);
        }
        if (killEscalation !== undefined) {
            clearTimeout(killEscalation);
        }
        process.off('SIGINT', forwardSignal);
        process.off('SIGTERM', forwardSignal);
    }
}

function resolveBuildPaths(options: CommonOptions, defaultDir: string): BuildPaths {
    const raw = options.buildDir ?? defaultDir;
    if (typeof raw !== 'string' || raw.trim() === '') {
        throw new Error('--build-dir must be a non-empty directory path');
    }
    return buildPaths(raw);
}

function requirePositiveInteger(flag: string, value: unknown): number {
    if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < 1) {
        throw new Error(`${flag} must be a positive integer, got '${String(value)}'`);
    }
    return value;
}

function requireNonEmptyString(flag: string, value: unknown): string {
    const text = typeof value === 'number' ? String(value) : value;
    if (typeof text !== 'string' || text.trim() === '' || text.includes(' ')) {
        throw new Error(`${flag} must be a non-empty token without spaces, got '${String(value)}'`);
    }
    return text;
}

function optionalString(flag: string, value: unknown): string | undefined {
    if (value === undefined) {
        return undefined;
    }
    if (typeof value !== 'string') {
        throw new Error(`${flag} must be a string, got '${String(value)}'`);
    }
    return value;
}
