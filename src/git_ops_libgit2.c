#include "git_vid_codec.h"
#include <git2.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

// libgit2 repository handle
static git_repository* repo = NULL;
static pthread_mutex_t repo_mutex = PTHREAD_MUTEX_INITIALIZER;

// Blob prefetch cache
#define PREFETCH_CACHE_SIZE 32
typedef struct {
    char oid_str[GIT_OID_HEXSZ + 1];
    git_blob* blob;
    int valid;
} blob_cache_entry_t;

static blob_cache_entry_t blob_cache[PREFETCH_CACHE_SIZE];
static int cache_write_pos = 0;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

// Background prefetch thread
static pthread_t prefetch_thread;
static volatile int prefetch_running = 0;
static char** prefetch_queue = NULL;
static int prefetch_queue_size = 0;
static int prefetch_queue_pos = 0;
static pthread_mutex_t prefetch_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t prefetch_cond = PTHREAD_COND_INITIALIZER;

// Initialize libgit2 and open repository
int git_init_libgit2(const char* repo_path) {
    // Initialize libgit2
    int error = git_libgit2_init();
    if (error < 0) {
        const git_error* e = git_error_last();
        fprintf(stderr, "Failed to initialize libgit2: %s\n", e ? e->message : "Unknown error");
        return GVC_ERROR_GIT;
    }
    
    // Open repository
    error = git_repository_open(&repo, repo_path);
    if (error < 0) {
        const git_error* e = git_error_last();
        fprintf(stderr, "Failed to open repository: %s\n", e ? e->message : "Unknown error");
        git_libgit2_shutdown();
        return GVC_ERROR_GIT;
    }
    
    // Initialize blob cache
    memset(blob_cache, 0, sizeof(blob_cache));
    
    printf("libgit2 repository opened: %s\n", repo_path);
    return GVC_SUCCESS;
}

// Find blob in cache
static git_blob* find_blob_in_cache(const char* oid_str) {
    pthread_mutex_lock(&cache_mutex);
    
    for (int i = 0; i < PREFETCH_CACHE_SIZE; i++) {
        if (blob_cache[i].valid && strcmp(blob_cache[i].oid_str, oid_str) == 0) {
            git_blob* blob = blob_cache[i].blob;
            // Mark as used (don't remove yet)
            pthread_mutex_unlock(&cache_mutex);
            return blob;
        }
    }
    
    pthread_mutex_unlock(&cache_mutex);
    return NULL;
}

// Add blob to cache
static void add_blob_to_cache(const char* oid_str, git_blob* blob) {
    pthread_mutex_lock(&cache_mutex);
    
    // Find empty slot or replace oldest
    int slot = cache_write_pos;
    
    // Free existing blob if slot is occupied
    if (blob_cache[slot].valid && blob_cache[slot].blob) {
        git_blob_free(blob_cache[slot].blob);
    }
    
    // Store new blob
    strncpy(blob_cache[slot].oid_str, oid_str, GIT_OID_HEXSZ);
    blob_cache[slot].oid_str[GIT_OID_HEXSZ] = '\0';
    blob_cache[slot].blob = blob;
    blob_cache[slot].valid = 1;
    
    cache_write_pos = (cache_write_pos + 1) % PREFETCH_CACHE_SIZE;
    
    pthread_mutex_unlock(&cache_mutex);
}

// Background prefetch worker
static void* prefetch_worker(void* arg) {
    (void)arg;
    
    while (prefetch_running) {
        pthread_mutex_lock(&prefetch_mutex);
        
        // Wait for work
        while (prefetch_running && prefetch_queue_pos >= prefetch_queue_size) {
            pthread_cond_wait(&prefetch_cond, &prefetch_mutex);
        }
        
        if (!prefetch_running) {
            pthread_mutex_unlock(&prefetch_mutex);
            break;
        }
        
        // Get next OID to prefetch
        char* oid_str = prefetch_queue[prefetch_queue_pos++];
        pthread_mutex_unlock(&prefetch_mutex);
        
        if (!oid_str) continue;
        
        // Check if already in cache
        if (find_blob_in_cache(oid_str)) {
            continue;
        }
        
        // Fetch blob
        git_oid oid;
        if (git_oid_fromstr(&oid, oid_str) == 0) {
            git_blob* blob;
            pthread_mutex_lock(&repo_mutex);
            int error = git_blob_lookup(&blob, repo, &oid);
            pthread_mutex_unlock(&repo_mutex);
            
            if (error == 0) {
                add_blob_to_cache(oid_str, blob);
            }
        }
    }
    
    return NULL;
}

// Start prefetch thread with commit list
int git_start_prefetch(char** commit_hashes, int num_commits) {
    if (prefetch_running) {
        return GVC_SUCCESS; // Already running
    }
    
    // Setup prefetch queue
    pthread_mutex_lock(&prefetch_mutex);
    prefetch_queue = commit_hashes;
    prefetch_queue_size = num_commits;
    prefetch_queue_pos = 0;
    prefetch_running = 1;
    pthread_mutex_unlock(&prefetch_mutex);
    
    // Start prefetch thread
    int result = pthread_create(&prefetch_thread, NULL, prefetch_worker, NULL);
    if (result != 0) {
        prefetch_running = 0;
        fprintf(stderr, "Failed to create prefetch thread\n");
        return GVC_ERROR_THREAD;
    }
    
    printf("Prefetch thread started for %d commits\n", num_commits);
    return GVC_SUCCESS;
}

// Stop prefetch thread
void git_stop_prefetch(void) {
    if (!prefetch_running) {
        return;
    }
    
    pthread_mutex_lock(&prefetch_mutex);
    prefetch_running = 0;
    pthread_cond_broadcast(&prefetch_cond);
    pthread_mutex_unlock(&prefetch_mutex);
    
    pthread_join(prefetch_thread, NULL);
    printf("Prefetch thread stopped\n");
}

// High-performance blob read using libgit2
int git_read_blob_libgit2(const char* commit_hash, uint8_t** data_out, size_t* size_out) {
    if (!repo) {
        fprintf(stderr, "Repository not initialized\n");
        return GVC_ERROR_GIT;
    }
    
    // Try cache first
    git_blob* cached_blob = find_blob_in_cache(commit_hash);
    if (cached_blob) {
        const void* blob_data = git_blob_rawcontent(cached_blob);
        size_t blob_size = git_blob_rawsize(cached_blob);
        
        *data_out = malloc(blob_size);
        if (!*data_out) {
            return GVC_ERROR_MEMORY;
        }
        
        memcpy(*data_out, blob_data, blob_size);
        *size_out = blob_size;
        return GVC_SUCCESS;
    }
    
    // Parse commit hash to OID
    git_oid commit_oid;
    int error = git_oid_fromstr(&commit_oid, commit_hash);
    if (error < 0) {
        const git_error* e = git_error_last();
        fprintf(stderr, "Invalid commit OID '%s': %s\n", commit_hash, e ? e->message : "Unknown error");
        return GVC_ERROR_GIT;
    }
    
    pthread_mutex_lock(&repo_mutex);
    
    // Look up the commit
    git_commit* commit;
    error = git_commit_lookup(&commit, repo, &commit_oid);
    if (error < 0) {
        pthread_mutex_unlock(&repo_mutex);
        const git_error* e = git_error_last();
        fprintf(stderr, "Failed to lookup commit '%s': %s\n", commit_hash, e ? e->message : "Unknown error");
        return GVC_ERROR_GIT;
    }
    
    // Get the tree from the commit
    git_tree* tree;
    error = git_commit_tree(&tree, commit);
    if (error < 0) {
        git_commit_free(commit);
        pthread_mutex_unlock(&repo_mutex);
        const git_error* e = git_error_last();
        fprintf(stderr, "Failed to get tree from commit '%s': %s\n", commit_hash, e ? e->message : "Unknown error");
        return GVC_ERROR_GIT;
    }
    
    // Look up the "frame.bin" entry in the tree
    const git_tree_entry* entry;
    entry = git_tree_entry_byname(tree, "frame.bin");
    if (!entry) {
        git_tree_free(tree);
        git_commit_free(commit);
        pthread_mutex_unlock(&repo_mutex);
        fprintf(stderr, "No 'frame.bin' found in commit '%s'\n", commit_hash);
        return GVC_ERROR_GIT;
    }
    
    // Get the blob OID from the tree entry
    const git_oid* blob_oid = git_tree_entry_id(entry);
    
    // Look up the blob
    git_blob* blob;
    error = git_blob_lookup(&blob, repo, blob_oid);
    
    git_tree_free(tree);
    git_commit_free(commit);
    pthread_mutex_unlock(&repo_mutex);
    
    if (error < 0) {
        const git_error* e = git_error_last();
        fprintf(stderr, "Failed to lookup blob from commit '%s': %s\n", commit_hash, e ? e->message : "Unknown error");
        return GVC_ERROR_GIT;
    }
    
    const void* blob_data = git_blob_rawcontent(blob);
    size_t blob_size = git_blob_rawsize(blob);
    
    *data_out = malloc(blob_size);
    if (!*data_out) {
        git_blob_free(blob);
        return GVC_ERROR_MEMORY;
    }
    
    memcpy(*data_out, blob_data, blob_size);
    *size_out = blob_size;
    
    // Add to cache for future use
    add_blob_to_cache(commit_hash, blob);
    
    return GVC_SUCCESS;
}

// Get commit chain using libgit2 (faster than git log)
int git_get_commit_chain_libgit2(char*** commit_hashes_out, int* num_commits_out) {
    if (!repo) {
        fprintf(stderr, "Repository not initialized\n");
        return GVC_ERROR_GIT;
    }
    
    git_revwalk* walker;
    int error = git_revwalk_new(&walker, repo);
    if (error < 0) {
        const git_error* e = git_error_last();
        fprintf(stderr, "Failed to create revwalk: %s\n", e ? e->message : "Unknown error");
        return GVC_ERROR_GIT;
    }
    
    // Start from HEAD
    error = git_revwalk_push_head(walker);
    if (error < 0) {
        const git_error* e = git_error_last();
        fprintf(stderr, "Failed to push HEAD: %s\n", e ? e->message : "Unknown error");
        git_revwalk_free(walker);
        return GVC_ERROR_GIT;
    }
    
    // Set sorting to reverse chronological order (oldest first)
    git_revwalk_sorting(walker, GIT_SORT_REVERSE);
    
    // Collect commit hashes
    char** hashes = malloc(sizeof(char*) * 1000); // Initial capacity
    int capacity = 1000;
    int count = 0;
    
    git_oid oid;
    while (git_revwalk_next(&oid, walker) == 0) {
        if (count >= capacity) {
            capacity *= 2;
            hashes = realloc(hashes, sizeof(char*) * capacity);
            if (!hashes) {
                git_revwalk_free(walker);
                return GVC_ERROR_MEMORY;
            }
        }
        
        hashes[count] = malloc(GIT_OID_HEXSZ + 1);
        git_oid_tostr(hashes[count], GIT_OID_HEXSZ + 1, &oid);
        count++;
    }
    
    git_revwalk_free(walker);
    
    *commit_hashes_out = hashes;
    *num_commits_out = count;
    
    printf("Found %d commits using libgit2\n", count);
    return GVC_SUCCESS;
}

// Cleanup libgit2 resources
void git_cleanup_libgit2(void) {
    // Stop prefetch thread
    git_stop_prefetch();
    
    // Clean up blob cache
    pthread_mutex_lock(&cache_mutex);
    for (int i = 0; i < PREFETCH_CACHE_SIZE; i++) {
        if (blob_cache[i].valid && blob_cache[i].blob) {
            git_blob_free(blob_cache[i].blob);
            blob_cache[i].valid = 0;
        }
    }
    pthread_mutex_unlock(&cache_mutex);
    
    // Close repository
    if (repo) {
        git_repository_free(repo);
        repo = NULL;
    }
    
    // Shutdown libgit2
    git_libgit2_shutdown();
    
    printf("libgit2 cleanup complete\n");
}

// Compatibility wrapper for existing git_show function
int git_show(const char* commit_hash, uint8_t** data_out, size_t* size_out) {
    return git_read_blob_libgit2(commit_hash, data_out, size_out);
}