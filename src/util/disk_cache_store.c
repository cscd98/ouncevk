/*
 * Copyright © 2026 OunceVK contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * Append-only shader store for Horizon's blob-callback disk cache.
 *
 * File layout:
 *
 *    store_header | record_header payload | record_header payload | ...
 *
 * The committed length in the header publishes the records. A put writes
 * and flushes its record before it moves the committed length past the
 * record. If the process stops during a put, the record stays as an
 * unpublished tail. A later put overwrites that tail.
 * Opening builds an in-memory key table from a scan of the records.
 * A sidecar index file holds a snapshot of that table. The scan skips the
 * region that the index covers. The index only makes opening faster.
 * Opening validates the index before it uses it.
 *
 * Horizon has no file locks. The store assumes that only one process at a
 * time uses it. In the process, two locks guard it. file_lock serializes the
 * file: opening and closing the store, appending records and reading them.
 * table_lock guards the in-memory key table. A lookup takes table_lock alone,
 * so a miss never waits for a record that is being written; a hit then takes
 * file_lock to read its record. When both are held, file_lock is taken first.
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "util/crc32.h"
#include "util/disk_cache.h"
#include "util/disk_cache_store.h"
#include "util/hash_table.h"
#include "util/log.h"
#include "util/macros.h"
#include "util/mesa-blake3.h"
#include "util/os_misc.h"
#include "util/os_time.h"
#include "util/ralloc.h"
#include "util/simple_mtx.h"

/* The store goes in the OunceVK configuration folder, not in sdmc:/switch,
 * where hbmenu lists every entry. */
#define STORE_DEFAULT_BASE_DIR "sdmc:/config/ouncevk"
#define STORE_DIR_NAME "shader-cache"
#define STORE_NAME_HEX_LEN 16

#define STORE_MAGIC 0x5453584eu /* "NXST" */
#define STORE_VERSION 1u
#define INDEX_MAGIC 0x4958584eu /* "NXXI" */
#define INDEX_VERSION 1u

/* Opening reads records in chunks of this size because SD access is slow. */
#define STORE_SCAN_CHUNK (1024 * 1024)

struct store_header {
   uint32_t magic;
   uint32_t version;
   uint64_t identity;
   uint64_t committed;
};

struct record_header {
   uint8_t key[CACHE_KEY_SIZE];
   uint32_t size;
   uint32_t crc;
};

struct index_header {
   uint32_t magic;
   uint32_t version;
   uint64_t identity;
   uint64_t covered;
   uint32_t entry_count;
   uint32_t entries_crc;
};

/* One table entry: the location of a key's latest payload in the store.
 * The index file uses the same entry format.
 */
struct store_entry {
   uint8_t key[CACHE_KEY_SIZE];
   uint64_t offset;
   uint32_t size;
   uint32_t crc;
};

static_assert(sizeof(struct store_header) == 24, "unpadded store header");
static_assert(sizeof(struct record_header) == CACHE_KEY_SIZE + 8,
              "unpadded record header");
static_assert(sizeof(struct index_header) == 32, "unpadded index header");
static_assert(sizeof(struct store_entry) == CACHE_KEY_SIZE + 16,
              "unpadded store entry");

#define FIRST_PAYLOAD_OFFSET \
   (sizeof(struct store_header) + sizeof(struct record_header))

struct store {
   unsigned refs;
   FILE *file;
   char *path;
   char *index_path;
   /* Random tag tying an index file to one initialization of the store. */
   uint64_t identity;
   /* Bytes of the file that hold published records, header included. */
   uint64_t committed;
   /* Committed length that the index file on disk describes, or 0. */
   uint64_t index_covered;
   uint64_t max_size;
   bool full_logged;
   /* Key -> struct store_entry, allocated as children of the table. */
   struct hash_table *table;
};

static simple_mtx_t file_lock = SIMPLE_MTX_INITIALIZER;
static simple_mtx_t table_lock = SIMPLE_MTX_INITIALIZER;
static struct store store;

/*
 * File and path helpers
 */

static bool
read_at(FILE *file, uint64_t offset, void *data, size_t size)
{
   return fseek(file, (long)offset, SEEK_SET) == 0 &&
          fread(data, 1, size, file) == size;
}

static bool
write_at(FILE *file, uint64_t offset, const void *data, size_t size)
{
   return fseek(file, (long)offset, SEEK_SET) == 0 &&
          fwrite(data, 1, size, file) == size;
}

static bool
file_size(FILE *file, uint64_t *size)
{
   long end;

   if (fseek(file, 0, SEEK_END) != 0)
      return false;

   end = ftell(file);
   if (end < 0)
      return false;

   *size = end;
   return true;
}

static char *
join_path(const char *dir, const char *name)
{
   char *path;

   if (asprintf(&path, "%s/%s", dir, name) < 0)
      return NULL;

   return path;
}

static bool
make_dir(const char *path)
{
   struct stat st;

   if (mkdir(path, 0755) == 0 || errno == EEXIST)
      return true;

   return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Creates a directory and its missing parents. A device prefix such as
 * "sdmc:" is not a directory and is never created.
 */
static bool
make_dirs(char *path)
{
   char *colon = strchr(path, ':');
   char *slash = strchr(path, '/');
   char *p = path;

   if (colon && (!slash || colon < slash))
      p = colon + 1;

   while (*p == '/')
      p++;

   for (; *p; p++) {
      bool ok;

      if (*p != '/')
         continue;

      *p = '\0';
      ok = make_dir(path);
      *p = '/';

      if (!ok)
         return false;
   }

   return make_dir(path);
}

static bool
has_store_name_prefix(const char *name)
{
   for (unsigned i = 0; i < STORE_NAME_HEX_LEN; i++) {
      char c = name[i];

      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return false;
   }

   return true;
}

/* "<16 lowercase hex digits><suffix>" */
static bool
is_store_name(const char *name, const char *suffix)
{
   return has_store_name_prefix(name) &&
          strcmp(name + STORE_NAME_HEX_LEN, suffix) == 0;
}

static bool
is_store_data_name(const char *name)
{
   return is_store_name(name, ".bin");
}

static bool
is_legacy_cache_name(const char *name)
{
   return strcmp(name, "cache.bin") == 0 || is_store_name(name, ".bin") ||
          is_store_name(name, ".idx");
}

typedef bool (*name_filter)(const char *name);

static void
free_names(char **names, unsigned count)
{
   for (unsigned i = 0; i < count; i++)
      free(names[i]);
   free(names);
}

/* Collects the directory's entry names that the filter accepts. Listing
 * first keeps deletions out of the readdir() loop.
 */
static char **
list_directory(const char *dir_path, name_filter filter, unsigned *count)
{
   char **names = NULL;
   unsigned capacity = 0;
   struct dirent *entry;
   DIR *dir;

   *count = 0;

   dir = opendir(dir_path);
   if (!dir)
      return NULL;

   while ((entry = readdir(dir)) != NULL) {
      if (!filter(entry->d_name))
         continue;

      if (*count == capacity) {
         unsigned new_capacity = MAX2(16, capacity * 2);
         char **grown = realloc(names, new_capacity * sizeof(*names));

         if (!grown)
            break;

         names = grown;
         capacity = new_capacity;
      }

      names[*count] = strdup(entry->d_name);
      if (!names[*count])
         break;

      (*count)++;
   }

   closedir(dir);
   return names;
}

/*
 * Old caches
 */

/* Removes the previous layout's directory, <base>/mesa_shader_cache. */
static void
remove_legacy_cache(const char *base_dir)
{
   char *dir_path = join_path(base_dir, CACHE_DIR_NAME);
   unsigned count;
   char **names;

   if (!dir_path)
      return;

   names = list_directory(dir_path, is_legacy_cache_name, &count);
   for (unsigned i = 0; i < count; i++) {
      char *path = join_path(dir_path, names[i]);

      if (path)
         unlink(path);
      free(path);
   }

   free_names(names, count);
   rmdir(dir_path);
   free(dir_path);
}

struct other_store {
   char *data_path;
   char *index_path;
   uint64_t size;
   time_t mtime;
};

static bool
describe_other_store(const char *dir_path, const char *data_name,
                     struct other_store *other)
{
   struct stat st;

   other->data_path = join_path(dir_path, data_name);
   if (!other->data_path)
      return false;

   if (stat(other->data_path, &st) != 0)
      goto fail;

   other->size = st.st_size;
   other->mtime = st.st_mtime;

   other->index_path = strdup(other->data_path);
   if (!other->index_path)
      goto fail;

   memcpy(other->index_path + strlen(other->index_path) - 4, ".idx", 4);
   if (stat(other->index_path, &st) == 0)
      other->size += st.st_size;

   return true;

fail:
   free(other->data_path);
   other->data_path = NULL;
   return false;
}

static int
compare_oldest_first(const void *a, const void *b)
{
   const struct other_store *x = a;
   const struct other_store *y = b;

   if (x->mtime != y->mtime)
      return x->mtime < y->mtime ? -1 : 1;

   return 0;
}

/* Stores of other driver builds share the directory. When together they
 * exceed max_size, deletes the least recently modified stores until they
 * fit.
 */
static void
evict_other_stores(const char *dir_path, const char *own_name,
                   uint64_t max_size)
{
   struct other_store *others;
   unsigned name_count, count = 0;
   uint64_t total = 0;
   char **names;

   names = list_directory(dir_path, is_store_data_name, &name_count);
   if (!names)
      return;

   others = calloc(name_count, sizeof(*others));
   if (!others) {
      free_names(names, name_count);
      return;
   }

   for (unsigned i = 0; i < name_count; i++) {
      if (strcmp(names[i], own_name) == 0)
         continue;

      if (!describe_other_store(dir_path, names[i], &others[count]))
         continue;

      total += others[count].size;
      count++;
   }

   if (total > max_size) {
      qsort(others, count, sizeof(*others), compare_oldest_first);

      for (unsigned i = 0; i < count && total > max_size; i++) {
         unlink(others[i].index_path);
         unlink(others[i].data_path);
         total -= others[i].size;
      }
   }

   for (unsigned i = 0; i < count; i++) {
      free(others[i].data_path);
      free(others[i].index_path);
   }
   free(others);
   free_names(names, name_count);
}

/*
 * Key table
 */

static uint32_t
key_hash(const void *key)
{
   return _mesa_hash_data(key, CACHE_KEY_SIZE);
}

static bool
key_equal(const void *a, const void *b)
{
   return memcmp(a, b, CACHE_KEY_SIZE) == 0;
}

/* Inserts the key or replaces its location. If allocation fails, the key is
 * not in the table.
 */
static void
table_set_locked(const uint8_t *key, uint64_t offset, uint32_t size, uint32_t crc)
{
   struct hash_entry *found = _mesa_hash_table_search(store.table, key);
   struct store_entry *entry;

   if (found) {
      entry = found->data;
   } else {
      entry = ralloc(store.table, struct store_entry);
      if (!entry)
         return;

      memcpy(entry->key, key, CACHE_KEY_SIZE);
      if (!_mesa_hash_table_insert(store.table, entry->key, entry)) {
         ralloc_free(entry);
         return;
      }
   }

   entry->offset = offset;
   entry->size = size;
   entry->crc = crc;
}

static void
table_set(const uint8_t *key, uint64_t offset, uint32_t size, uint32_t crc)
{
   simple_mtx_lock(&table_lock);
   table_set_locked(key, offset, size, crc);
   simple_mtx_unlock(&table_lock);
}

/* Copies the key's entry, or returns false when the key is not stored. */
static bool
table_lookup(const void *key, struct store_entry *out)
{
   bool found = false;

   simple_mtx_lock(&table_lock);
   if (store.table) {
      struct hash_entry *entry = _mesa_hash_table_search(store.table, key);
      if (entry) {
         *out = *(const struct store_entry *)entry->data;
         found = true;
      }
   }
   simple_mtx_unlock(&table_lock);
   return found;
}

/*
 * Store header
 */

static uint64_t
new_store_identity(void)
{
   /* SplitMix64 over the clock and an address. */
   uint64_t x = (uint64_t)os_time_get_nano() ^
                ((uint64_t)(uintptr_t)&store << 16);

   x += UINT64_C(0x9e3779b97f4a7c15);
   x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
   x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
   return x ^ (x >> 31);
}

static bool
store_write_header(void)
{
   const struct store_header header = {
      .magic = STORE_MAGIC,
      .version = STORE_VERSION,
      .identity = store.identity,
      .committed = store.committed,
   };

   return write_at(store.file, 0, &header, sizeof(header)) &&
          fflush(store.file) == 0;
}

static bool
store_read_header(struct store_header *header)
{
   return read_at(store.file, 0, header, sizeof(*header)) &&
          header->magic == STORE_MAGIC && header->version == STORE_VERSION &&
          header->committed >= sizeof(*header);
}

/* Starts an empty store with a new identity, so no index file matches. */
static bool
store_reinitialize(void)
{
   store.identity = new_store_identity();
   store.committed = sizeof(struct store_header);
   store.index_covered = 0;

   return store_write_header();
}

/*
 * Index file
 */

static bool
index_header_valid(const struct index_header *header)
{
   uint64_t max_entries;

   if (header->magic != INDEX_MAGIC || header->version != INDEX_VERSION ||
       header->identity != store.identity ||
       header->covered < sizeof(struct store_header) ||
       header->covered > store.committed)
      return false;

   /* Every entry needs a record header and at least one payload byte. */
   max_entries = (header->covered - sizeof(struct store_header)) /
                 (sizeof(struct record_header) + 1);
   return header->entry_count <= max_entries;
}

static bool
index_entry_valid(const struct store_entry *entry, uint64_t covered)
{
   return entry->size > 0 && entry->size <= DISK_CACHE_STORE_MAX_BLOB &&
          entry->offset >= FIRST_PAYLOAD_OFFSET &&
          entry->offset + entry->size <= covered;
}

static uint32_t
entries_crc(const struct store_entry *entries, uint32_t count)
{
   return count ? util_hash_crc32(entries, count * sizeof(*entries))
                : util_hash_crc32("", 0);
}

/* Reads and checks the whole index before adding any of it to the table. */
static bool
store_load_index_file(uint64_t *covered)
{
   struct store_entry *entries = NULL;
   struct index_header header;
   bool ok = false;
   FILE *file;

   file = fopen(store.index_path, "rb");
   if (!file)
      return false;

   if (fread(&header, sizeof(header), 1, file) != 1 ||
       !index_header_valid(&header))
      goto out;

   if (header.entry_count > 0) {
      entries = malloc(header.entry_count * sizeof(*entries));
      if (!entries ||
          fread(entries, sizeof(*entries), header.entry_count, file) !=
             header.entry_count)
         goto out;
   }

   if (entries_crc(entries, header.entry_count) != header.entries_crc)
      goto out;

   for (uint32_t i = 0; i < header.entry_count; i++) {
      if (!index_entry_valid(&entries[i], header.covered))
         goto out;
   }

   for (uint32_t i = 0; i < header.entry_count; i++)
      table_set(entries[i].key, entries[i].offset, entries[i].size,
                entries[i].crc);

   *covered = header.covered;
   ok = true;

out:
   free(entries);
   fclose(file);
   return ok;
}

/* Returns where the record scan starts: the end of what a valid index
 * covers, or the first record. An unusable index is deleted so that it
 * cannot match a later state of the store.
 */
static uint64_t
store_load_index(void)
{
   uint64_t covered;

   if (store_load_index_file(&covered)) {
      store.index_covered = covered;
      return covered;
   }

   store.index_covered = 0;
   unlink(store.index_path);
   return sizeof(struct store_header);
}

/* A write failure is not fatal. The index only makes opening faster. */
static void
store_write_index(void)
{
   uint32_t count = _mesa_hash_table_num_entries(store.table);
   struct store_entry *entries = NULL;
   struct index_header header;
   unsigned i = 0;
   FILE *file;
   bool ok;

   if (count > 0) {
      entries = malloc(count * sizeof(*entries));
      if (!entries)
         return;
   }

   hash_table_foreach(store.table, he)
      entries[i++] = *(const struct store_entry *)he->data;

   header = (struct index_header) {
      .magic = INDEX_MAGIC,
      .version = INDEX_VERSION,
      .identity = store.identity,
      .covered = store.committed,
      .entry_count = count,
      .entries_crc = entries_crc(entries, count),
   };

   file = fopen(store.index_path, "wb");
   if (!file) {
      free(entries);
      return;
   }

   ok = fwrite(&header, sizeof(header), 1, file) == 1 &&
        (count == 0 || fwrite(entries, sizeof(*entries), count, file) == count);
   ok = fclose(file) == 0 && ok;

   if (ok)
      store.index_covered = store.committed;

   free(entries);
}

/*
 * Record scan
 */

/* Adds the records in [start, limit) to the table and returns the end of
 * the last whole record in *end. Stops at a zero-length, oversized or
 * truncated record, or at a read error. Fails only if out of memory.
 */
static bool
store_scan(uint64_t start, uint64_t limit, uint64_t *end)
{
   uint8_t *chunk = malloc(STORE_SCAN_CHUNK);
   uint64_t chunk_start = start;
   size_t chunk_size = 0;
   uint64_t pos = start;

   if (!chunk)
      return false;

   while (pos + sizeof(struct record_header) <= limit) {
      struct record_header record;
      uint64_t record_end;

      if (pos + sizeof(record) > chunk_start + chunk_size) {
         size_t want = MIN2(STORE_SCAN_CHUNK, limit - pos);

         chunk_start = pos;
         chunk_size = 0;
         if (fseek(store.file, (long)pos, SEEK_SET) != 0)
            break;

         chunk_size = fread(chunk, 1, want, store.file);
         if (chunk_size < sizeof(record))
            break;
      }

      memcpy(&record, chunk + (pos - chunk_start), sizeof(record));

      if (record.size == 0 || record.size > DISK_CACHE_STORE_MAX_BLOB)
         break;

      record_end = pos + sizeof(record) + record.size;
      if (record_end > limit)
         break;

      table_set(record.key, pos + sizeof(record), record.size, record.crc);
      pos = record_end;
   }

   free(chunk);
   *end = pos;
   return true;
}

/*
 * Open and close
 */

/* Builds the key table from the header, the index and a record scan. If the
 * committed length runs past the last whole record, repairs the header.
 */
static bool
store_load(void)
{
   struct store_header header;
   uint64_t scan_start, scan_limit, scan_end, size;

   struct hash_table *table = _mesa_hash_table_create(NULL, key_hash, key_equal);
   if (!table)
      return false;
   simple_mtx_lock(&table_lock);
   store.table = table;
   simple_mtx_unlock(&table_lock);

   if (!store_read_header(&header))
      return store_reinitialize();

   store.identity = header.identity;
   store.committed = header.committed;

   scan_start = store_load_index();

   scan_limit = store.committed;
   if (file_size(store.file, &size))
      scan_limit = MIN2(scan_limit, size);

   if (!store_scan(scan_start, scan_limit, &scan_end))
      return false;

   /* The bytes after the last whole record are garbage. Later puts
    * overwrite them.
    */
   if (scan_end < store.committed) {
      store.committed = scan_end;
      if (!store_write_header())
         return false;
   }

   if (scan_end > scan_start)
      store_write_index();

   return true;
}

static bool
store_open_file(bool *created)
{
   *created = false;

   store.file = fopen(store.path, "r+b");
   if (store.file)
      return true;

   store.file = fopen(store.path, "w+b");
   *created = store.file != NULL;
   return *created;
}

static void
store_reset(void)
{
   simple_mtx_lock(&table_lock);
   struct hash_table *table = store.table;
   store.table = NULL;
   simple_mtx_unlock(&table_lock);

   if (table)
      _mesa_hash_table_destroy(table, NULL);
   if (store.file)
      fclose(store.file);
   free(store.path);
   free(store.index_path);

   simple_mtx_lock(&table_lock);
   memset(&store, 0, sizeof(store));
   simple_mtx_unlock(&table_lock);
}

static char *
store_base_dir(void)
{
   const char *dir = os_get_option("MESA_SHADER_CACHE_DIR");

   return strdup(dir && *dir ? dir : STORE_DEFAULT_BASE_DIR);
}

/* The store file name is the first 8 bytes of BLAKE3(driver keys). */
static void
store_name(const void *driver_keys, size_t driver_keys_size,
           char name[STORE_NAME_HEX_LEN + 1])
{
   blake3_hash hash;

   _mesa_blake3_compute(driver_keys, driver_keys_size, hash);

   for (unsigned i = 0; i < STORE_NAME_HEX_LEN / 2; i++)
      snprintf(name + 2 * i, 3, "%02x", hash[i]);
}

static bool
store_set_paths(const char *dir_path, const char *name, char **data_name)
{
   char *index_name;

   if (asprintf(data_name, "%s.bin", name) < 0) {
      *data_name = NULL;
      return false;
   }

   if (asprintf(&index_name, "%s.idx", name) < 0)
      return false;

   store.path = join_path(dir_path, *data_name);
   store.index_path = join_path(dir_path, index_name);
   free(index_name);

   return store.path && store.index_path;
}

static bool
store_open(uint64_t max_size, const void *driver_keys,
           size_t driver_keys_size)
{
   char name[STORE_NAME_HEX_LEN + 1];
   char *base_dir, *dir_path = NULL, *data_name = NULL;
   bool created, ok = false;

   store_name(driver_keys, driver_keys_size, name);
   store.max_size = max_size;

   base_dir = store_base_dir();
   if (!base_dir)
      goto out;

   dir_path = join_path(base_dir, STORE_DIR_NAME);
   if (!dir_path)
      goto out;

   if (!make_dirs(dir_path)) {
      mesa_logw("shader cache: cannot create %s", dir_path);
      goto out;
   }

   if (!store_set_paths(dir_path, name, &data_name))
      goto out;

   if (!store_open_file(&created)) {
      mesa_logw("shader cache: cannot open %s", store.path);
      goto out;
   }

   if (created) {
      remove_legacy_cache(base_dir);
      evict_other_stores(dir_path, data_name, max_size);
   }

   if (!store_load()) {
      mesa_logw("shader cache: cannot load %s", store.path);
      goto out;
   }

   mesa_logi("shader cache: %s, %u entries, %" PRIu64 " KiB", store.path,
             _mesa_hash_table_num_entries(store.table),
             store.committed / 1024);
   ok = true;

out:
   if (!ok)
      store_reset();
   free(data_name);
   free(dir_path);
   free(base_dir);
   return ok;
}

static void
store_close(void)
{
   if (store.index_covered != store.committed)
      store_write_index();

   store_write_header();
   store_reset();
}

/*
 * Blob callbacks
 */

/* Appends one record and publishes it. On any failure the committed length
 * and the table are unchanged.
 */
static void
store_append(const struct record_header *record, const void *payload)
{
   uint64_t offset = store.committed;
   uint64_t end = offset + sizeof(*record) + record->size;

   if (end > store.max_size) {
      if (!store.full_logged)
         mesa_logi("shader cache: %s is full", store.path);
      store.full_logged = true;
      return;
   }

   if (!write_at(store.file, offset, record, sizeof(*record)) ||
       fwrite(payload, 1, record->size, store.file) != record->size ||
       fflush(store.file) != 0)
      return;

   store.committed = end;
   if (!store_write_header()) {
      store.committed = offset;
      return;
   }

   table_set(record->key, offset + sizeof(*record), record->size,
             record->crc);
}

static void
store_put(const void *key, signed long key_size, const void *value,
          signed long value_size)
{
   struct record_header record;

   if (key_size != CACHE_KEY_SIZE || value_size <= 0 ||
       value_size > DISK_CACHE_STORE_MAX_BLOB)
      return;

   memcpy(record.key, key, CACHE_KEY_SIZE);
   record.size = value_size;
   record.crc = util_hash_crc32(value, value_size);

   simple_mtx_lock(&file_lock);
   if (store.file)
      store_append(&record, value);
   simple_mtx_unlock(&file_lock);
}

/* Reads an entry's payload into buffer. A payload larger than the buffer
 * is a miss, not truncated.
 */
static bool
store_read(const struct store_entry *entry, void *buffer, uint64_t capacity)
{
   bool ok;

   if (entry->size > capacity)
      return false;

   simple_mtx_lock(&file_lock);
   ok = store.file && read_at(store.file, entry->offset, buffer, entry->size);
   simple_mtx_unlock(&file_lock);
   return ok;
}

static signed long
store_get(const void *key, signed long key_size, void *value,
          signed long value_size)
{
   struct store_entry entry;

   if (key_size != CACHE_KEY_SIZE || value_size <= 0)
      return 0;

   /* A miss needs only the table. A hit reads its record under file_lock;
    * if the store changed in between, the checksum rejects what was read.
    */
   if (!table_lookup(key, &entry) || !store_read(&entry, value, value_size))
      return 0;

   if (util_hash_crc32(value, entry.size) != entry.crc)
      return 0;

   return entry.size;
}

/*
 * Public interface
 */

void
disk_cache_store_init(struct disk_cache *cache, uint64_t max_size,
                      const void *driver_keys, size_t driver_keys_size)
{
   bool opened;

   simple_mtx_lock(&file_lock);
   opened = store.refs > 0 ||
            store_open(max_size, driver_keys, driver_keys_size);
   if (opened)
      store.refs++;
   simple_mtx_unlock(&file_lock);

   if (opened)
      disk_cache_set_callbacks(cache, store_put, store_get);
}

void
disk_cache_store_fini(void)
{
   simple_mtx_lock(&file_lock);

   assert(store.refs > 0);
   if (store.refs > 0 && --store.refs == 0)
      store_close();

   simple_mtx_unlock(&file_lock);
}
