// cache_debug.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UUID_LEN 36
#define MAX_PAGE_NUM_LEN 8
#define CACHE_MAGIC 0x524D4348  // "RMCH" in hex
#define CACHE_VERSION 1

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
    printf("\nExamples:\n");
    printf("  %s /home/root/onenote-sync/cache/.sync_cache\n", prog_name);
    printf("  %s -v /home/root/onenote-sync/cache/.sync_cache\n", prog_name);
    printf("  %s -d ab46df20-ea64-4e05-b1e2-6c47fa2d73c3 cache_file\n", prog_name);
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
 * print_document_summary - Print summary info for a document
 */
void print_document_summary(const char* doc_id, uint16_t num_pages, time_t latest_mtime) {
    char time_str[32];
    format_timestamp(latest_mtime, time_str, sizeof(time_str));

    printf("Document: %s\n", doc_id);
    printf("  Pages: %d\n", num_pages);
    printf("  Latest Modified: %s\n", time_str);
    printf("\n");
}

/**
 * print_document_detailed - Print detailed info for a document
 */
void print_document_detailed(const char* doc_id, uint16_t num_pages, int verbose) {
    printf("=== Document: %s ===\n", doc_id);
    printf("Total Pages: %d\n\n", num_pages);
}

/**
 * print_page_info - Print page information
 */
void print_page_info(const char* uuid, const char* page_num, time_t mtime, int verbose) {
    char time_str[32];
    format_timestamp(mtime, time_str, sizeof(time_str));

    if (verbose) {
        printf("  Page UUID: %s\n", uuid);
        printf("  Page Number: %s\n", strlen(page_num) > 0 ? page_num : "(unknown)");
        printf("  Modified: %s (%ld)\n", time_str, mtime);
        printf("  ---\n");
    }
    else {
        printf("  %-4s  %s  %s\n",
            strlen(page_num) > 0 ? page_num : "?",
            time_str,
            uuid);
    }
}

/**
 * parse_cache_file - Main parsing function
 */
int parse_cache_file(const char* filename, int verbose, int summary_only, const char* filter_doc) {
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

    if (version != CACHE_VERSION) {
        fprintf(stderr, "Error: Unsupported version (%d, expected %d)\n",
            version, CACHE_VERSION);
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
    printf("Version: %d\n", version);
    printf("Documents: %d\n", num_docs);
    printf("\n");

    if (num_docs == 0) {
        printf("Cache is empty.\n");
        fclose(f);
        return 0;
    }

    uint32_t total_pages = 0;
    time_t global_latest = 0;
    int documents_shown = 0;

    // Process each document
    for (uint32_t i = 0; i < num_docs; i++) {
        uint8_t doc_id_len;
        if (fread(&doc_id_len, sizeof(doc_id_len), 1, f) != 1) {
            fprintf(stderr, "Error: Cannot read document ID length for document %d\n", i);
            break;
        }

        if (doc_id_len != UUID_LEN) {
            fprintf(stderr, "Error: Invalid document ID length (%d, expected %d)\n",
                doc_id_len, UUID_LEN);
            break;
        }

        char doc_id[UUID_LEN + 1];
        if (fread(doc_id, doc_id_len, 1, f) != 1) {
            fprintf(stderr, "Error: Cannot read document ID for document %d\n", i);
            break;
        }
        doc_id[doc_id_len] = '\0';

        uint16_t num_pages;
        if (fread(&num_pages, sizeof(num_pages), 1, f) != 1) {
            fprintf(stderr, "Error: Cannot read page count for document %s\n", doc_id);
            break;
        }

        // Check if we should show this document
        int show_document = (filter_doc == NULL || strcmp(doc_id, filter_doc) == 0);

        time_t doc_latest = 0;

        // Process pages for this document
        for (uint16_t j = 0; j < num_pages; j++) {
            char page_uuid[UUID_LEN + 1];
            if (fread(page_uuid, UUID_LEN, 1, f) != 1) {
                fprintf(stderr, "Error: Cannot read page UUID for page %d in document %s\n",
                    j, doc_id);
                goto cleanup;
            }
            page_uuid[UUID_LEN] = '\0';

            uint8_t page_num_len;
            if (fread(&page_num_len, sizeof(page_num_len), 1, f) != 1) {
                fprintf(stderr, "Error: Cannot read page number length for page %s\n", page_uuid);
                goto cleanup;
            }

            if (page_num_len >= MAX_PAGE_NUM_LEN) {
                fprintf(stderr, "Error: Page number too long (%d >= %d) for page %s\n",
                    page_num_len, MAX_PAGE_NUM_LEN, page_uuid);
                goto cleanup;
            }

            char page_num[MAX_PAGE_NUM_LEN];
            if (fread(page_num, page_num_len, 1, f) != 1) {
                fprintf(stderr, "Error: Cannot read page number for page %s\n", page_uuid);
                goto cleanup;
            }
            page_num[page_num_len] = '\0';

            time_t mtime;
            if (fread(&mtime, sizeof(mtime), 1, f) != 1) {
                fprintf(stderr, "Error: Cannot read modification time for page %s\n", page_uuid);
                goto cleanup;
            }

            // Track latest modification times
            if (mtime > doc_latest) doc_latest = mtime;
            if (mtime > global_latest) global_latest = mtime;

            // Print page info if we're showing this document and it's the first page
            if (show_document && j == 0) {
                if (summary_only) {
                    print_document_summary(doc_id, num_pages, doc_latest);
                }
                else {
                    print_document_detailed(doc_id, num_pages, verbose);
                    if (!verbose) {
                        printf("  %-4s  %-19s  %s\n", "Page", "Modified", "UUID");
                        printf("  %-4s  %-19s  %s\n", "----", "-------------------",
                            "------------------------------------");
                    }
                }
                documents_shown++;
            }

            // Print page info if we're showing this document and not summary mode
            if (show_document && !summary_only) {
                print_page_info(page_uuid, page_num, mtime, verbose);
            }
        }

        if (show_document && !summary_only) {
            printf("\n");
        }

        total_pages += num_pages;
    }

    // Print summary statistics
    if (!filter_doc) {
        char latest_str[32];
        format_timestamp(global_latest, latest_str, sizeof(latest_str));

        printf("=== Summary ===\n");
        printf("Total Documents: %d\n", num_docs);
        printf("Total Pages: %d\n", total_pages);
        printf("Latest Activity: %s\n", latest_str);

        if (documents_shown != num_docs) {
            printf("Documents Shown: %d\n", documents_shown);
        }
    }
    else {
        if (documents_shown == 0) {
            printf("Document '%s' not found in cache.\n", filter_doc);
        }
    }

cleanup:
    fclose(f);
    return 0;
}

/**
 * main - Entry point
 */
int main(int argc, char** argv) {
    int verbose = 0;
    int summary_only = 0;
    char* filter_doc = NULL;
    char* cache_file = NULL;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        }
        else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--summary") == 0) {
            summary_only = 1;
        }
        else if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 < argc) {
                filter_doc = argv[++i];
            }
            else {
                fprintf(stderr, "Error: -d requires a document ID\n");
                return 1;
            }
        }
        else if (argv[i][0] != '-') {
            cache_file = argv[i];
        }
        else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!cache_file) {
        fprintf(stderr, "Error: Cache file path required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    return parse_cache_file(cache_file, verbose, summary_only, filter_doc);
}