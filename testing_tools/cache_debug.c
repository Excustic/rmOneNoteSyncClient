// cache_debug_v2.c - Cache debug tool supporting both version 1 and 2
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UUID_LEN 36
#define MAX_PAGE_NUM_LEN 8
#define CACHE_MAGIC 0x524D4348  // "RMCH" in hex
#define CACHE_VERSION_1 1
#define CACHE_VERSION_2 2

// Sync status values (version 2 only)
typedef enum {
    SYNC_PENDING = 0,
    SYNC_UPLOADED = 1,
    SYNC_FAILED = 2,
    SYNC_SKIPPED = 3
} sync_status_t;

/**
 * print_usage - Display usage information
 */
void print_usage(const char* prog_name) {
    printf("Usage: %s [OPTIONS] <cache_file>\n", prog_name);
    printf("\nOptions:\n");
    printf("  -h, --help     Show this help message\n");
    printf("  -v, --verbose  Show detailed output\n");
    printf("  -s, --summary  Show summary only\n");
    printf("  -d DOC_ID      Show only specific document\n");
    printf("  -p             Show only pending pages (version 2)\n");
    printf("  -u             Show only uploaded pages (version 2)\n");
    printf("  -f             Show only failed pages (version 2)\n");
    printf("\nExamples:\n");
    printf("  %s /home/root/onenote-sync/cache/.sync_cache\n", prog_name);
    printf("  %s -v /home/root/onenote-sync/cache/.sync_cache\n", prog_name);
    printf("  %s -p cache_file  # Show pending uploads\n", prog_name);
    printf("\n");
}

/**
 * format_timestamp - Convert timestamp to readable format
 */
void format_timestamp(time_t timestamp, char* buffer, size_t size) {
    if (timestamp == 0) {
        strncpy(buffer, "Never", size - 1);
        buffer[size - 1] = '\0';
        return;
    }

    struct tm* tm_info = localtime(&timestamp);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

/**
 * status_to_string - Convert sync status to string
 */
const char* status_to_string(uint8_t status) {
    switch (status) {
        case SYNC_PENDING: return "PENDING";
        case SYNC_UPLOADED: return "UPLOADED";
        case SYNC_FAILED: return "FAILED";
        case SYNC_SKIPPED: return "SKIPPED";
        default: return "UNKNOWN";
    }
}

/**
 * parse_cache_file - Main parsing function
 */
int parse_cache_file(const char* filename, int verbose, int summary_only, 
                    const char* filter_doc, int filter_status, int status_value) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open cache file '%s'\n", filename);
        return 1;
    }

    // Read and verify header
    uint32_t magic, num_docs;
    uint8_t version;

    if (fread(&magic, sizeof(magic), 1, f) != 1) {
        fprintf(stderr, "Error: Cannot read magic number\n");
        fclose(f);
        return 1;
    }

    if (magic != CACHE_MAGIC) {
        fprintf(stderr, "Error: Invalid magic number (0x%08X, expected 0x%08X)\n",
            magic, CACHE_MAGIC);
        fclose(f);
        return 1;
    }

    if (fread(&version, sizeof(version), 1, f) != 1) {
        fprintf(stderr, "Error: Cannot read version\n");
        fclose(f);
        return 1;
    }

    if (version != CACHE_VERSION_1 && version != CACHE_VERSION_2) {
        fprintf(stderr, "Error: Unsupported version (%d)\n", version);
        fclose(f);
        return 1;
    }

    if (fread(&num_docs, sizeof(num_docs), 1, f) != 1) {
        fprintf(stderr, "Error: Cannot read document count\n");
        fclose(f);
        return 1;
    }

    // Print header info
    printf("=== Cache File Debug Info ===\n");
    printf("File: %s\n", filename);
    printf("Magic: 0x%08X (RMCH)\n", magic);
    printf("Version: %d%s\n", version, 
           version == CACHE_VERSION_2 ? " (with sync status)" : " (legacy)");
    printf("Documents: %d\n", num_docs);
    printf("\n");

    if (num_docs == 0) {
        printf("Cache is empty.\n");
        fclose(f);
        return 0;
    }

    // Counters
    uint32_t total_pages = 0;
    uint32_t pending_count = 0;
    uint32_t uploaded_count = 0;
    uint32_t failed_count = 0;
    uint32_t skipped_count = 0;

    // Process each document
    for (uint32_t i = 0; i < num_docs; i++) {
        uint8_t doc_id_len;
        if (fread(&doc_id_len, sizeof(doc_id_len), 1, f) != 1) break;

        if (doc_id_len != UUID_LEN) break;

        char doc_id[UUID_LEN + 1];
        if (fread(doc_id, doc_id_len, 1, f) != 1) break;
        doc_id[doc_id_len] = '\0';

        uint16_t num_pages;
        if (fread(&num_pages, sizeof(num_pages), 1, f) != 1) break;

        // Check if we should show this document
        int show_document = (!filter_doc || strcmp(doc_id, filter_doc) == 0);

        if (show_document && !summary_only && !filter_status) {
            printf("=== Document: %s ===\n", doc_id);
            printf("Total Pages: %d\n\n", num_pages);
            
            if (!verbose) {
                if (version == CACHE_VERSION_2) {
                    printf("  %-4s  %-19s  %-10s  %-36s\n", 
                           "Page", "Modified", "Status", "UUID");
                    printf("  %-4s  %-19s  %-10s  %-36s\n", 
                           "----", "-------------------", "----------",
                           "------------------------------------");
                } else {
                    printf("  %-4s  %-19s  %-36s\n", 
                           "Page", "Modified", "UUID");
                    printf("  %-4s  %-19s  %-36s\n", 
                           "----", "-------------------",
                           "------------------------------------");
                }
            }
        }

        // Read pages
        for (uint16_t j = 0; j < num_pages; j++) {
            char page_uuid[UUID_LEN + 1];
            if (fread(page_uuid, UUID_LEN, 1, f) != 1) goto cleanup;
            page_uuid[UUID_LEN] = '\0';

            uint8_t page_num_len;
            if (fread(&page_num_len, sizeof(page_num_len), 1, f) != 1) goto cleanup;

            char page_num[MAX_PAGE_NUM_LEN] = "";
            if (page_num_len > 0 && page_num_len < MAX_PAGE_NUM_LEN) {
                if (fread(page_num, page_num_len, 1, f) != 1) goto cleanup;
                page_num[page_num_len] = '\0';
            }

            time_t mtime;
            if (fread(&mtime, sizeof(mtime), 1, f) != 1) goto cleanup;

            uint8_t sync_status = SYNC_PENDING;
            uint8_t retry_count = 0;

            // Read sync status if version 2
            if (version == CACHE_VERSION_2) {
                if (fread(&sync_status, sizeof(sync_status), 1, f) != 1) goto cleanup;
                if (fread(&retry_count, sizeof(retry_count), 1, f) != 1) goto cleanup;
                
                // Update counters
                switch (sync_status) {
                    case SYNC_PENDING: pending_count++; break;
                    case SYNC_UPLOADED: uploaded_count++; break;
                    case SYNC_FAILED: failed_count++; break;
                    case SYNC_SKIPPED: skipped_count++; break;
                }
            }

            // Check if we should show this page
            int show_page = show_document && !summary_only;
            if (filter_status && sync_status != status_value) {
                show_page = 0;
            }

            if (show_page) {
                char time_str[32];
                format_timestamp(mtime, time_str, sizeof(time_str));

                if (verbose) {
                    printf("  Page UUID: %s\n", page_uuid);
                    printf("  Page Number: %s\n", 
                           strlen(page_num) > 0 ? page_num : "(unknown)");
                    printf("  Modified: %s (%ld)\n", time_str, mtime);
                    if (version == CACHE_VERSION_2) {
                        printf("  Sync Status: %s\n", status_to_string(sync_status));
                        if (retry_count > 0) {
                            printf("  Retry Count: %d\n", retry_count);
                        }
                    }
                    printf("  ---\n");
                } else {
                    if (version == CACHE_VERSION_2) {
                        printf("  %-4s  %s  %-10s  %s\n",
                            strlen(page_num) > 0 ? page_num : "?",
                            time_str,
                            status_to_string(sync_status),
                            page_uuid);
                    } else {
                        printf("  %-4s  %s  %s\n",
                            strlen(page_num) > 0 ? page_num : "?",
                            time_str,
                            page_uuid);
                    }
                }
            }

            total_pages++;
        }

        if (show_document && !summary_only && !filter_status) {
            printf("\n");
        }
    }

cleanup:
    fclose(f);

    // Print summary
    if (!filter_doc || summary_only) {
        printf("=== Summary ===\n");
        printf("Total Pages: %d\n", total_pages);
        if (version == CACHE_VERSION_2) {
            printf("Status Breakdown:\n");
            printf("  Pending:  %d\n", pending_count);
            printf("  Uploaded: %d\n", uploaded_count);
            printf("  Failed:   %d\n", failed_count);
            printf("  Skipped:  %d\n", skipped_count);
        }
    }

    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    int verbose = 0;
    int summary_only = 0;
    char* filter_doc = NULL;
    int filter_status = 0;
    int status_value = 0;
    char* cache_file = NULL;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--summary") == 0) {
            summary_only = 1;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            filter_doc = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0) {
            filter_status = 1;
            status_value = SYNC_PENDING;
        } else if (strcmp(argv[i], "-u") == 0) {
            filter_status = 1;
            status_value = SYNC_UPLOADED;
        } else if (strcmp(argv[i], "-f") == 0) {
            filter_status = 1;
            status_value = SYNC_FAILED;
        } else if (argv[i][0] != '-') {
            cache_file = argv[i];
        }
    }

    if (!cache_file) {
        fprintf(stderr, "Error: No cache file specified\n");
        print_usage(argv[0]);
        return 1;
    }

    return parse_cache_file(cache_file, verbose, summary_only, 
                          filter_doc, filter_status, status_value);
}