// watcher_updated.c - Updated watcher that sets SYNC_PENDING for new/modified files
//
// CHANGES FROM ORIGINAL:
// 1. Uses cache_io.h for cache operations
// 2. Sets sync_status = SYNC_PENDING when detecting changes
// 3. Properly integrates with the new cache format

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include "cache_io.h"
#include "metadata_parser.h"

// Configuration defaults
#define DEFAULT_WATCH_PATH "/home/root/.local/share/remarkable/xochitl"
#define DEFAULT_LOG_PATH "/home/root/onenote-sync/logs/watcher.log"
#define DEFAULT_CACHE_PATH "/home/root/onenote-sync/cache/.sync_cache"
#define DEFAULT_CONFIG_PATH "/home/root/onenote-sync/watcher.conf"

#define BUF_LEN (10 * (sizeof(struct inotify_event) + NAME_MAX + 1))

// Global configuration
static char watch_path[PATH_MAX] = DEFAULT_WATCH_PATH;
static char log_path[PATH_MAX] = DEFAULT_LOG_PATH;
static char cache_path[PATH_MAX] = DEFAULT_CACHE_PATH;
static CacheHandle* cache = NULL;

/**
 * log_msg - Write timestamped log message
 */
void log_msg(const char* fmt, ...) {
    FILE* f = fopen(log_path, "a");
    if (!f) return;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char timestr[20];
    strftime(timestr, sizeof(timestr), "[%Y-%m-%d %H:%M:%S]", &tm);

    fprintf(f, "%s ", timestr);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fprintf(f, "\n");
    fclose(f);
}

/**
 * load_config - Load configuration from file
 */
void load_config() {
    FILE* f = fopen(DEFAULT_CONFIG_PATH, "r");
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

        // Trim whitespace
        while (*val == ' ' || *val == '\t') val++;
        val[strcspn(val, "\n\r")] = '\0';

        if (strcmp(key, "WATCH_PATH") == 0) {
            strncpy(watch_path, val, PATH_MAX - 1);
        } else if (strcmp(key, "LOG_PATH") == 0) {
            strncpy(log_path, val, PATH_MAX - 1);
        } else if (strcmp(key, "CACHE_PATH") == 0) {
            strncpy(cache_path, val, PATH_MAX - 1);
        }
    }
    fclose(f);
}

/**
 * extract_document_id - Extract document UUID from a path
 *
 * @param path: File path (e.g., "036f73e1-32ad-44a4-8909-182a7381b5a6.metadata")
 * @return: Pointer to start of UUID in path, or NULL
 */
const char* extract_document_id(const char* path) {
    if (!path) return NULL;

    // Find the last '/' to get filename
    const char* filename = strrchr(path, '/');
    if (!filename) {
        filename = path;
    } else {
        filename++; // Skip the '/'
    }

    // Check if it looks like a UUID (36 chars)
    if (strlen(filename) >= UUID_LEN) {
        // Verify it has the UUID format (8-4-4-4-12 with hyphens)
        if (filename[8] == '-' && filename[13] == '-' &&
            filename[18] == '-' && filename[23] == '-') {
            return filename;
        }
    }

    return NULL;
}

/**
 * scan_document_pages - Scan all .rm files in a document directory
 *
 * @param doc_id: Document UUID
 * @return: Number of pages updated
 */
int scan_document_pages(const char* doc_id) {
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", watch_path, doc_id);

    DIR* dir = opendir(dir_path);
    if (!dir) {
        log_msg("Cannot open directory %s: %s", dir_path, strerror(errno));
        return 0;
    }

    int pages_updated = 0;
    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL) {
        // Look for .rm files
        char* ext = strrchr(entry->d_name, '.');
        if (!ext || strcmp(ext, ".rm") != 0) continue;

        // Extract page UUID (filename without .rm extension)
        char page_uuid[UUID_LEN + 1];
        size_t name_len = ext - entry->d_name;
        if (name_len != UUID_LEN) continue;

        strncpy(page_uuid, entry->d_name, UUID_LEN);
        page_uuid[UUID_LEN] = '\0';

        // Get file modification time
        char file_path[PATH_MAX];
        snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(file_path, &st) != 0) continue;

        // Try to get page number from content file
        char page_num[8] = "";
        parse_content_file(doc_id, page_uuid, page_num, sizeof(page_num));

        // Check if this page needs updating
        DocumentEntry* doc = cache_find_document(cache, doc_id);
        PageEntry* page = doc ? cache_find_page(doc, page_uuid) : NULL;

        if (!page || page->mtime < st.st_mtime) {
            // New or modified page - mark as pending
            cache_add_or_update_page(cache, doc_id, page_uuid,
                                   page_num, st.st_mtime, SYNC_PENDING);
            pages_updated++;
            log_msg("Page %s/%s marked for sync (mtime=%ld)",
                   doc_id, page_uuid, st.st_mtime);
        }
        else if(page && page->sync_status == SYNC_UPLOADED && page->mtime < st.st_mtime)
        {
            cache_update_page_status(cache, doc_id, page_uuid, SYNC_PENDING, 0);
            page->mtime = st.st_mtime;
            pages_updated++;
            log_msg("Previously uploaded page %s/%s modified, marking for re-sync",
                doc_id, page_uuid);
        }
    }

    closedir(dir);
    return pages_updated;
}

/**
 * process_metadata_change - Process a change to a .metadata file
 *
 * @param filename: Name of the metadata file
 */
void process_metadata_change(const char* filename) {
    // Extract document ID from filename
    char doc_id[UUID_LEN + 1];
    if (strlen(filename) < UUID_LEN + 9) return; // UUID + ".metadata"

    strncpy(doc_id, filename, UUID_LEN);
    doc_id[UUID_LEN] = '\0';

    log_msg("Processing metadata change for document %s", doc_id);

    // Scan all pages in this document
    int pages_updated = scan_document_pages(doc_id);

    if (pages_updated > 0) {
        log_msg("Updated %d pages for document %s", pages_updated, doc_id);
        cache_save(cache);
    }
}

/**
 * main - Main entry point
 */
int main(int argc, char** argv) {
    // Load configuration
    load_config();

    // Override watch path if provided as argument
    if (argc > 1) {
        strncpy(watch_path, argv[1], PATH_MAX - 1);
        watch_path[PATH_MAX - 1] = '\0';
    }

    log_msg("=== Watcher started ===");
    log_msg("Watch path: %s", watch_path);
    log_msg("Cache path: %s", cache_path);
    log_msg("Log path: %s", log_path);

    // Open cache
    cache = cache_open(cache_path);
    if (!cache) {
        log_msg("ERROR: Failed to open cache");
        return 1;
    }

    // Report cache status
    int pending = cache_count_by_status(cache, SYNC_PENDING);
    int uploaded = cache_count_by_status(cache, SYNC_UPLOADED);
    int failed = cache_count_by_status(cache, SYNC_FAILED);
    log_msg("Cache loaded: %d pending, %d uploaded, %d failed",
           pending, uploaded, failed);

    // Initialize inotify
    int fd = inotify_init();
    if (fd < 0) {
        log_msg("ERROR: Failed to initialize inotify: %s", strerror(errno));
        cache_close(cache, true);
        return 1;
    }

    // Add watch
    int wd = inotify_add_watch(fd, watch_path,
                              IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_TO);
    if (wd < 0) {
        log_msg("ERROR: Failed to add watch on %s: %s",
               watch_path, strerror(errno));
        close(fd);
        cache_close(cache, true);
        return 1;
    }

    log_msg("Watching for changes...");

    // Event loop
    char buf[BUF_LEN];
    while (1) {
        int len = read(fd, buf, BUF_LEN);
        if (len < 0) {
            if (errno == EINTR) continue;
            log_msg("ERROR: Read failed: %s", strerror(errno));
            break;
        }

        // Process events
        int i = 0;
        while (i < len) {
            struct inotify_event* event = (struct inotify_event*)&buf[i];

            if (event->len > 0) {
                // Check if it's a metadata file
                if (strstr(event->name, ".metadata")) {
                    if (event->mask & (IN_CREATE | IN_MODIFY | IN_MOVED_TO)) {
                        process_metadata_change(event->name);
                    }
                }

                // Also check for direct .rm file changes in subdirectories
                if (strstr(event->name, ".rm")) {
                    // Extract document ID from the path
                    const char* doc_id_start = extract_document_id(event->name);
                    if (doc_id_start) {
                        char doc_id[UUID_LEN + 1];
                        strncpy(doc_id, doc_id_start, UUID_LEN);
                        doc_id[UUID_LEN] = '\0';
                        log_msg("Direct .rm change detected in %s", doc_id);
                        scan_document_pages(doc_id);
                        cache_save(cache);
                    }
                }
            }

            i += sizeof(struct inotify_event) + event->len;
        }
    }

    // Cleanup
    inotify_rm_watch(fd, wd);
    close(fd);
    cache_close(cache, true);
    log_msg("=== Watcher stopped ===");
    
    return 0;
}