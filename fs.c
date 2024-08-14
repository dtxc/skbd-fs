#include "fs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


char **split_string(const char *str, const char *delimiter, int *num_tokens) {
    char *str_copy, *token, *saveptr;
    int count = 0;
    char **result = NULL;
    str_copy = strdup(str);

    // count the tokens to allocate memory
    token = strtok_r(str_copy, delimiter, &saveptr);
    while (token != NULL) {
        count++;
        token = strtok_r(NULL, delimiter, &saveptr);
    }

    result = (char **)malloc(count * sizeof(char*));
    
    // extract the tokens
    strcpy(str_copy, str);
    token = strtok_r(str_copy, delimiter, &saveptr);
    count = 0;
    while (token != NULL) {
        result[count] = strdup(token);
        count++;
        token = strtok_r(NULL, delimiter, &saveptr);
    }

    free(str_copy);
    *num_tokens = count;
    return result;
}

uint8_t mode_calc(uint8_t user, uint8_t kernel) {
    return user << 3 | kernel;
}

void new_fs(FILE *fp, uint64_t size, int isos) {
    if (size % BLOCK_SIZE != 0) {
        printf("fs size must be a multiple of %d kib", BLOCK_SIZE / 1024);
        return;
    }

    block_t *blk = malloc(BLOCK_SIZE);
    blk->next = 0;
    blk->attributes |= BLOCK_METADATA;

    fs_header_t *fs = malloc(sizeof(fs_header_t));
    strcpy(fs->magic, FS_MAGIC);
    fs->size = size;
    fs->block_size = BLOCK_SIZE;
    
    memcpy(blk->data, fs, sizeof(fs_header_t));
    fwrite(blk, BLOCK_SIZE, 1, fp);

    blk->next = 0;
    blk->attributes |= BLOCK_METADATA;
    memset(blk->data, 0, sizeof(blk->data));

    node_t *node = malloc(sizeof(node_t));
    node->first_block = 1;
    node->flags |= FS_DIR;
    node->magic = NODE_MAGIC;
    node->size = 0;
    node->mode = mode_calc(MODE_R, MODE_R | MODE_W);
    strcpy(node->name, "ROOT");

    memcpy(blk->data, node, sizeof(node_t));
    fwrite(blk, BLOCK_SIZE, 1, fp);

    blk->next = 0;
    blk->attributes = 0;
    blk->attributes |= BLOCK_FREE;
    memset(blk->data, 0, sizeof(blk->data));

    for (uint32_t i = 2 * BLOCK_SIZE; i < size; i+=BLOCK_SIZE) {
        fwrite(blk, BLOCK_SIZE, 1, fp);
    }

    mknode(fp, "/", "dev", FS_DIR);
    mknode(fp, "/dev", "stdout", FS_CHARDEV);
    mknode(fp, "/dev", "stdin", FS_CHARDEV);
}

/*
    finds the next free block in the filesystem.
    returns null if there are no free blocks available

    iblk: starting block
*/
static uint32_t find_next_free_block(FILE *fp, block_t *dest, uint32_t iblk) {
    uint64_t offset = iblk * BLOCK_SIZE;
    uint32_t fs_size = 1048576;

    block_t *blk = malloc(BLOCK_SIZE);
    fseek(fp, offset, SEEK_SET);

    while (offset < fs_size) {
        memset(blk, 0, BLOCK_SIZE);
        fread(blk, BLOCK_SIZE, 1, fp);

        if (blk->attributes & BLOCK_FREE) {
            memcpy(dest, blk, BLOCK_SIZE);
            free(blk);
            return offset / BLOCK_SIZE;
        }

        offset += BLOCK_SIZE;
    }

    free(blk);
    return 0;
}

static block_t *get_block_by_index(FILE *fp, uint32_t iblk) {
    uint64_t offset = iblk * BLOCK_SIZE;
    block_t *blk = malloc(BLOCK_SIZE);

    fseek(fp, offset, SEEK_SET);
    fread(blk, BLOCK_SIZE, 1, fp);

    return blk;
}

static uint32_t find_node(FILE *fp, char *path, int type, node_t *dst) {
    int nsplits = 0;
    char **pwd_split = split_string(path, "/", &nsplits);
    block_t *blk = get_block_by_index(fp, 1);
    node_t *node = malloc(sizeof(node_t));

    memcpy(node, blk->data, sizeof(node_t));
    uint32_t *children = (uint32_t *) (blk->data + sizeof(node_t));

    int found = 0;
    uint32_t block_offset = 1;
    node_t *temp = malloc(sizeof(node_t));
    block_t *b;
    for (int i = 0; i < nsplits; i++) {
        found = 0;
        for (int j = 0; j < node->size; j++) { // iterate through each subdirectory of the parent.
            // possible data corruption
            if (*children == 0) return 0;

            b = get_block_by_index(fp, *children);
            assert(b->attributes & BLOCK_METADATA); // block has to contain a node
            memcpy(temp, b->data, sizeof(node_t));
            assert(temp->magic == NODE_MAGIC);

            int target_type = (i == nsplits-1) ? type : FS_DIR;
            if (temp->flags & target_type) {
                if (strcmp(temp->name, pwd_split[i]) == 0) {
                    found = 1;
                    block_offset = *children;
                    memcpy(node, temp, sizeof(node_t));
                    memcpy(blk, b, BLOCK_SIZE);

                    children = (uint32_t *) (blk->data + sizeof(node_t));
                    free(b);
                    break;
                }
            }

            children++;
            free(b);
        }

        // node was not found.
        if (found == 0) {
            return 0;
        }
    }
    memcpy(dst, node, sizeof(node_t));
    free(node);
    free(temp);

    return block_offset;
}

int mknode(FILE *fp, char *pwd, char *name, int type) {
    node_t *node = malloc(sizeof(node_t));
    uint32_t block_offset = find_node(fp, pwd, FS_DIR, node);
    if (block_offset == 0) {
        free(node);
        printf("pwd: %s, name: %s\n", pwd, name);
        return -1;
    }

    block_t *blk = get_block_by_index(fp, block_offset);
    memcpy(node, blk->data, sizeof(node_t));

    uint32_t parent_size = node->size;
    uint32_t parent_off = node->first_block;

    block_t *block = malloc(BLOCK_SIZE);
    uint32_t r = find_next_free_block(fp, block, block_offset);
    assert(r > 0);

    fseek(fp, parent_off * BLOCK_SIZE, SEEK_SET);

    memcpy(blk->data + sizeof(node_t) + node->size * 4, &r, 4);
    node->size++;
    memcpy(blk->data, node, sizeof(node_t));
    fwrite(blk, BLOCK_SIZE, 1, fp);
    fseek(fp, r * BLOCK_SIZE, SEEK_SET);
    
    memset(blk, 0, BLOCK_SIZE);
    memset(node, 0, sizeof(node_t));

    node->magic = NODE_MAGIC;
    node->first_block = r;
    node->flags |= type;
    if (type == FS_FILE) node->mode = 63; // rwx rwx
    else if (type == FS_DIR) node->mode = 54; // rw- rw-
    else if (type == FS_CHARDEV) node->mode = 38; // r-- rw-
    node->size = 0;
    strcpy(node->name, name);

    blk->attributes |= BLOCK_METADATA;
    blk->next = 0;
    memcpy(blk->data, node, sizeof(node_t));
    fwrite(blk, BLOCK_SIZE, 1, fp);

    free(blk);
    free(node);
    return 0;
}

// if both append and write is selected, write will be executed
file_t *_fopen(FILE *fp, char *path, uint8_t mode) {
    if (mode == 0) return NULL;

    node_t *node = malloc(sizeof(node_t));
    uint32_t off = find_node(fp, path, FS_FILE, node);

    if (off == 0) {
        free(node);
        return NULL;
    }

    file_t *file = malloc(sizeof(file_t));

    if (mode & FMODE_W) {
        if ((node->mode & MODE_W) == 0) {
            free(node);
            free(file);
            return NULL; // not writeable
        }

        file->mode = node->mode;
        file->open_mode = mode;
        file->ptr_local = 0;
        file->ptr_global = BLOCK_SIZE * node->first_block;
        file->size = node->size;
        file->start = node->first_block;
        strcpy(file->name, node->name);
    } else if (mode & FMODE_A) { // append
        if ((node->mode & MODE_W) == 0) {
            free(node);
            free(file);
            return NULL;
        }

        // we need to find the last block to set the file pointer
        block_t *block = get_block_by_index(fp, node->first_block);
        int next;
        while (block->next != 0) {
            next = block->next;
            free(block);

            block = get_block_by_index(fp, block->next);
        }

        file_t *file = malloc(sizeof(file_t));
        file->mode = node->mode;
        file->open_mode = mode;
        file->ptr_local = node->size;
        file->ptr_global = next * BLOCK_SIZE + (BLOCK_SIZE - node->size % BLOCK_SIZE);
        file->size = node->size;
        file->start = node->first_block;
        strcpy(file->name, node->name);
    } else if (mode & FMODE_R) {
        file_t *file = malloc(sizeof(file_t));
        file->mode = node->mode;
        file->open_mode = mode;
        file->ptr_local = 0;
        file->ptr_global = BLOCK_SIZE * node->first_block;
        file->size = node->size;
        file->start = node->first_block;
        strcpy(file->name, node->name);
    }

    free(node);
    return file;
}

int main() {
    FILE *fp = fopen("image.bin", "r+b");
    new_fs(fp, 1024 * 1024, 1);
}
