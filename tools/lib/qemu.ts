import { existsSync } from 'node:fs';

import type { BuildPaths } from './paths.ts';

export interface QemuCommand {
    readonly harts: number;
    readonly paths: BuildPaths;
}

/** Override with SEELE_QEMU=/path/to/qemu-system-riscv64 if needed. */
export function qemuBinary(): string {
    return process.env.SEELE_QEMU ?? 'qemu-system-riscv64';
}

export function qemuArgs({ harts, paths }: QemuCommand): string[] {
    const args = [
        `-machine virt -bios none -kernel ${paths.kernelElf} -m 128M -smp ${harts}`,
        `-nographic -display none -serial stdio -monitor none`,
        `-global virtio-mmio.force-legacy=false`,
        `-drive file=${paths.diskImage},if=none,format=raw,id=x0`,
        `-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0`,
    ];

    return args.flatMap((arg) => arg.split(' '));
}

export function assertArtifacts(paths: BuildPaths): void {
    if (!existsSync(paths.kernelElf)) {
        throw new Error(`missing ${paths.kernelElf}; build the kernel in ${paths.buildDir} first`);
    }
    if (!existsSync(paths.diskImage)) {
        throw new Error(`missing ${paths.diskImage}; create the disk in ${paths.buildDir} first`);
    }
}
