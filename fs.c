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
    node->mode = 7;
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
uint32_t find_next_free_block(FILE *fp, block_t *dest, uint32_t iblk) {
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

block_t *get_block_by_index(FILE *fp, uint32_t iblk) {
    uint64_t offset = iblk * BLOCK_SIZE;
    block_t *blk = malloc(BLOCK_SIZE);

    fseek(fp, offset, SEEK_SET);
    fread(blk, BLOCK_SIZE, 1, fp);

    return blk;
}

int mknode(FILE *fp, char *pwd, char *name, int type) {
    int nsplits = 0;
    char **pwd_split = split_string(pwd, "/", &nsplits);
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
            if (*children == 0) return 1;

            b = get_block_by_index(fp, *children);
            assert(b->attributes & BLOCK_METADATA); // block has to contain a node
            memcpy(temp, b->data, sizeof(node_t));
            assert(temp->magic == NODE_MAGIC);

            if (temp->flags & FS_DIR) {
                if (strcmp(temp->name, pwd_split[i]) == 0) {
                    found = 1;
                    memcpy(node, temp, sizeof(node_t));
                    memcpy(blk, b, BLOCK_SIZE);
                    block_offset = *children;

                    children = (uint32_t *) (blk->data + sizeof(node_t));
                    break;
                }
            }

            children++;
            free(b);
        }

        // directory was not found.
        if (found == 0) return -1;
    }
    free(temp);

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
    node->mode = (type == FS_FILE) ? 7 : (MODE_R | MODE_W);
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