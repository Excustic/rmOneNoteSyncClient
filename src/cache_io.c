// cache_io.c - Binary cache I/O implementation
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "cache_io.h"
#include <fcntl.h>
#include <sys/file.h>

#define HASH_TABLE_SIZE 256

/**
 * hash_string - Simple hash function for document IDs
 * 
 * @param str: String to hash
 * @return: Hash value for table indexing
 */
static unsigned int hash_string(const char* str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % HASH_TABLE_SIZE;
}

/**
 * cache_open - Open or create a cache file
 * 
 * @param path: Path to cache file
 * @return: Cache handle or NULL on error
 */
CacheHandle* cache_open(const char* path) {
    CacheHandle* cache = calloc(1, sizeof(CacheHandle));
    if (!cache) return NULL;
    
    // Initialize hash table
    cache->table = calloc(HASH_TABLE_SIZE, sizeof(DocumentEntry*));
    if (!cache->table) {
        free(cache);
        return NULL;
    }
    
    cache->table_size = HASH_TABLE_SIZE;
    strncpy(cache->path, path, PATH_MAX - 1);
    cache->path[PATH_MAX - 1] = '\0';
    cache->dirty = false;
    
    // Try to load existing cache
    FILE* f = fopen(path, "rb");
    if (!f) {
        // No existing cache, that's OK
        return cache;
    }
    
    // Read and verify header
    uint32_t magic, num_docs;
    uint8_t version;
    
    if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != CACHE_MAGIC) {
        fclose(f);
        return cache; // Invalid cache, start fresh
    }
    
    if (fread(&version, sizeof(version), 1, f) != 1) {
        fclose(f);
        return cache;
    }
    
    // Handle version differences
    if (version != CACHE_VERSION && version != 1) {
        fclose(f);
        return cache; // Incompatible version, start fresh
    }
    
    if (fread(&num_docs, sizeof(num_docs), 1, f) != 1) {
        fclose(f);
        return cache;
    }
    
    // Read documents
    for (uint32_t i = 0; i < num_docs; i++) {
        uint8_t doc_id_len;
        if (fread(&doc_id_len, sizeof(doc_id_len), 1, f) != 1) break;
        
        if (doc_id_len != UUID_LEN) break;
        
        DocumentEntry* doc = calloc(1, sizeof(DocumentEntry));
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
        
        // Read pages
        PageEntry* last_page = NULL;
        for (uint16_t j = 0; j < num_pages; j++) {
            PageEntry* page = calloc(1, sizeof(PageEntry));
            if (!page) break;
            
            if (fread(page->uuid, UUID_LEN, 1, f) != 1) {
                free(page);
                break;
            }
            page->uuid[UUID_LEN] = '\0';
            
            uint8_t page_num_len;
            if (fread(&page_num_len, sizeof(page_num_len), 1, f) != 1) {
                free(page);
                break;
            }
            
            if (page_num_len >= MAX_PAGE_NUM_LEN) {
                free(page);
                break;
            }
            
            if (page_num_len > 0) {
                if (fread(page->page_num, page_num_len, 1, f) != 1) {
                    free(page);
                    break;
                }
                page->page_num[page_num_len] = '\0';
            }
            
            if (fread(&page->mtime, sizeof(page->mtime), 1, f) != 1) {
                free(page);
                break;
            }
            
            // Read sync status fields if version 2
            if (version == CACHE_VERSION) {
                if (fread(&page->sync_status, sizeof(page->sync_status), 1, f) != 1 ||
                    fread(&page->retry_count, sizeof(page->retry_count), 1, f) != 1) {
                    free(page);
                    break;
                }
            } else {
                // Version 1: default to pending
                page->sync_status = SYNC_PENDING;
                page->retry_count = 0;
            }
            
            // Add to linked list
            if (last_page) {
                last_page->next = page;
            } else {
                doc->pages = page;
            }
            last_page = page;
        }
        
        // Add document to hash table
        unsigned int hash = hash_string(doc->doc_id);
        doc->next = cache->table[hash];
        cache->table[hash] = doc;
    }
    
    fclose(f);
    return cache;
}

/**
 * cache_close - Close cache and free resources
 * 
 * @param cache: Cache handle
 * @param save: Whether to save changes before closing
 */
void cache_close(CacheHandle* cache, bool save) {
    if (!cache) return;
    
    if (save && cache->dirty) {
        cache_save(cache);
    }
    
    // Free all documents and pages
    for (size_t i = 0; i < cache->table_size; i++) {
        DocumentEntry* doc = cache->table[i];
        while (doc) {
            DocumentEntry* next_doc = doc->next;
            
            // Free pages
            PageEntry* page = doc->pages;
            while (page) {
                PageEntry* next_page = page->next;
                free(page);
                page = next_page;
            }
            
            free(doc);
            doc = next_doc;
        }
    }
    
    free(cache->table);
    free(cache);
}


/**
 * cache_save_locked - Save cache with file locking
 * Fixed version of cache_save that uses file locking
 */
int cache_save(CacheHandle* cache) {
    if (!cache || !cache->dirty) return 0;

    // Write to temporary file first
    char temp_path[PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", cache->path);

    FILE* f = fopen(temp_path, "wb");
    if (!f) return -1;

    // Use exclusive lock for writing
    int fd = fileno(f);
    flock(fd, LOCK_EX);

    // Count documents
    uint32_t num_docs = 0;
    for (size_t i = 0; i < cache->table_size; i++) {
        for (DocumentEntry* doc = cache->table[i]; doc; doc = doc->next) {
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
    for (size_t i = 0; i < cache->table_size; i++) {
        for (DocumentEntry* doc = cache->table[i]; doc; doc = doc->next) {
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
                if (page_num_len > 0) {
                    fwrite(page->page_num, page_num_len, 1, f);
                }

                fwrite(&page->mtime, sizeof(page->mtime), 1, f);
                fwrite(&page->sync_status, sizeof(page->sync_status), 1, f);
                fwrite(&page->retry_count, sizeof(page->retry_count), 1, f);
            }
        }
    }

    flock(fd, LOCK_UN);  // Release lock
    fclose(f);

    // Atomic rename
    if (rename(temp_path, cache->path) != 0) {
        unlink(temp_path);
        return -1;
    }

    cache->dirty = false;
    return 0;
}

/**
 * cache_find_document - Find a document by ID
 * 
 * @param cache: Cache handle
 * @param doc_id: Document UUID
 * @return: Document entry or NULL if not found
 */
DocumentEntry* cache_find_document(CacheHandle* cache, const char* doc_id) {
    if (!cache || !doc_id) return NULL;
    
    unsigned int hash = hash_string(doc_id);
    DocumentEntry* doc = cache->table[hash];
    
    while (doc) {
        if (strcmp(doc->doc_id, doc_id) == 0) {
            return doc;
        }
        doc = doc->next;
    }
    
    return NULL;
}

/**
 * cache_find_page - Find a page within a document
 * 
 * @param doc: Document entry
 * @param page_uuid: Page UUID
 * @return: Page entry or NULL if not found
 */
PageEntry* cache_find_page(DocumentEntry* doc, const char* page_uuid) {
    if (!doc || !page_uuid) return NULL;
    
    PageEntry* page = doc->pages;
    while (page) {
        if (strcmp(page->uuid, page_uuid) == 0) {
            return page;
        }
        page = page->next;
    }
    
    return NULL;
}

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
                             sync_status_t status) {
    if (!cache || !doc_id || !page_uuid) return -1;
    
    // Find or create document
    DocumentEntry* doc = cache_find_document(cache, doc_id);
    if (!doc) {
        doc = calloc(1, sizeof(DocumentEntry));
        if (!doc) return -1;
        
        strncpy(doc->doc_id, doc_id, UUID_LEN);
        doc->doc_id[UUID_LEN] = '\0';
        
        // Add to hash table
        unsigned int hash = hash_string(doc_id);
        doc->next = cache->table[hash];
        cache->table[hash] = doc;
    }
    
    // Find or create page
    PageEntry* page = cache_find_page(doc, page_uuid);
    if (!page) {
        page = calloc(1, sizeof(PageEntry));
        if (!page) return -1;
        
        strncpy(page->uuid, page_uuid, UUID_LEN);
        page->uuid[UUID_LEN] = '\0';
        
        // Add to linked list
        page->next = doc->pages;
        doc->pages = page;
    }
    
    // Update page data
    if (page_num) {
        strncpy(page->page_num, page_num, MAX_PAGE_NUM_LEN - 1);
        page->page_num[MAX_PAGE_NUM_LEN - 1] = '\0';
    }
    page->mtime = mtime;
    page->sync_status = status;
    
    cache->dirty = true;
    return 0;
}

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
                             uint8_t retry_count) {
    if (!cache || !doc_id || !page_uuid) return -1;
    
    DocumentEntry* doc = cache_find_document(cache, doc_id);
    if (!doc) return -1;
    
    PageEntry* page = cache_find_page(doc, page_uuid);
    if (!page) return -1;
    
    page->sync_status = status;
    page->retry_count = retry_count;
    cache->dirty = true;
    
    return 0;
}

/**
 * cache_get_pending_pages - Get list of pages pending upload
 * 
 * @param cache: Cache handle
 * @param max_pages: Maximum number of pages to return
 * @return: Array of page entries (caller must free)
 */
PageEntry** cache_get_pending_pages(CacheHandle* cache, int max_pages) {
    if (!cache || max_pages <= 0) return NULL;
    
    // Allocate array for results
    PageEntry** results = calloc(max_pages + 1, sizeof(PageEntry*));
    if (!results) return NULL;
    
    int count = 0;
    
    // Search all documents
    for (size_t i = 0; i < cache->table_size && count < max_pages; i++) {
        for (DocumentEntry* doc = cache->table[i]; doc && count < max_pages; doc = doc->next) {
            for (PageEntry* page = doc->pages; page && count < max_pages; page = page->next) {
                if (page->sync_status == SYNC_PENDING) {
                    results[count++] = page;
                }
            }
        }
    }
    
    return results;
}

/**
 * cache_count_by_status - Count pages by sync status
 * 
 * @param cache: Cache handle
 * @param status: Status to count
 * @return: Number of pages with given status
 */
int cache_count_by_status(CacheHandle* cache, sync_status_t status) {
    if (!cache) return 0;
    
    int count = 0;
    
    for (size_t i = 0; i < cache->table_size; i++) {
        for (DocumentEntry* doc = cache->table[i]; doc; doc = doc->next) {
            for (PageEntry* page = doc->pages; page; page = page->next) {
                if (page->sync_status == status) {
                    count++;
                }
            }
        }
    }
    
    return count;
}

/**
 * cache_get_document_for_page - Find which document contains a page
 * 
 * @param cache: Cache handle
 * @param page_uuid: Page UUID to search for
 * @return: Document ID or NULL if not found
 */
const char* cache_get_document_for_page(CacheHandle* cache, const char* page_uuid) {
    if (!cache || !page_uuid) return NULL;
    
    for (size_t i = 0; i < cache->table_size; i++) {
        for (DocumentEntry* doc = cache->table[i]; doc; doc = doc->next) {
            for (PageEntry* page = doc->pages; page; page = page->next) {
                if (strcmp(page->uuid, page_uuid) == 0) {
                    return doc->doc_id;
                }
            }
        }
    }
    
    return NULL;
}

/**
 * cache_reload - Reload cache from disk to get latest changes
 *
 * @param cache: Cache handle
 * @return: 0 on success, -1 on error
 *
 * This function clears the current in-memory cache and reloads from disk.
 * Used to synchronize between watcher and httpclient processes.
 */
int cache_reload(CacheHandle* cache) {
    if (!cache) return -1;

    // Clear existing cache entries
    for (size_t i = 0; i < cache->table_size; i++) {
        DocumentEntry* doc = cache->table[i];
        while (doc) {
            DocumentEntry* next_doc = doc->next;

            // Free pages
            PageEntry* page = doc->pages;
            while (page) {
                PageEntry* next_page = page->next;
                free(page);
                page = next_page;
            }

            free(doc);
            doc = next_doc;
        }
        cache->table[i] = NULL;
    }

    // Reload from file
    FILE* f = fopen(cache->path, "rb");
    if (!f) {
        // No file, that's OK
        cache->dirty = false;
        return 0;
    }

    // Use file locking to ensure we don't read while another process is writing
    int fd = fileno(f);
    flock(fd, LOCK_SH);  // Shared lock for reading

    // Read and verify header
    uint32_t magic, num_docs;
    uint8_t version;

    if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != CACHE_MAGIC) {
        flock(fd, LOCK_UN);
        fclose(f);
        return -1;
    }

    if (fread(&version, sizeof(version), 1, f) != 1) {
        flock(fd, LOCK_UN);
        fclose(f);
        return -1;
    }

    if (version != CACHE_VERSION && version != 1) {
        flock(fd, LOCK_UN);
        fclose(f);
        return -1;
    }

    if (fread(&num_docs, sizeof(num_docs), 1, f) != 1) {
        flock(fd, LOCK_UN);
        fclose(f);
        return -1;
    }

    // Read documents
    for (uint32_t i = 0; i < num_docs; i++) {
        uint8_t doc_id_len;
        if (fread(&doc_id_len, sizeof(doc_id_len), 1, f) != 1) break;

        if (doc_id_len != UUID_LEN) break;

        DocumentEntry* doc = calloc(1, sizeof(DocumentEntry));
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

        // Read pages
        PageEntry* last_page = NULL;
        for (uint16_t j = 0; j < num_pages; j++) {
            PageEntry* page = calloc(1, sizeof(PageEntry));
            if (!page) break;

            if (fread(page->uuid, UUID_LEN, 1, f) != 1) {
                free(page);
                break;
            }
            page->uuid[UUID_LEN] = '\0';

            uint8_t page_num_len;
            if (fread(&page_num_len, sizeof(page_num_len), 1, f) != 1) {
                free(page);
                break;
            }

            if (page_num_len >= MAX_PAGE_NUM_LEN) {
                free(page);
                break;
            }

            if (page_num_len > 0) {
                if (fread(page->page_num, page_num_len, 1, f) != 1) {
                    free(page);
                    break;
                }
                page->page_num[page_num_len] = '\0';
            }

            if (fread(&page->mtime, sizeof(page->mtime), 1, f) != 1) {
                free(page);
                break;
            }

            // Read sync status fields if version 2
            if (version == CACHE_VERSION) {
                if (fread(&page->sync_status, sizeof(page->sync_status), 1, f) != 1 ||
                    fread(&page->retry_count, sizeof(page->retry_count), 1, f) != 1) {
                    free(page);
                    break;
                }
            }
            else {
                page->sync_status = SYNC_PENDING;
                page->retry_count = 0;
            }

            // Add to linked list
            if (last_page) {
                last_page->next = page;
            }
            else {
                doc->pages = page;
            }
            last_page = page;
        }

        // Add document to hash table
        unsigned int hash = hash_string(doc->doc_id);
        doc->next = cache->table[hash];
        cache->table[hash] = doc;
    }

    flock(fd, LOCK_UN);  // Release lock
    fclose(f);

    cache->dirty = false;
    return 0;
}