// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <inttypes.h>

// Forward declaration from object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;

    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;

    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                   st.st_size != (off_t)index->entries[i].size) {
            printf("  modified:   %s\n", index->entries[i].path);
            unstaged_count++;
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;

    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0 ||
                strcmp(ent->d_name, ".pes") == 0 ||
                strcmp(ent->d_name, "pes") == 0 ||
                strstr(ent->d_name, ".o") != NULL)
                continue;

            int tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    tracked = 1;
                    break;
                }
            }

            if (!tracked) {
                struct stat st;
                if (stat(ent->d_name, &st) == 0 && S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }

    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── TODO IMPLEMENTATIONS ────────────────────────────────────────────────────

static int cmp_index(const void *a, const void *b) {
    const IndexEntry *ea = (const IndexEntry *)a;
    const IndexEntry *eb = (const IndexEntry *)b;
    return strcmp(ea->path, eb->path);
}

int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;   // no index yet is valid

    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry temp;
        char hex[HASH_HEX_SIZE + 1];

        int rc = fscanf(
            f,
            "%o %64s %" SCNu64 " %u %[^\n]\n",
            &temp.mode,
            hex,
            &temp.mtime_sec,
            &temp.size,
            temp.path
        );

        if (rc == EOF) break;
        if (rc != 5) {
            fclose(f);
            return -1;
        }

        if (hex_to_hash(hex, &temp.hash) != 0) {
            fclose(f);
            return -1;
        }

        index->entries[index->count++] = temp;
    }

    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    Index copy = *index;
    qsort(copy.entries, copy.count, sizeof(IndexEntry), cmp_index);

    FILE *f = fopen(INDEX_FILE ".tmp", "w");
    if (!f) return -1;

    for (int i = 0; i < copy.count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&copy.entries[i].hash, hex);

        fprintf(
            f,
            "%o %s %" PRIu64 " %u %s\n",
            copy.entries[i].mode,
            hex,
            copy.entries[i].mtime_sec,
            copy.entries[i].size,
            copy.entries[i].path
        );
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    return rename(INDEX_FILE ".tmp", INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    size_t size = (size_t)st.st_size;
    void *buf = malloc(size ? size : 1);
    if (!buf) {
        fclose(f);
        return -1;
    }

    if (size > 0 && fread(buf, 1, size, f) != size) {
        free(buf);
        fclose(f);
        return -1;
    }

    fclose(f);

    ObjectID blob;
    if (object_write(OBJ_BLOB, buf, size, &blob) != 0) {
        free(buf);
        return -1;
    }

    free(buf);

    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES)
            return -1;
        e = &index->entries[index->count++];
    }

    e->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    e->hash = blob;
    e->mtime_sec = (uint64_t)st.st_mtime;
    e->size = (uint32_t)st.st_size;
    snprintf(e->path, sizeof(e->path), "%s", path);

    return index_save(index);
}
