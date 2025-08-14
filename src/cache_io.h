// cache_io.h - Shared cache structures and I/O functions
#ifndef CACHE_IO_H
#define CACHE_IO_H

#include <stdint.h>
#include <time.h>
#include <stdbool.h>

#define CACHE_MAGIC 0x524D4348  // "RMCH" in hex
#define CACHE_VERSION 2         // Version 2 adds sync status fields
#define UUID_LEN 36
#define MAX_PAGE_NUM_LEN 8
#define PATH_MAX 4096

// Sync status values
typedef enum {
    SYNC_PENDING = 0,    // Needs to be uploaded
    SYNC_UPLOADED = 1,   // Successfully uploaded
    SYNC_FAILED = 2,     // Failed after max retries
    SYNC_SKIPPED = 3     // Skipped (not under shared_path)
} sync_status_t;

/**
 * PageEntry - Represents a single page within a document
 */
typedef struct PageEntry {
    char uuid[UUID_LEN + 1];           // Page UUID
    char page_num[MAX_PAGE_NUM_LEN];   // Page number (idx from content file)
    time_t mtime;                      // Last modification time
    uint8_t sync_status;               // Upload status
    uint8_t retry_count;               // Number of retry attempts
    struct PageEntry* next;            // Next page in linked list
} PageEntry;

/**
 * DocumentEntry - Represents a document with multiple pages
 */
typedef struct DocumentEntry {
    char doc_id[UUID_LEN + 1];        // Document UUID
    PageEntry* pages;                  // Linked list of pages
    struct DocumentEntry* next;        // Next document in hash table bucket
} DocumentEntry;

/**
 * CacheHandle - Opaque handle for cache operations
 */
typedef struct CacheHandle {
    DocumentEntry** table;             // Hash table of documents
    size_t table_size;                // Size of hash table
    bool dirty;                        // Whether cache needs saving
    char path[PATH_MAX];              // Path to cache file
} CacheHandle;

/**
 * cache_open - Open or create a cache file
 * 
 * @param path: Path to cache file
 * @return: Cache handle or NULL on error
 */
CacheHandle* cache_open(const char* path);

/**
 * cache_close - Close cache and free resources
 * 
 * @param cache: Cache handle
 * @param save: Whether to save changes before closing
 */
void cache_close(CacheHandle* cache, bool save);

/**
 * cache_save - Save cache to disk
 * 
 * @param cache: Cache handle
 * @return: 0 on success, -1 on error
 */
int cache_save(CacheHandle* cache);

/**
 * cache_find_document - Find a document by ID
 * 
 * @param cache: Cache handle
 * @param doc_id: Document UUID
 * @return: Document entry or NULL if not found
 */
DocumentEntry* cache_find_document(CacheHandle* cache, const char* doc_id);

/**
 * cache_find_page - Find a page within a document
 * 
 * @param doc: Document entry
 * @param page_uuid: Page UUID
 * @return: Page entry or NULL if not found
 */
PageEntry* cache_find_page(DocumentEntry* doc, const char* page_uuid);

/**
 * cache_add_or_update_page - Add or update a page entry
 * 
 * @param cache: Cache handle
 * @param doc_id: Document UUID
 * @param page_uuid: Page UUID
 * @param page_num: Page number (can be empty string)
 * @param mtime: Modification time
 * @param status: Sync status
 * @return: 0 on success, -1 on error
 */
int cache_add_or_update_page(CacheHandle* cache, 
                             const char* doc_id,
                             const char* page_uuid,
                             const char* page_num,
                             time_t mtime,
                             sync_status_t status);

/**
 * cache_update_page_status - Update sync status of a page
 * 
 * @param cache: Cache handle
 * @param doc_id: Document UUID
 * @param page_uuid: Page UUID
 * @param status: New sync status
 * @param retry_count: New retry count
 * @return: 0 on success, -1 on error
 */
int cache_update_page_status(CacheHandle* cache,
                             const char* doc_id,
                             const char* page_uuid,
                             sync_status_t status,
                             uint8_t retry_count);

/**
 * cache_get_pending_pages - Get list of pages pending upload
 * 
 * @param cache: Cache handle
 * @param max_pages: Maximum number of pages to return
 * @return: Array of page entries (caller must free)
 * 
 * Returns a newly allocated array of PageEntry pointers.
 * The array is terminated by a NULL pointer.
 * Caller must free the array but not the PageEntry structures.
 */
PageEntry** cache_get_pending_pages(CacheHandle* cache, int max_pages);

/**
 * cache_count_by_status - Count pages by sync status
 * 
 * @param cache: Cache handle
 * @param status: Status to count
 * @return: Number of pages with given status
 */
int cache_count_by_status(CacheHandle* cache, sync_status_t status);

/**
 * cache_get_document_for_page - Find which document contains a page
 * 
 * @param cache: Cache handle
 * @param page_uuid: Page UUID to search for
 * @return: Document ID or NULL if not found
 */
const char* cache_get_document_for_page(CacheHandle* cache, const char* page_uuid);

int cache_reload(CacheHandle* cache);

#endif // CACHE_IO_H