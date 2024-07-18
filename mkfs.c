#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "wfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdint.h>

int main(int argc, char *argv[])
{
    char *diskImagePath;
    int inodeCount;
    int blockCount;
    int option;

    // Parse command line arguments
    while ((option = getopt(argc, argv, "d:i:b:")) != -1)
    {
        switch (option)
        {
        case 'd':
            diskImagePath = optarg;
            break;
        case 'i':
            inodeCount = atoi(optarg);
            break;
        case 'b':
            blockCount = atoi(optarg);
            break;
        default:
            fprintf(stderr, "Usage: %s -d <disk_img> -i <num_inodes> -b <num_blocks>\n", argv[0]);
            return 1;
        }
    }

    int fileDescriptor;
    struct stat fileStatus;
    struct wfs_sb fileSystemSuperblock;

    // Open the disk image file
    if ((fileDescriptor = open(diskImagePath, O_RDWR, S_IRWXU)) < 0)
    {
        perror("open");
        printf("err fd: %d\n", fileDescriptor);
        return 1;
    }

    // Get file status
    if (fstat(fileDescriptor, &fileStatus) < 0)
    {
        perror("fstat");
        close(fileDescriptor);
        printf("err fstat\n");
        return 1;
    }

    // Map the file into memory
    void *mappedFileMemory = mmap(0, fileStatus.st_size, PROT_WRITE, MAP_SHARED, fileDescriptor, 0);
    if (mappedFileMemory == MAP_FAILED)
    {
        perror("mmap");
        close(fileDescriptor);
        return 1;
    }

    // Adjust the number of inodes and blocks to be a multiple of 32
    inodeCount = (inodeCount + 31) & ~31;
    blockCount = (blockCount + 31) & ~31;

    // Set up the superblock
    fileSystemSuperblock.num_inodes = inodeCount;
    fileSystemSuperblock.num_data_blocks = blockCount;
    fileSystemSuperblock.i_bitmap_ptr = sizeof(struct wfs_sb);
    fileSystemSuperblock.d_bitmap_ptr = fileSystemSuperblock.i_bitmap_ptr + (inodeCount / 8);
    fileSystemSuperblock.i_blocks_ptr = fileSystemSuperblock.d_bitmap_ptr + (blockCount / 8);
    fileSystemSuperblock.d_blocks_ptr = fileSystemSuperblock.i_blocks_ptr + (inodeCount * BLOCK_SIZE);

    // Write the superblock to the disk image
    memcpy(mappedFileMemory, &fileSystemSuperblock, sizeof(struct wfs_sb));

    struct wfs_inode rootDirectoryInode;
    // Initialize the root directory inode
    memset(&rootDirectoryInode, 0, sizeof(struct wfs_inode));
    rootDirectoryInode.mode = S_IFDIR | S_IWUSR | S_IRUSR | S_IXUSR;
    rootDirectoryInode.uid = getuid();
    rootDirectoryInode.gid = getgid();
    rootDirectoryInode.size = 0;
    rootDirectoryInode.nlinks = 1;

    uint32_t bit = 0x1;
    memcpy((char *)mappedFileMemory + fileSystemSuperblock.i_bitmap_ptr, &bit, sizeof(uint32_t));
    memcpy((char *)mappedFileMemory + fileSystemSuperblock.i_blocks_ptr, &rootDirectoryInode, sizeof(struct wfs_inode));

    // Unmap the memory and close the file descriptor
    munmap(mappedFileMemory, fileStatus.st_size);
    close(fileDescriptor);
    return 0;
}