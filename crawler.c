/*
 * Multi-threaded Web Crawler Library Implementation
 * 
 * SYNCHRONIZATION DESIGN:
 * - Single global mutex protects all shared state for simplicity and correctness
 * - Four condition variables for different wait conditions:
 *   1. links_available: Signals downloaders when links queue has work
 *   2. links_space: Signals parsers when links queue has space
 *   3. pages_available: Signals parsers when pages queue has work
 *   4. state_changed: Signals main thread when crawler state changes
 * 
 * PRODUCER-CONSUMER ARCHITECTURE:
 * 1. Links Queue (bounded): Parsers produce URLs → Downloaders consume URLs
 *    - Bounded to control memory usage (work_queue_size parameter)
 *    - Parsers block when full, downloaders block when empty
 * 
 * 2. Pages Queue (unbounded): Downloaders produce HTML → Parsers consume HTML
 *    - Unbounded to prevent deadlock situations
 *    - Only parsers block when empty (downloaders never block on full)
 * 
 * THREAD LIFECYCLE:
 * - Workers loop: wait for work → dequeue → increment active counter → 
 *   do work (outside lock) → enqueue results → decrement active counter
 * - Active counters only include threads actively doing I/O or parsing
 * - Threads waiting for work are NOT counted as active
 * 
 * COMPLETION DETECTION:
 * - Crawler completes when: all queues empty AND no active workers
 * - Workers signal state_changed after completing work
 * - Main thread re-checks completion condition on each signal
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include "../os-crawler-framework/api.h"

// ============================================================================
// DATA STRUCTURES
// ============================================================================

// Node for linked list queue
typedef struct queue_node {
    void *data;
    struct queue_node *next;
} queue_node_t;

// Thread-safe queue (can be bounded or unbounded)
typedef struct {
    queue_node_t *head;
    queue_node_t *tail;
    int size;
    int capacity;  // -1 for unbounded
} queue_t;

// Hash set node for tracking visited URLs
typedef struct hash_node {
    char *url;
    struct hash_node *next;
} hash_node_t;

// Simple hash set
typedef struct {
    hash_node_t **buckets;
    int num_buckets;
} hash_set_t;

// Item in links queue (bounded)
typedef struct {
    char *url;
    char *parent_url;
} link_item_t;

// Item in pages queue (unbounded)
typedef struct {
    char *url;
    char *content;
} page_item_t;

// Main crawler state - SINGLE MUTEX DESIGN
typedef struct {
    // Thread pools
    pthread_t *download_workers;
    pthread_t *parse_workers;
    int num_download_workers;
    int num_parse_workers;
    
    // Producer-Consumer Queues
    queue_t *links_queue;      // Bounded: Parsers -> Downloaders
    queue_t *pages_queue;      // Unbounded: Downloaders -> Parsers
    
    // SINGLE GLOBAL MUTEX protects ALL state
    pthread_mutex_t mutex;
    
    // Condition variables for different wait conditions
    pthread_cond_t links_available;    // Downloaders wait here for links
    pthread_cond_t links_space;        // Parsers wait here when links queue full
    pthread_cond_t pages_available;    // Parsers wait here for pages
    pthread_cond_t state_changed;      // Main thread waits for completion
    
    // Tracking
    hash_set_t *visited_urls;
    hash_set_t *queued_urls;
    
    // State counters
    int active_downloads;      // Downloaders currently doing I/O
    int active_parsers;        // Parsers currently processing
    bool shutdown;
    
    // Callbacks
    char *(*fetch_fn)(const char *);
    void (*edge_fn)(const char *, const char *);
    
    // DEBUG: Thread statistics
    int *download_thread_work_count;  // Work done by each downloader
    int *parse_thread_work_count;     // Work done by each parser
} crawler_state_t;

// ============================================================================
// QUEUE OPERATIONS
// NOTE: These functions are NOT thread-safe. Caller must hold mutex.
// ============================================================================

// Create a new queue with given capacity (-1 for unbounded)
queue_t *queue_create(int capacity) {
    queue_t *q = malloc(sizeof(queue_t));
    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
    q->capacity = capacity;
    return q;
}

bool queue_is_empty(queue_t *q) {
    return q->size == 0;
}

bool queue_is_full(queue_t *q) {
    if (q->capacity == -1) return false;
    return q->size >= q->capacity;
}

void queue_enqueue(queue_t *q, void *data) {
    queue_node_t *node = malloc(sizeof(queue_node_t));
    node->data = data;
    node->next = NULL;
    
    if (q->tail == NULL) {
        q->head = q->tail = node;
    } else {
        q->tail->next = node;
        q->tail = node;
    }
    q->size++;
}

void *queue_dequeue(queue_t *q) {
    if (q->head == NULL) return NULL;
    
    queue_node_t *node = q->head;
    void *data = node->data;
    q->head = node->next;
    
    if (q->head == NULL) {
        q->tail = NULL;
    }
    
    q->size--;
    free(node);
    return data;
}

void queue_destroy(queue_t *q) {
    while (q->head != NULL) {
        queue_node_t *node = q->head;
        q->head = node->next;
        free(node->data);
        free(node);
    }
    free(q);
}

// ============================================================================
// HASH SET OPERATIONS
// NOTE: These functions are NOT thread-safe. Caller must hold mutex.
// ============================================================================

#define HASH_BUCKETS 1024

// Simple hash function (djb2 algorithm)
unsigned int hash_string(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % HASH_BUCKETS;
}

hash_set_t *hash_set_create() {
    hash_set_t *set = malloc(sizeof(hash_set_t));
    set->num_buckets = HASH_BUCKETS;
    set->buckets = calloc(HASH_BUCKETS, sizeof(hash_node_t *));
    return set;
}

bool hash_set_contains(hash_set_t *set, const char *url) {
    unsigned int bucket = hash_string(url);
    hash_node_t *node = set->buckets[bucket];
    
    while (node != NULL) {
        if (strcmp(node->url, url) == 0) {
            return true;
        }
        node = node->next;
    }
    return false;
}

void hash_set_add(hash_set_t *set, const char *url) {
    if (hash_set_contains(set, url)) return;
    
    unsigned int bucket = hash_string(url);
    hash_node_t *node = malloc(sizeof(hash_node_t));
    node->url = strdup(url);
    node->next = set->buckets[bucket];
    set->buckets[bucket] = node;
}

void hash_set_remove(hash_set_t *set, const char *url) {
    unsigned int bucket = hash_string(url);
    hash_node_t *node = set->buckets[bucket];
    hash_node_t *prev = NULL;
    
    while (node != NULL) {
        if (strcmp(node->url, url) == 0) {
            if (prev == NULL) {
                set->buckets[bucket] = node->next;
            } else {
                prev->next = node->next;
            }
            free(node->url);
            free(node);
            return;
        }
        prev = node;
        node = node->next;
    }
}

void hash_set_destroy(hash_set_t *set) {
    for (int i = 0; i < set->num_buckets; i++) {
        hash_node_t *node = set->buckets[i];
        while (node != NULL) {
            hash_node_t *next = node->next;
            free(node->url);
            free(node);
            node = next;
        }
    }
    free(set->buckets);
    free(set);
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Get the index of the calling thread in the thread pool array
// Returns -1 if not found (shouldn't happen in normal operation)
int get_thread_index(pthread_t *threads, int count, pthread_t self) {
    for (int i = 0; i < count; i++) {
        if (pthread_equal(threads[i], self)) {
            return i;
        }
    }
    return -1;
}

// Check if the crawl is complete
// Must be called while holding the global mutex
// Completion means: no work in queues AND no workers actively processing
bool is_crawl_complete(crawler_state_t *state) {
    return queue_is_empty(state->links_queue) &&
           queue_is_empty(state->pages_queue) &&
           state->active_downloads == 0 &&
           state->active_parsers == 0;
}

// Extract all links from HTML content
// Links are in format: <a href="filename">filename</a>
// Returns array of link strings and sets count
// Caller must free returned array and all strings in it
char **extract_links(const char *html, int *count) {
    *count = 0;
    
    // Count links first
    const char *ptr = html;
    while ((ptr = strstr(ptr, "<a href=\"")) != NULL) {
        (*count)++;
        ptr += 9;
    }
    
    if (*count == 0) return NULL;
    
    // Allocate array for links
    char **links = malloc(*count * sizeof(char *));
    int idx = 0;
    
    // Extract each link
    ptr = html;
    while ((ptr = strstr(ptr, "<a href=\"")) != NULL) {
        ptr += 9;
        
        const char *end = strchr(ptr, '"');
        if (end == NULL) break;
        
        size_t len = end - ptr;
        links[idx] = malloc(len + 1);
        strncpy(links[idx], ptr, len);
        links[idx][len] = '\0';
        
        idx++;
        ptr = end + 1;
    }
    
    return links;
}

// ============================================================================
// WORKER THREADS
// ============================================================================

/*
 * Download Worker Thread
 * 
 * Role: CONSUMER of links (from parsers), PRODUCER of pages (for parsers)
 * 
 * Algorithm:
 * 1. Wait for a URL from links_queue (blocks if empty)
 * 2. Dequeue URL and increment active_downloads
 * 3. Release mutex and fetch content via fetch_fn callback (slow I/O)
 * 4. Re-acquire mutex and enqueue page to pages_queue
 * 5. Decrement active_downloads and signal state change
 * 6. Repeat until shutdown
 * 
 * Notes:
 * - Mutex held only during queue operations and counter updates
 * - I/O happens outside critical section for maximum parallelism
 * - Signals links_space when consuming (helps blocked parsers)
 * - Signals pages_available when producing (wakes blocked parsers)
 */
void *download_worker_thread(void *arg) {
    crawler_state_t *state = (crawler_state_t *)arg;
    pthread_t self = pthread_self();
    int thread_id = -1;
    
    while (true) {
        link_item_t *link_item = NULL;
        
        // === CRITICAL SECTION: Get work ===
        pthread_mutex_lock(&state->mutex);
        
        // Lazy initialize thread_id
        if (thread_id == -1) {
            thread_id = get_thread_index(state->download_workers, state->num_download_workers, self);
        }
        
        // Wait for work or shutdown
        while (queue_is_empty(state->links_queue) && !state->shutdown) {
            pthread_cond_wait(&state->links_available, &state->mutex);
        }
        
        // Check shutdown
        if (state->shutdown && queue_is_empty(state->links_queue)) {
            pthread_mutex_unlock(&state->mutex);
            break;
        }
        
        // Get work
        link_item = queue_dequeue(state->links_queue);
        state->active_downloads++;
        state->download_thread_work_count[thread_id]++;
        
        fprintf(stderr, "[D%d] Dequeued link: %s (links_queue: %d items)\n", 
                thread_id, link_item->url, state->links_queue->size);
        
        // Signal that space is available in links queue
        pthread_cond_signal(&state->links_space);
        
        pthread_mutex_unlock(&state->mutex);
        // === END CRITICAL SECTION ===
        
        // Do I/O outside the lock
        fprintf(stderr, "[D%d] Fetching: %s\n", thread_id, link_item->url);
        char *content = state->fetch_fn(link_item->url);
        
        // === CRITICAL SECTION: Deliver result ===
        pthread_mutex_lock(&state->mutex);
        
        if (content != NULL) {
            // Create page item and add to queue
            page_item_t *page_item = malloc(sizeof(page_item_t));
            page_item->url = strdup(link_item->url);
            page_item->content = content;
            
            queue_enqueue(state->pages_queue, page_item);
            fprintf(stderr, "[D%d] Enqueued page: %s (pages_queue: %d items)\n",
                    thread_id, page_item->url, state->pages_queue->size);
            pthread_cond_signal(&state->pages_available);
        } else {
            fprintf(stderr, "[D%d] Broken link: %s\n", thread_id, link_item->url);
        }
        
        // Mark this download as complete
        state->active_downloads--;
        
        // Signal potential completion
        pthread_cond_broadcast(&state->state_changed);
        
        pthread_mutex_unlock(&state->mutex);
        // === END CRITICAL SECTION ===
        
        // Cleanup
        free(link_item->url);
        if (link_item->parent_url) {
            free(link_item->parent_url);
        }
        free(link_item);
    }
    
    fprintf(stderr, "[D%d] Thread exiting (processed %d URLs)\n", 
            thread_id, state->download_thread_work_count[thread_id]);
    return NULL;
}

/*
 * Parse Worker Thread
 * 
 * Role: CONSUMER of pages (from downloaders), PRODUCER of links (for downloaders)
 * 
 * Algorithm:
 * 1. Wait for a page from pages_queue (blocks if empty)
 * 2. Dequeue page and increment active_parsers
 * 3. Release mutex and parse HTML to extract links (CPU work)
 * 4. Re-acquire mutex and process each discovered link:
 *    a. Call edge_fn callback to report graph edge
 *    b. Check if URL already visited or queued
 *    c. If new: add to queued_urls set and enqueue to links_queue
 *       (may block if links_queue is full - bounded queue)
 * 5. Mark current URL as visited
 * 6. Decrement active_parsers and signal state change
 * 7. Repeat until shutdown
 * 
 * Notes:
 * - Parsing happens outside critical section
 * - May block when links_queue is full (producer-consumer flow control)
 * - Signals links_available when producing (wakes blocked downloaders)
 */
void *parse_worker_thread(void *arg) {
    crawler_state_t *state = (crawler_state_t *)arg;
    pthread_t self = pthread_self();
    int thread_id = -1;
    
    while (true) {
        page_item_t *page_item = NULL;
        
        // === CRITICAL SECTION: Get work ===
        pthread_mutex_lock(&state->mutex);
        
        // Lazy initialize thread_id
        if (thread_id == -1) {
            thread_id = get_thread_index(state->parse_workers, state->num_parse_workers, self);
        }
        
        // Wait for work or shutdown
        while (queue_is_empty(state->pages_queue) && !state->shutdown) {
            pthread_cond_wait(&state->pages_available, &state->mutex);
        }
        
        // Check shutdown
        if (state->shutdown && queue_is_empty(state->pages_queue)) {
            pthread_mutex_unlock(&state->mutex);
            break;
        }
        
        // Get work
        page_item = queue_dequeue(state->pages_queue);
        state->active_parsers++;
        state->parse_thread_work_count[thread_id]++;
        
        fprintf(stderr, "[P%d] Dequeued page: %s (pages_queue: %d items)\n",
                thread_id, page_item->url, state->pages_queue->size);
        
        pthread_mutex_unlock(&state->mutex);
        // === END CRITICAL SECTION ===
        
        // Do parsing outside the lock
        fprintf(stderr, "[P%d] Parsing: %s\n", thread_id, page_item->url);
        int link_count;
        char **links = extract_links(page_item->content, &link_count);
        fprintf(stderr, "[P%d] Found %d links in %s\n", thread_id, link_count, page_item->url);
        
        // === CRITICAL SECTION: Process discovered links ===
        pthread_mutex_lock(&state->mutex);
        
        int new_links_added = 0;
        
        // Process each discovered link
        for (int i = 0; i < link_count; i++) {
            // Call edge callback (release mutex during callback)
            // This reports the graph structure to the caller
            pthread_mutex_unlock(&state->mutex);
            state->edge_fn(page_item->url, links[i]);
            pthread_mutex_lock(&state->mutex);
            
            // Check if we should visit this link
            bool should_visit = !hash_set_contains(state->visited_urls, links[i]) &&
                               !hash_set_contains(state->queued_urls, links[i]);
            
            if (should_visit) {
                hash_set_add(state->queued_urls, links[i]);
                
                // Wait if links queue is full
                while (queue_is_full(state->links_queue) && !state->shutdown) {
                    fprintf(stderr, "[P%d] Links queue full, waiting...\n", thread_id);
                    pthread_cond_wait(&state->links_space, &state->mutex);
                }
                
                if (!state->shutdown) {
                    // Add to links queue
                    link_item_t *new_link = malloc(sizeof(link_item_t));
                    new_link->url = strdup(links[i]);
                    new_link->parent_url = strdup(page_item->url);
                    
                    queue_enqueue(state->links_queue, new_link);
                    new_links_added++;
                    pthread_cond_signal(&state->links_available);
                }
            }
        }
        
        fprintf(stderr, "[P%d] Enqueued %d new links (links_queue: %d items)\n",
                thread_id, new_links_added, state->links_queue->size);
        
        // Mark URL as visited
        hash_set_add(state->visited_urls, page_item->url);
        hash_set_remove(state->queued_urls, page_item->url);
        
        // Mark this parse as complete
        state->active_parsers--;
        
        // Signal potential completion
        pthread_cond_broadcast(&state->state_changed);
        
        pthread_mutex_unlock(&state->mutex);
        // === END CRITICAL SECTION ===
        
        // Cleanup
        for (int i = 0; i < link_count; i++) {
            free(links[i]);
        }
        free(links);
        free(page_item->url);
        free(page_item->content);
        free(page_item);
    }
    
    fprintf(stderr, "[P%d] Thread exiting (processed %d pages)\n",
            thread_id, state->parse_thread_work_count[thread_id]);
    return NULL;
}

// ============================================================================
// MAIN CRAWL FUNCTION (Library API)
// ============================================================================

/*
 * crawl - Multi-threaded web crawler
 * 
 * This is the main library entry point. It creates thread pools, initializes
 * synchronization primitives, seeds the crawler with start_url, and waits
 * for completion before cleaning up.
 * 
 * Parameters:
 *   start_url: Initial URL to crawl (typically "root" for tests)
 *   download_workers: Number of downloader threads
 *   parse_workers: Number of parser threads  
 *   work_queue_size: Maximum size of bounded links queue
 *   fetch_fn: Callback to fetch content for a URL (returns malloc'd string)
 *   edge_fn: Callback to report discovered edges in the graph
 * 
 * Returns:
 *   0 on success, -1 on error
 * 
 * Thread Safety:
 *   This function creates and manages all threads internally. It blocks
 *   until the crawl is complete before returning.
 */
int crawl(
    const char *start_url,
    int download_workers,
    int parse_workers,
    int work_queue_size,
    char *(*fetch_fn)(const char *link),
    void (*edge_fn)(const char *from, const char *to))
{
    // Validate parameters
    if (!start_url || !fetch_fn || !edge_fn) {
        return -1;
    }
    
    if (download_workers <= 0 || parse_workers <= 0 || work_queue_size <= 0) {
        return -1;
    }
    
    // Initialize state
    crawler_state_t *state = malloc(sizeof(crawler_state_t));
    if (!state) return -1;
    
    state->num_download_workers = download_workers;
    state->num_parse_workers = parse_workers;
    state->fetch_fn = fetch_fn;
    state->edge_fn = edge_fn;
    
    // Create queues
    state->links_queue = queue_create(work_queue_size);
    state->pages_queue = queue_create(-1);
    
    // Initialize single mutex and condition variables
    pthread_mutex_init(&state->mutex, NULL);
    pthread_cond_init(&state->links_available, NULL);
    pthread_cond_init(&state->links_space, NULL);
    pthread_cond_init(&state->pages_available, NULL);
    pthread_cond_init(&state->state_changed, NULL);
    
    // Initialize tracking structures
    state->visited_urls = hash_set_create();
    state->queued_urls = hash_set_create();
    
    // Initialize state
    state->active_downloads = 0;
    state->active_parsers = 0;
    state->shutdown = false;
    
    // Initialize statistics tracking
    state->download_thread_work_count = calloc(download_workers, sizeof(int));
    state->parse_thread_work_count = calloc(parse_workers, sizeof(int));
    
    // Print configuration
    fprintf(stderr, "\n=== CRAWLER CONFIGURATION ===\n");
    fprintf(stderr, "Download workers: %d\n", download_workers);
    fprintf(stderr, "Parse workers: %d\n", parse_workers);
    fprintf(stderr, "Links queue size: %d (bounded)\n", work_queue_size);
    fprintf(stderr, "Pages queue size: unlimited (unbounded)\n");
    fprintf(stderr, "============================\n\n");
    
    // Create worker threads
    state->download_workers = malloc(download_workers * sizeof(pthread_t));
    for (int i = 0; i < download_workers; i++) {
        pthread_create(&state->download_workers[i], NULL, download_worker_thread, state);
    }
    
    state->parse_workers = malloc(parse_workers * sizeof(pthread_t));
    for (int i = 0; i < parse_workers; i++) {
        pthread_create(&state->parse_workers[i], NULL, parse_worker_thread, state);
    }
    
    // Seed the crawler with start URL
    link_item_t *start_item = malloc(sizeof(link_item_t));
    start_item->url = strdup(start_url);
    start_item->parent_url = NULL;
    
    pthread_mutex_lock(&state->mutex);
    hash_set_add(state->queued_urls, start_url);
    queue_enqueue(state->links_queue, start_item);
    fprintf(stderr, "[MAIN] Seeded with: %s\n\n", start_url);
    pthread_cond_signal(&state->links_available);
    pthread_mutex_unlock(&state->mutex);
    
    // Wait for completion
    pthread_mutex_lock(&state->mutex);
    while (!is_crawl_complete(state)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        
        pthread_cond_timedwait(&state->state_changed, &state->mutex, &ts);
    }
    
    // Collect statistics while still holding mutex
    int total_downloads = 0;
    int total_parses = 0;
    for (int i = 0; i < download_workers; i++) {
        total_downloads += state->download_thread_work_count[i];
    }
    for (int i = 0; i < parse_workers; i++) {
        total_parses += state->parse_thread_work_count[i];
    }
    
    pthread_mutex_unlock(&state->mutex);
    
    fprintf(stderr, "\n=== CRAWL COMPLETE ===\n");
    
    // Print thread statistics
    fprintf(stderr, "\nDownload Thread Statistics:\n");
    for (int i = 0; i < download_workers; i++) {
        fprintf(stderr, "  D%d: %d URLs\n", i, state->download_thread_work_count[i]);
    }
    fprintf(stderr, "  Total: %d URLs downloaded\n", total_downloads);
    
    fprintf(stderr, "\nParse Thread Statistics:\n");
    for (int i = 0; i < parse_workers; i++) {
        fprintf(stderr, "  P%d: %d pages\n", i, state->parse_thread_work_count[i]);
    }
    fprintf(stderr, "  Total: %d pages parsed\n", total_parses);
    fprintf(stderr, "======================\n\n");
    
    // Shutdown threads
    pthread_mutex_lock(&state->mutex);
    state->shutdown = true;
    pthread_cond_broadcast(&state->links_available);
    pthread_cond_broadcast(&state->pages_available);
    pthread_cond_broadcast(&state->links_space);
    pthread_mutex_unlock(&state->mutex);
    
    // Join threads
    for (int i = 0; i < download_workers; i++) {
        pthread_join(state->download_workers[i], NULL);
    }
    for (int i = 0; i < parse_workers; i++) {
        pthread_join(state->parse_workers[i], NULL);
    }
    
    // Cleanup
    queue_destroy(state->links_queue);
    queue_destroy(state->pages_queue);
    hash_set_destroy(state->visited_urls);
    hash_set_destroy(state->queued_urls);
    
    pthread_mutex_destroy(&state->mutex);
    pthread_cond_destroy(&state->links_available);
    pthread_cond_destroy(&state->links_space);
    pthread_cond_destroy(&state->pages_available);
    pthread_cond_destroy(&state->state_changed);
    
    free(state->download_thread_work_count);
    free(state->parse_thread_work_count);
    free(state->download_workers);
    free(state->parse_workers);
    free(state);
    
    return 0;
}