import { open } from 'node:fs/promises';

export const DISK_SIZE = 1024 * 1024;
export const DISK_MARKER = 'HELLO-FROM-VIRTIO-DISK\n';

/**
 * Create the raw virtio-blk test disk.
 *
 * The image is exactly 1 MiB (2048 sectors). The marker lives at LBA 0 so the
 * kernel can prove that the virtio queue actually read from the disk.
 */
export async function createDisk(imagePath: string): Promise<void> {
    const file = await open(imagePath, 'w');
    try {
        await file.truncate(DISK_SIZE);
        await file.write(DISK_MARKER, 0, 'utf8');
    } finally {
        await file.close();
    }
}
