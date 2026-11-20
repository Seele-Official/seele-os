import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const toolsDir = dirname(fileURLToPath(import.meta.url));

export const rootDir = resolve(toolsDir, '../..');

export interface BuildPaths {
    /** Absolute path of the CMake build directory. */
    readonly buildDir: string;
    readonly kernelElf: string;
    readonly diskImage: string;
}

/**
 * Resolve the artifacts a QEMU run needs from a build directory.
 *
 * `buildDir` is resolved against the repository root, so both relative
 * (`build-debug`) and absolute paths work regardless of the current directory.
 */
export function buildPaths(buildDir: string): BuildPaths {
    const directory = resolve(rootDir, buildDir);
    return {
        buildDir: directory,
        kernelElf: resolve(directory, 'kernel.elf'),
        diskImage: resolve(directory, 'disk.img'),
    };
}
