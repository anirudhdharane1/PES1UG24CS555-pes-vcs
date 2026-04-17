// tree.c — Tree object serialization and construction

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// Forward declaration from object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;

        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;

        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }

    return 0;
}

// Sort helper
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name,
                  ((const TreeEntry *)b)->name);
}

// Serialize tree
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size ? max_size : 1);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count,
          sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;

    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];

        int written = sprintf((char *)buffer + offset,
                              "%o %s",
                              entry->mode,
                              entry->name);

        offset += written + 1;

        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── IMPLEMENTATION ─────────────────────────────────────────────────────────

static int build_tree(Index *index, const char *prefix, ObjectID *out) {
    Tree tree;
    tree.count = 0;

    char seen[256][256];
    int seen_count = 0;

    size_t plen = strlen(prefix);

    for (int i = 0; i < index->count; i++) {
        const char *full = index->entries[i].path;
        const char *rel = full;

        if (plen > 0) {
            if (strncmp(full, prefix, plen) != 0)
                continue;

            if (full[plen] != '/')
                continue;

            rel = full + plen + 1;
        }

        const char *slash = strchr(rel, '/');

        // Direct file
        if (!slash) {
            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = index->entries[i].mode;
            e->hash = index->entries[i].hash;
            snprintf(e->name, sizeof(e->name), "%s", rel);
        }
        // Subdirectory
        else {
            int len = slash - rel;
            char dirname[256];

            strncpy(dirname, rel, len);
            dirname[len] = '\0';

            int exists = 0;
            for (int j = 0; j < seen_count; j++) {
                if (strcmp(seen[j], dirname) == 0) {
                    exists = 1;
                    break;
                }
            }

            if (!exists) {
                strcpy(seen[seen_count++], dirname);

                char new_prefix[512];

                if (plen == 0)
                    snprintf(new_prefix, sizeof(new_prefix), "%s", dirname);
                else
                    snprintf(new_prefix, sizeof(new_prefix),
                             "%s/%s", prefix, dirname);

                ObjectID sub;
                if (build_tree(index, new_prefix, &sub) != 0)
                    return -1;

                TreeEntry *e = &tree.entries[tree.count++];
                e->mode = MODE_DIR;
                e->hash = sub;
                snprintf(e->name, sizeof(e->name), "%s", dirname);
            }
        }
    }

    void *raw;
    size_t raw_len;

    if (tree_serialize(&tree, &raw, &raw_len) != 0)
        return -1;

    int rc = object_write(OBJ_TREE, raw, raw_len, out);
    free(raw);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    Index index;

    if (index_load(&index) != 0)
        return -1;

    return build_tree(&index, "", id_out);
}
