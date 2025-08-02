// watcher.c
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <signal.h>
#include <libgen.h>
#include <dirent.h>

#define BUF_LEN (10 * (sizeof(struct inotify_event) + NAME_MAX + 1))
#define CACHE_SIZE 256  // Hash table size
#define MAX_PAGE_NUM_LEN 8  // Max length for page number (like "ba", "bb", etc.)
#define UUID_LEN 36  // Standard UUID string length

// Binary cache format constants
#define CACHE_MAGIC 0x524D4348  // "RMCH" in hex
#define CACHE_VERSION 1

// Default configuration values
#define DEFAULT_WATCH_PATH   "/home/root/.local/share/remarkable/xochitl"
#define DEFAULT_LOG_PATH     "/home/root/onenote-sync/logs/watcher.log"
#define DEFAULT_CACHE_PATH   "/home/root/onenote-sync/cache/.sync_cache"
#define CONFIG_PATH          "/home/root/onenote-sync/watcher.conf"
#define DEFAULT_MAX_MEGABYTES    1    // MB
#define DEFAULT_BACKUP_CNT   3
#define DEFAULT_TIMEOUT_SEC  15

// Global configuration variables
static char watch_path[PATH_MAX];
static char log_path[PATH_MAX];
static char cache_path[PATH_MAX];
static size_t max_mb;
static int backup_cnt;
static int timeout_sec;

// Global state
static volatile sig_atomic_t running = 1;
static int timeout_counter = 0;
static int cache_dirty = 0;

/**
 * PageEntry - Represents a single page in a document
 */
typedef struct PageEntry {
    char uuid[UUID_LEN + 1];        // Page UUID
    char page_num[MAX_PAGE_NUM_LEN]; // Page number (idx from content)
    time_t mtime;                    // Last modification time
    struct PageEntry* next;          // Next page in this document
} PageEntry;

/**
 * DocumentEntry - Represents a document with its pages
 */
typedef struct DocumentEntry {
    char doc_id[UUID_LEN + 1];      // Document UUID
    PageEntry* pages;                // Linked list of pages
    struct DocumentEntry* next;      // Next document in hash bucket
} DocumentEntry;

static DocumentEntry* cache_table[CACHE_SIZE] = { 0 };

/**
 * signal_handler - Clean shutdown on SIGTERM/SIGINT
 */
void signal_handler(int sig) {
    running = 0;
}

/**
 * hash_string - Simple hash function for strings
 */
static unsigned int hash_string(const char* str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash % CACHE_SIZE;
}

/**
 * create_directory_path - Safely create directory path
 */
int create_directory_path(const char* path) {
    char temp_path[PATH_MAX];
    char* dir_path;
    char cmd[PATH_MAX + 20];
    int result;

    strncpy(temp_path, path, PATH_MAX - 1);
    temp_path[PATH_MAX - 1] = '\0';

    dir_path = dirname(temp_path);
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", dir_path);
    result = system(cmd);

    return (result == 0) ? 0 : -1;
}

/**
 * ensure_directories_exist - Create all necessary directories
 */
int ensure_directories_exist() {
    int result = 0;

    if (create_directory_path(log_path) != 0) {
        fprintf(stderr, "Warning: Could not create log directory for %s\n", log_path);
        result = -1;
    }

    if (create_directory_path(cache_path) != 0) {
        fprintf(stderr, "Warning: Could not create cache directory for %s\n", cache_path);
        result = -1;
    }

    if (create_directory_path(CONFIG_PATH) != 0) {
        fprintf(stderr, "Warning: Could not create config directory for %s\n", CONFIG_PATH);
        result = -1;
    }

    return result;
}

/**
 * rotate_logs - Battery-efficient log rotation
 */
void rotate_logs() {
    struct stat st;

    if (stat(log_path, &st) != 0 || st.st_size <= (max_mb * 1024 * 1024)) {
        return;
    }

    for (int i = backup_cnt - 1; i >= 0; i--) {
        char old_file[PATH_MAX], new_file[PATH_MAX];
        snprintf(old_file, sizeof(old_file), "%s.%d", log_path, i);
        snprintf(new_file, sizeof(new_file), "%s.%d", log_path, i + 1);
        rename(old_file, new_file);
    }

    char backup_file[PATH_MAX];
    snprintf(backup_file, sizeof(backup_file), "%s.0", log_path);
    rename(log_path, backup_file);
}

/**
 * load_config - Read configuration file
 */
void load_config() {
    strncpy(watch_path, DEFAULT_WATCH_PATH, PATH_MAX - 1);
    strncpy(log_path, DEFAULT_LOG_PATH, PATH_MAX - 1);
    strncpy(cache_path, DEFAULT_CACHE_PATH, PATH_MAX - 1);
    watch_path[PATH_MAX - 1] = '\0';
    log_path[PATH_MAX - 1] = '\0';
    cache_path[PATH_MAX - 1] = '\0';

    max_mb = DEFAULT_MAX_MEGABYTES;
    backup_cnt = DEFAULT_BACKUP_CNT;
    timeout_sec = DEFAULT_TIMEOUT_SEC;

    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char* key = line;
        char* val = eq + 1;

        while (*val == ' ' || *val == '\t') val++;
        val[strcspn(val, "\n\r")] = '\0';

        if (strcmp(key, "WATCH_PATH") == 0) {
            strncpy(watch_path, val, PATH_MAX - 1);
            watch_path[PATH_MAX - 1] = '\0';
        }
        else if (strcmp(key, "LOG_PATH") == 0) {
            strncpy(log_path, val, PATH_MAX - 1);
            log_path[PATH_MAX - 1] = '\0';
        }
        else if (strcmp(key, "CACHE_PATH") == 0) {
            strncpy(cache_path, val, PATH_MAX - 1);
            cache_path[PATH_MAX - 1] = '\0';
        }
        else if (strcmp(key, "MAX_MEGABYTES") == 0) {
            unsigned long mb = strtoul(val, NULL, 10);
            if (mb > 0 && mb < 1000) max_mb = mb;
        }
        else if (strcmp(key, "BACKUP_CNT") == 0) {
            int cnt = atoi(val);
            if (cnt >= 0 && cnt <= 10) backup_cnt = cnt;
        }
        else if (strcmp(key, "TIMEOUT_SEC") == 0) {
            int timeout = atoi(val);
            if (timeout >= 15 && timeout <= 900) timeout_sec = timeout;
        }
    }
    fclose(f);
}

/**
 * log_msg - Efficient logging
 */
void log_msg(const char* fmt, ...) {
    FILE* f = fopen(log_path, "a");
    if (!f) return;

    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm* tm_ptr = localtime_r(&now, &tm_buf);

    if (tm_ptr) {
        char timestr[20];
        strftime(timestr, sizeof(timestr), "[%H:%M:%S]", tm_ptr);
        fprintf(f, "%s ", timestr);
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fprintf(f, "\n");
    fclose(f);
}

/**
 * extract_page_numbers - Parse .content file to get page idx mappings
 */
int extract_page_numbers(const char* content_path, const char* doc_id) {
    FILE* f = fopen(content_path, "r");
    if (!f) return 0;

    char buffer[4096];
    char* content = NULL;
    size_t content_size = 0;
    size_t total_read = 0;

    // Read entire file
    while (fgets(buffer, sizeof(buffer), f)) {
        size_t len = strlen(buffer);
        content = realloc(content, total_read + len + 1);
        if (!content) break;
        strcpy(content + total_read, buffer);
        total_read += len;
    }
    fclose(f);

    if (!content) return 0;

    // Find pages array in JSON
    char* pages_start = strstr(content, "\"pages\"");
    if (!pages_start) {
        free(content);
        return 0;
    }

    // Simple JSON parsing for page entries
    char* pos = pages_start;
    int pages_found = 0;

    while ((pos = strstr(pos, "\"id\"")) != NULL) {
        // Extract page ID
        char* id_start = strchr(pos, ':');
        if (!id_start) break;

        id_start = strchr(id_start, '"');
        if (!id_start) break;
        id_start++;

        char* id_end = strchr(id_start, '"');
        if (!id_end) break;

        char page_uuid[UUID_LEN + 1];
        size_t id_len = id_end - id_start;
        if (id_len != UUID_LEN) {
            pos = id_end;
            continue;
        }

        strncpy(page_uuid, id_start, UUID_LEN);
        page_uuid[UUID_LEN] = '\0';

        // Find corresponding idx value
        char* idx_pos = pos;
        char* next_page = strstr(pos + 1, "\"id\"");
        char* search_end = next_page ? next_page : content + total_read;

        char* idx_start = strstr(idx_pos, "\"idx\"");
        if (idx_start && idx_start < search_end) {
            char* value_start = strstr(idx_start, "\"value\"");
            if (value_start && value_start < search_end) {
                char* val_quote = strchr(value_start, '"');
                if (val_quote) {
                    val_quote = strchr(val_quote + 1, '"');
                    if (val_quote) {
                        val_quote++;
                        char* val_end = strchr(val_quote, '"');
                        if (val_end) {
                            char page_num[MAX_PAGE_NUM_LEN];
                            size_t val_len = val_end - val_quote;
                            if (val_len < MAX_PAGE_NUM_LEN) {
                                strncpy(page_num, val_quote, val_len);
                                page_num[val_len] = '\0';

                                // Update cache entry with page number
                                unsigned int hash = hash_string(doc_id);
                                for (DocumentEntry* doc = cache_table[hash]; doc; doc = doc->next) {
                                    if (strcmp(doc->doc_id, doc_id) == 0) {
                                        for (PageEntry* page = doc->pages; page; page = page->next) {
                                            if (strcmp(page->uuid, page_uuid) == 0) {
                                                strncpy(page->page_num, page_num, MAX_PAGE_NUM_LEN - 1);
                                                page->page_num[MAX_PAGE_NUM_LEN - 1] = '\0';
                                                pages_found++;
                                                break;
                                            }
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        pos = id_end;
    }

    free(content);
    return pages_found;
}

/**
 * scan_document_pages - Scan .rm files in document directory
 */
int scan_document_pages(const char* doc_id) {
    char doc_dir[PATH_MAX];
    snprintf(doc_dir, sizeof(doc_dir), "%s/%s", watch_path, doc_id);

    DIR* dir = opendir(doc_dir);
    if (!dir) return 0;

    struct dirent* entry;
    int pages_updated = 0;
    unsigned int hash = hash_string(doc_id);

    // Find or create document entry
    DocumentEntry* doc_entry = NULL;
    for (DocumentEntry* d = cache_table[hash]; d; d = d->next) {
        if (strcmp(d->doc_id, doc_id) == 0) {
            doc_entry = d;
            break;
        }
    }

    if (!doc_entry) {
        doc_entry = malloc(sizeof(DocumentEntry));
        if (!doc_entry) {
            closedir(dir);
            return 0;
        }
        strncpy(doc_entry->doc_id, doc_id, UUID_LEN);
        doc_entry->doc_id[UUID_LEN] = '\0';
        doc_entry->pages = NULL;
        doc_entry->next = cache_table[hash];
        cache_table[hash] = doc_entry;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (!strstr(entry->d_name, ".rm")) continue;

        // Extract UUID from filename (remove .rm extension)
        char page_uuid[UUID_LEN + 1];
        strncpy(page_uuid, entry->d_name, UUID_LEN);
        page_uuid[UUID_LEN] = '\0';

        // Get file modification time
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", doc_dir, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        // Find or create page entry
        PageEntry* page_entry = NULL;
        for (PageEntry* p = doc_entry->pages; p; p = p->next) {
            if (strcmp(p->uuid, page_uuid) == 0) {
                page_entry = p;
                break;
            }
        }

        if (!page_entry) {
            page_entry = malloc(sizeof(PageEntry));
            if (!page_entry) continue;

            strncpy(page_entry->uuid, page_uuid, UUID_LEN);
            page_entry->uuid[UUID_LEN] = '\0';
            page_entry->page_num[0] = '\0';  // Will be filled by extract_page_numbers
            page_entry->mtime = st.st_mtime;
            page_entry->next = doc_entry->pages;
            doc_entry->pages = page_entry;
            pages_updated++;
            cache_dirty = 1;
        }
        else if (page_entry->mtime < st.st_mtime) {
            page_entry->mtime = st.st_mtime;
            pages_updated++;
            cache_dirty = 1;
        }
    }

    closedir(dir);

    // Extract page numbers from content file
    char content_path[PATH_MAX];
    snprintf(content_path, sizeof(content_path), "%s/%s.content", watch_path, doc_id);
    extract_page_numbers(content_path, doc_id);

    return pages_updated;
}

/**
 * load_cache - Load binary cache from disk
 */
void load_cache() {
    FILE* f = fopen(cache_path, "rb");
    if (!f) return;

    // Read and verify header
    uint32_t magic, num_docs;
    uint8_t version;

    if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != CACHE_MAGIC ||
        fread(&version, sizeof(version), 1, f) != 1 || version != CACHE_VERSION ||
        fread(&num_docs, sizeof(num_docs), 1, f) != 1) {
        fclose(f);
        return;
    }

    // Read documents
    for (uint32_t i = 0; i < num_docs; i++) {
        uint8_t doc_id_len;
        if (fread(&doc_id_len, sizeof(doc_id_len), 1, f) != 1) break;

        if (doc_id_len != UUID_LEN) break;

        DocumentEntry* doc = malloc(sizeof(DocumentEntry));
        if (!doc) break;

        if (fread(doc->doc_id, doc_id_len, 1, f) != 1) {
            free(doc);
            break;
        }
        doc->doc_id[doc_id_len] = '\0';

        uint16_t num_pages;
        if (fread(&num_pages, sizeof(num_pages), 1, f) != 1) {
            free(doc);
            break;
        }

        doc->pages = NULL;

        // Read pages
        for (uint16_t j = 0; j < num_pages; j++) {
            PageEntry* page = malloc(sizeof(PageEntry));
            if (!page) break;

            if (fread(page->uuid, UUID_LEN, 1, f) != 1) {
                free(page);
                break;
            }
            page->uuid[UUID_LEN] = '\0';

            uint8_t page_num_len;
            if (fread(&page_num_len, sizeof(page_num_len), 1, f) != 1 ||
                page_num_len >= MAX_PAGE_NUM_LEN) {
                free(page);
                break;
            }

            if (fread(page->page_num, page_num_len, 1, f) != 1) {
                free(page);
                break;
            }
            page->page_num[page_num_len] = '\0';

            if (fread(&page->mtime, sizeof(page->mtime), 1, f) != 1) {
                free(page);
                break;
            }

            page->next = doc->pages;
            doc->pages = page;
        }

        // Add document to hash table
        unsigned int hash = hash_string(doc->doc_id);
        doc->next = cache_table[hash];
        cache_table[hash] = doc;
    }

    fclose(f);
}

/**
 * save_cache - Save binary cache to disk
 */
void save_cache() {
    if (!cache_dirty) return;

    FILE* f = fopen(cache_path, "wb");
    if (!f) return;

    // Count documents
    uint32_t num_docs = 0;
    for (int i = 0; i < CACHE_SIZE; i++) {
        for (DocumentEntry* doc = cache_table[i]; doc; doc = doc->next) {
            num_docs++;
        }
    }

    // Write header
    uint32_t magic = CACHE_MAGIC;
    uint8_t version = CACHE_VERSION;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&version, sizeof(version), 1, f);
    fwrite(&num_docs, sizeof(num_docs), 1, f);

    // Write documents
    for (int i = 0; i < CACHE_SIZE; i++) {
        for (DocumentEntry* doc = cache_table[i]; doc; doc = doc->next) {
            uint8_t doc_id_len = UUID_LEN;
            fwrite(&doc_id_len, sizeof(doc_id_len), 1, f);
            fwrite(doc->doc_id, doc_id_len, 1, f);

            // Count pages
            uint16_t num_pages = 0;
            for (PageEntry* page = doc->pages; page; page = page->next) {
                num_pages++;
            }
            fwrite(&num_pages, sizeof(num_pages), 1, f);

            // Write pages
            for (PageEntry* page = doc->pages; page; page = page->next) {
                fwrite(page->uuid, UUID_LEN, 1, f);

                uint8_t page_num_len = strlen(page->page_num);
                fwrite(&page_num_len, sizeof(page_num_len), 1, f);
                fwrite(page->page_num, page_num_len, 1, f);
                fwrite(&page->mtime, sizeof(page->mtime), 1, f);
            }
        }
    }

    fclose(f);
    cache_dirty = 0;
}

/**
 * cleanup_cache - Free all cache memory
 */
void cleanup_cache() {
    for (int i = 0; i < CACHE_SIZE; i++) {
        DocumentEntry* doc = cache_table[i];
        while (doc) {
            DocumentEntry* next_doc = doc->next;

            PageEntry* page = doc->pages;
            while (page) {
                PageEntry* next_page = page->next;
                free(page);
                page = next_page;
            }

            free(doc);
            doc = next_doc;
        }
        cache_table[i] = NULL;
    }
}

/**
 * build_event_name - Create event description string
 */
void build_event_name(uint32_t mask, char* buffer, size_t size) {
    buffer[0] = '\0';

    if (mask & IN_CREATE)     strncat(buffer, "CREATE+", size - strlen(buffer) - 1);
    if (mask & IN_MODIFY)     strncat(buffer, "MODIFY+", size - strlen(buffer) - 1);
    if (mask & IN_DELETE)     strncat(buffer, "DELETE+", size - strlen(buffer) - 1);
    if (mask & IN_MOVED_FROM) strncat(buffer, "MOVED_FROM+", size - strlen(buffer) - 1);
    if (mask & IN_MOVED_TO)   strncat(buffer, "MOVED_TO+", size - strlen(buffer) - 1);

    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '+') {
        buffer[len - 1] = '\0';
    }
}

/**
 * main - Entry point
 */
int main(int argc, char** argv) {
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    load_config();

    if (ensure_directories_exist() != 0) {
        fprintf(stderr, "Warning: Some directories could not be created\n");
    }

    rotate_logs();
    load_cache();

    int inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        log_msg("ERROR: inotify_init1 failed: %s", strerror(errno));
        return 1;
    }

    int watch_fd = inotify_add_watch(inotify_fd, watch_path,
        IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);

    if (watch_fd < 0) {
        log_msg("ERROR: cannot watch %s: %s", watch_path, strerror(errno));
        close(inotify_fd);
        return 1;
    }

    log_msg("Started watching %s (timeout=%ds)", watch_path, timeout_sec);

    char buffer[BUF_LEN] __attribute__((aligned(8)));

    while (running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(inotify_fd, &read_fds);

        struct timeval tv = { timeout_sec, 0 };

        int select_result = select(inotify_fd + 1, &read_fds, NULL, NULL, &tv);

        if (select_result < 0) {
            if (errno == EINTR) continue;
            log_msg("ERROR: select failed: %s", strerror(errno));
            break;
        }

        if (select_result == 0) {
            timeout_counter++;

            if (timeout_counter >= 10) {
                rotate_logs();
                timeout_counter = 0;
                save_cache();
            }
            continue;
        }

        ssize_t bytes_read = read(inotify_fd, buffer, BUF_LEN);
        if (bytes_read < 0) {
            if (errno == EAGAIN) continue;
            log_msg("ERROR: read failed: %s", strerror(errno));
            break;
        }

        char* ptr = buffer;
        while (ptr < buffer + bytes_read) {
            struct inotify_event* event = (struct inotify_event*)ptr;
            ptr += sizeof(struct inotify_event) + event->len;

            if (event->len == 0 || !strstr(event->name, ".metadata")) {
                continue;
            }

            // Extract document ID (remove .metadata extension)
            char doc_id[UUID_LEN + 1];
            strncpy(doc_id, event->name, UUID_LEN);
            doc_id[UUID_LEN] = '\0';

            char event_desc[128];
            build_event_name(event->mask, event_desc, sizeof(event_desc));

            // Scan document pages for changes
            int pages_updated = scan_document_pages(doc_id);

            if (pages_updated > 0) {
                log_msg("%s on %s -> %d pages updated", event_desc, doc_id, pages_updated);
                save_cache();
            }
            else {
                log_msg("%s on %s -> no page changes", event_desc, doc_id);
            }
        }
    }

    log_msg("Shutting down watcher");
    save_cache();
    cleanup_cache();
    inotify_rm_watch(inotify_fd, watch_fd);
    close(inotify_fd);

    return 0;
}