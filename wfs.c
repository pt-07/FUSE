#include "wfs.h"
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <libgen.h>
#include <assert.h>
#include <stdint.h>


/***********************************************************************************************
                                macros
***********************************************************************************************/
#define min(a, b) ((a) < (b) ? (a) : (b))

/***********************************************************************************************
                                global variables
***********************************************************************************************/
void *mappedMemoryRegion;
int errorCheck;

/***********************************************************************************************
                                function prototypes
***********************************************************************************************/
int getInode(struct wfs_inode *enclosing, char *path, struct wfs_inode **inode);
int getInodePath(char *path, struct wfs_inode **inode);
int wfs_mknod(const char *path, mode_t mode, dev_t dev);
int wfs_mkdir(const char *path, mode_t mode);
int wfs_getattr(const char *path, struct stat *stbuf);
char *find_doffset(struct wfs_inode *inode, off_t off, int flag);
int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi);
struct wfs_inode *retrieve_inode(int num);
ssize_t allocateBlock(uint32_t *bitmap, size_t size);
off_t allocate_DB();
struct wfs_inode *allocateInode();
int wfs_unlink(const char *path);
int wfs_rmdir(const char *path);
int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
void freeBitmap(uint32_t pos, uint32_t *bitmap);
void freeDblock(off_t block);
void free_inode(struct wfs_inode *inode);
int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
void populate_stat(struct stat *stbuf, struct wfs_inode *inode);
void update_inode_size(struct wfs_inode *inode, off_t offset, size_t size);
size_t calculate_bytes_to_write(size_t blockSize, size_t size, ssize_t bytesWritten);
char* mmap_ptr(off_t offset);
/***********************************************************************************************
                                FUSE OPERATIONS
***********************************************************************************************/
static struct fuse_operations ops = {
    .getattr = wfs_getattr,
    .mknod = wfs_mknod,
    .mkdir = wfs_mkdir,
    .unlink = wfs_unlink,
    .rmdir = wfs_rmdir,
    .read = wfs_read,
    .write = wfs_write,
    .readdir = wfs_readdir,
};

/***********************************************************************************************
                                FUSE OPERATIONS IMPLEMENTATIONS
***********************************************************************************************/

/**
 * Creates a new regular file in the WFS file system.
 *
 * @param path The path of the file to create.
 * @param mode The file mode (permissions).
 * @param dev The device ID (unused in this implementation).
 * @return 0 on success, or a negative error code on failure.
 */
int wfs_mknod(const char *path, mode_t mode, dev_t dev) {
    printf("wfs_mknod\n");
    (void)dev;  // Unused parameter

    struct wfs_inode *parent = NULL;
    char *dirPath = strdup(path);
    char *fileName = strdup(path);
    if (!dirPath || !fileName) {
        free(dirPath);
        free(fileName);
        return -ENOMEM;
    }

    // Retrieve the parent inode
    if (getInodePath(dirname(dirPath), &parent) < 0) {
        free(dirPath);
        free(fileName);
        return errorCheck;
    }

    // Allocate a new inode for the file
    struct wfs_inode *inode = allocateInode();
    if (inode == NULL) {
        free(dirPath);
        free(fileName);
        return -ENOSPC;  // No space left for a new inode
    }

    inode->mode = S_IFREG | mode;
    inode->uid = getuid();
    inode->gid = getgid();
    inode->size = 0;
    inode->nlinks = 1;

    // Adding a directory entry manually in the parent directory
    struct wfs_dentry *dentry;
    off_t off = 0;
    bool added = false;
    while (off < parent->size) {
        dentry = (struct wfs_dentry *)find_doffset(parent, off, 0);
        if (dentry == NULL) {
            break;
        }
        if (dentry->num == 0) {
            dentry->num = inode->num;
            strncpy(dentry->name, basename(fileName), MAX_NAME);
            parent->nlinks++;
            added = true;
            break;
        }
        off += sizeof(struct wfs_dentry);
    }

    // If no empty entry found, create a new one
    if (!added) {
        dentry = (struct wfs_dentry *)find_doffset(parent, parent->size, 1);
        if (dentry == NULL) {
            free_inode(inode);
            free(dirPath);
            free(fileName);
            return -ENOSPC;
        }
        dentry->num = inode->num;
        strncpy(dentry->name, basename(fileName), MAX_NAME);
        parent->nlinks++;
        parent->size += sizeof(struct wfs_dentry);
    }

    free(dirPath);
    free(fileName);
    return 0;
}

/**
 * Creates a new directory in the WFS file system.
 *
 * @param path The path of the directory to create.
 * @param mode The directory mode (permissions).
 * @return 0 on success, or a negative error code on failure.
 */
int wfs_mkdir(const char *path, mode_t mode) {
    printf("wfs_mkdir\n");
    struct wfs_inode *parent = NULL;
    char *dirPath = strdup(path);
    if (dirPath == NULL) {
        perror("Failed to allocate memory");
        return -ENOMEM;
    }
    char *fileName = strdup(path);
    if (fileName == NULL) {
        free(dirPath);
        perror("Failed to allocate memory");
        return -ENOMEM;
    }

    if (getInodePath(dirname(dirPath), &parent) < 0) {
        free(dirPath);
        free(fileName);
        return errorCheck;
    }

    struct wfs_inode *inode = allocateInode();
    if (inode == NULL) {
        free(dirPath);
        free(fileName);
        return -ENOSPC;
    }

    inode->mode = S_IFDIR | mode;
    inode->uid = getuid();
    inode->gid = getgid();
    inode->size = 0;
    inode->nlinks = 2;

    struct wfs_dentry *dentry;
    off_t off = 0;
    bool added = false;
    while (off < parent->size) {
        dentry = (struct wfs_dentry *)find_doffset(parent, off, 0);
        if (dentry == NULL) {
            break;
        }
        if (dentry->num == 0) {
            dentry->num = inode->num;
            strncpy(dentry->name, basename(fileName), MAX_NAME);
            parent->nlinks++;
            added = true;
            break;
        }
        off += sizeof(struct wfs_dentry);
    }

    if (!added) {
        dentry = (struct wfs_dentry *)find_doffset(parent, parent->size, 1);
        if (dentry == NULL) {
            free_inode(inode);
            free(dirPath);
            free(fileName);
            return -ENOSPC;
        }
        dentry->num = inode->num;
        strncpy(dentry->name, basename(fileName), MAX_NAME);
        parent->nlinks++;
        parent->size += sizeof(struct wfs_dentry);
    }

    free(dirPath);
    free(fileName);
    return 0;
}

/**
 * Retrieves the attributes of a file or directory in the WFS file system.
 *
 * @param path The path of the file or directory.
 * @param stbuf The stat structure to store the attributes.
 * @return 0 on success, or a negative error code on failure.
 */
int wfs_getattr(const char *path, struct stat *stbuf)
{
    printf("wfs_getattr\n");
    struct wfs_inode *inode;
    char *search = strdup(path);
    if (getInodePath(search, &inode) < 0)
    {
        free(search);
        return errorCheck;
    }
    populate_stat(stbuf, inode);
    free(search);
    return 0;
}
void populate_stat(struct stat *stbuf, struct wfs_inode *inode)
{
    stbuf->st_mode = inode->mode;
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_size = inode->size;
    stbuf->st_nlink = inode->nlinks;
}

/**
 * Reads data from a file in the WFS file system.
 *
 * @param path The path of the file to read.
 * @param buf The buffer to store the read data.
 * @param size The maximum number of bytes to read.
 * @param offset The offset within the file to start reading from.
 * @param fi File information structure (not used in this function).
 * @return The number of bytes read on success, or a negative error code on failure.
 */
    int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
    {
        printf("wfs_read\n");
        (void)fi;

        struct wfs_inode *inode;
        char *search = strdup(path);

        if (getInodePath(search, &inode) < 0)
        {
            free(search);
            return errorCheck;
        }

        size_t bytesRead = 0;
        size_t i;

        for (i = 0; bytesRead < size && offset + bytesRead < inode->size; i++)
        {
            size_t currentPosition = offset + bytesRead;
            size_t blockSize = BLOCK_SIZE - (currentPosition % BLOCK_SIZE);
            size_t remainingSize = inode->size - currentPosition;
            size_t toRead = blockSize > remainingSize ? remainingSize : blockSize;

            char *blockAddress = find_doffset(inode, currentPosition, 0);
            if(blockAddress == NULL) {
                return -ENOENT;
            }
            memcpy(buf + bytesRead, blockAddress, toRead);
            bytesRead += toRead;
        }

        free(search);
        return bytesRead;
    }

/**
 * Writes data to a file in the WFS filesystem.
 *
 * @param path The path of the file to write.
 * @param buf The buffer containing the data to write.
 * @param size The size of the data to write.
 * @param offset The offset at which to start writing in the file.
 * @param fi File information (unused in this implementation).
 * @return The number of bytes written, or an error code if an error occurred.
 */
int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    (void)fi;
    struct wfs_inode *inode;
    char *search = strdup(path);

    if (getInodePath(search, &inode) < 0)
    {
        free(search);
        return errorCheck;
    }

    ssize_t bytesWritten = 0;
    size_t currentPosition = offset;
    size_t bytesToWrite = 0;

    for (; bytesWritten < size; bytesWritten += bytesToWrite, currentPosition += bytesToWrite)
    {
        size_t blockSize = BLOCK_SIZE - (currentPosition % BLOCK_SIZE);
        bytesToWrite = min(blockSize, size - bytesWritten);

        char *blockAddress = find_doffset(inode, currentPosition, 1);
        if (blockAddress == NULL)
        {
            free(search);
            return -ENOSPC; // No space left or other block allocation error
        }

        memcpy(blockAddress, buf + bytesWritten, bytesToWrite);
    }

    update_inode_size(inode, offset, size);

    free(search);
    return bytesWritten;
}

// Helper function to calculate the number of bytes to write
size_t calculate_bytes_to_write(size_t blockSize, size_t size, ssize_t bytesWritten) {
    return min(blockSize, size - bytesWritten);
}

// Helper function to update the inode size
void update_inode_size(struct wfs_inode *inode, off_t offset, size_t size) {
    if (offset + size > inode->size) {
        inode->size = offset + size;
    }
}

/**
 * Reads the contents of a directory in the WFS file system.
 *
 * @param path The path of the directory to read.
 * @param buf The buffer to store the directory entries.
 * @param filler The filler function to add directory entries to the buffer.
 * @param offset The offset within the directory.
 * @param fi File information structure (not used in this function).
 * @return 0 on success, or a negative error code on failure.
 */
int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    printf("wfs_readdir\n");
    (void)fi;
    (void)offset;
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    struct wfs_inode *inode;
    char *search = strdup(path);
    if (getInodePath(search, &inode) < 0)
    {
        free(search);
        return errorCheck;
    }

    size_t size = inode->size;
    struct wfs_dentry *dent;
    off_t off = 0;
    while (off < size)
    {
        dent = (struct wfs_dentry *)find_doffset(inode, off, 0);

        if(dent == NULL) {
            continue;
        }
        if (dent->num != 0)
        {
            filler(buf, dent->name, NULL, 0);
        }
        off += sizeof(struct wfs_dentry);
    }
    free(search);
    return 0;
}

/**
 * Removes a file from the WFS file system.
 *
 * @param path The path of the file to remove.
* @return 0 on success, or a negative error code on failure.
 */
int wfs_unlink(const char *path)
{
    printf("wfs_unlink\n");
    struct wfs_inode *parent_inode;
    struct wfs_inode *inode;
    char *base = strdup(path);
    char *search = strdup(path);

    if (getInodePath(dirname(base), &parent_inode) < 0)
    {
        free(base);
        free(search);
        return errorCheck;
    }
    if (getInodePath(search, &inode) < 0)
    {
        free(base);
        free(search);
        return errorCheck;
    }
    size_t size = parent_inode->size;
    struct wfs_dentry *dent;

    off_t off = 0;
    while (off < size)
    {
        dent = (struct wfs_dentry *)find_doffset(parent_inode, off, 0);
        if(dent == NULL) {
            return -ENOENT;
        }
        if (dent->num == inode->num)
        {
            dent->num = 0;
            break;
        }
        off += sizeof(struct wfs_dentry);
    }

    // Free all data blocks associated with the inode
    int i = 0;
    while (i < D_BLOCK) {
        if(inode->blocks[i] != 0) {
            freeDblock(inode->blocks[i]);
            inode->blocks[i] = 0;  // Set the block pointer to 0 after freeing
        }
        i++;
    }
    // Free the inode after all associated data blocks have been freed
    free_inode(inode);
    free(base);
    free(search);
    return 0;
}

/**
 * Removes a directory from the WFS file system.
 *
 * @param path The path of the directory to remove.
 * @return 0 on success, or a negative error code on failure.
 */
int wfs_rmdir(const char *path)
{
    printf("wfs_rmdir\n");
    wfs_unlink(path);
    return 0;
}
/***********************************************************************************************
                                HELPER FUNCTIONS
***********************************************************************************************/
/**
 * Finds the data offset for a given inode, offset, and flag.
 *
 * @param inode The pointer to the wfs_inode structure.
 * @param off The offset value.
 * @param flag The flag value.
 * @return A pointer to the data offset.
 */

char * find_doffset(struct wfs_inode *inode, off_t offset, int flag)
{
    printf("data_offset\n");

    int block_index = offset / BLOCK_SIZE;
    off_t *blocks_array;

    if (block_index > D_BLOCK)
    {
        block_index -= IND_BLOCK;
        if (inode->blocks[IND_BLOCK] == 0)
        {
            if ((inode->blocks[IND_BLOCK] = allocate_DB()) < 0)
            {
                return NULL;
            }
        }
        blocks_array = (off_t *)mmap_ptr(inode->blocks[IND_BLOCK]);
    }
    else
    {
        blocks_array = inode->blocks;
    }

    if (*(blocks_array + block_index) == 0 && flag)
    {   
        off_t block = allocate_DB();
        if(block < 0 || (*(blocks_array + block_index) = block) == 0)
        {
            errorCheck = -ENOSPC;
            return NULL;
        }
    }

    return (char *)mmap_ptr(*(blocks_array + block_index)) + (offset % BLOCK_SIZE);
}


char* mmap_ptr(off_t offset) { 
    return (char *)mappedMemoryRegion + offset;
}
/**
 * Frees the bitmap at the given position.
 *
 * @param pos The position of the bitmap.
 * @param bitmap The pointer to the bitmap.
 */
void freeBitmap(uint32_t pos, uint32_t *bitmap)
{
    printf("free_bitmap\n");
    bitmap[pos / 32] -= 1 << (pos % 32);
}

/**
 * Frees the data block at the given block number.
 *
 * @param block The block number.
 */
void freeDblock(off_t block)
{
    memset(mmap_ptr(block), 0, BLOCK_SIZE);
    freeBitmap((block - ((struct wfs_sb *)mappedMemoryRegion)->d_blocks_ptr) / BLOCK_SIZE, (uint32_t *)mmap_ptr(((struct wfs_sb *)mappedMemoryRegion)->d_bitmap_ptr));
}

/**
 * Frees the given inode.
 *
 * @param inode The pointer to the wfs_inode structure.
 */
void free_inode(struct wfs_inode *inode)
{
    printf("free_inode\n");
    memset(inode, 0, sizeof(struct wfs_inode));
    // for (int i = 0; i < D_BLOCK; i++)
    // {
    //     if (inode->blocks[i] != 0)
    //     {
    //         freeDblock(inode->blocks[i]);
    //     }
    // }
     freeBitmap(((char*)inode - (char*)mmap_ptr(((struct wfs_sb *)mappedMemoryRegion)->i_blocks_ptr)) / BLOCK_SIZE, (uint32_t *)mmap_ptr(((struct wfs_sb *)mappedMemoryRegion)->i_bitmap_ptr));
}
/**
 * Retrieves the inode with the given number.
 *
 * @param num The inode number.
 * @return A pointer to the retrieved inode.
 */
struct wfs_inode *retrieve_inode(int num)
{
    uint32_t *bitmap = (uint32_t *)mmap_ptr(((struct wfs_sb *)mappedMemoryRegion)->i_bitmap_ptr);
    return (bitmap[num / 32] & (0x1 << (num % 32)))  ? (struct wfs_inode *)((char *)mmap_ptr(((struct wfs_sb *)mappedMemoryRegion)->i_blocks_ptr) + num * BLOCK_SIZE) : NULL;
}

/**
 * Allocates a block in the bitmap.
 *
 * @param bitmap The pointer to the bitmap.
 * @param size The size of the bitmap.
 * @return The allocated block number.
 */
ssize_t allocateBlock(uint32_t *bitmap, size_t size)
{
    printf("allocate_block\n");
    for (uint32_t i = 0; i < size; i++)
    {
        uint32_t bm_region = bitmap[i];
        if (bm_region == UINT32_MAX)
        {
            continue;
        }
        for (uint32_t k = 0; k < 32; k++)
        {
            if (!((bm_region >> k) & 0x1))
            {
                bitmap[i] = bitmap[i] | (0x1 << k);
                return i * 32 + k;
            }
        }
    }
    return -1;
}

/**
 * Allocates a data block.
 *
 * @return The allocated data block number.
 */
off_t allocate_DB() {
    struct wfs_sb *sb = (struct wfs_sb *)mappedMemoryRegion;
    off_t blockNum = allocateBlock((uint32_t *)mmap_ptr(sb->d_bitmap_ptr), sb->num_data_blocks / 32);
    if (blockNum < 0) {
        errorCheck = -ENOSPC;  // Setting error when no data blocks are left
        return -1;
    }
    return sb->d_blocks_ptr + BLOCK_SIZE * blockNum;
}

/**
 * Allocates an inode.
 *
 * @return A pointer to the allocated inode.
 */
struct wfs_inode *allocateInode()
{
    struct wfs_sb *sb = (struct wfs_sb *)mappedMemoryRegion;
    off_t inodeNum = allocateBlock((uint32_t *)mmap_ptr(sb->i_bitmap_ptr), sb->num_inodes / 32);
    if (inodeNum < 0)
    {
        errorCheck = -ENOSPC;
        return NULL;
    }
    struct wfs_inode *inode = (struct wfs_inode *)((char *)mmap_ptr(sb->i_blocks_ptr) + inodeNum * BLOCK_SIZE);
    inode->num = inodeNum;
    return inode;
}
/**
 * Retrieves the inode for the given path.
 *
 * @param enclosing The pointer to the enclosing wfs_inode structure.
 * @param path The path to the inode.
 * @param inode The pointer to the retrieved wfs_inode structure.
 * @return 0 if successful, -1 otherwise.
 */
int getInode(struct wfs_inode *enclosing, char *path, struct wfs_inode **inode)
{
    printf("get_inode_rec\n");
    printf("path: %s\n", path);
    if (!strcmp(path, ""))
    {
        *inode = enclosing;
        printf("inode: %p\n", (void *)*inode);
        return 0;
    }
    char *nextPath = path;
    printf("nextPath: %s\n", nextPath);

    while (*path != '/' && *path != '\0')
    {
        path++;
    }
    if (*path != '\0')
    {
        *path = '\0';
        path++;
    }

    // Start of dentryNum functionality
    size_t size = enclosing->size;
    struct wfs_dentry *dent;
    int inum = -1;

    for (off_t off = 0; off < size; off += sizeof(struct wfs_dentry))
    {
        dent = (struct wfs_dentry *)find_doffset(enclosing, off, 0);
        if(dent == NULL) {
            errorCheck = -ENOENT;
            return -1;
        }
        if (dent->num != 0 && !strcmp(dent->name, nextPath))
        {
            inum = dent->num;
            break;
        }
    }

    printf("inum: %d\n", inum);
    if (inum < 0)
    {
        errorCheck = -ENOENT;
        return -1;
    }
    return getInode(retrieve_inode(inum), path, inode);
}
/**
 * Retrieves the inode for the given path.
 *
 * @param path The path to the inode.
 * @param inode The pointer to the retrieved wfs_inode structure.
 * @return 0 if successful, -1 otherwise.
 */
int getInodePath(char *path, struct wfs_inode **inode)
{
    printf("get_inode_by_path\n");
    return getInode(retrieve_inode(0), path + 1, inode);
}

int main(int argc, char *argv[])
{
    int fuse_stat;
    struct stat sb;
    int fd;
    char *disk_img = argv[1];

    for (int i = 2; i < argc; i++)
    {
        argv[i - 1] = argv[i];
    }
    argc -= 1;
    if ((fd = open(disk_img, O_RDWR)) < 0)
    {
        perror("Error opening file");
        exit(1);
    }
    if (fstat(fd, &sb) < 0)
    {
        perror("Error getting file stats");
        exit(1);
    }

    mappedMemoryRegion = mmap(NULL, sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mappedMemoryRegion == MAP_FAILED)
    {
        perror("Error mapping memory");
        exit(1);
    }
    assert(retrieve_inode(0) != NULL);
    fuse_stat = fuse_main(argc, argv, &ops, NULL);
    munmap(mappedMemoryRegion, sb.st_size);
    close(fd);
    return fuse_stat;
}