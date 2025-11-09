/*
 * Multi-threaded Web Crawler Library Implementation
 * 
 * SYNCHRONIZATION DESIGN:
 * - Fine-grained locking: Each data structure has its own mutex
 * - Minimizes lock contention for maximum parallelism
 * - Strict lock ordering to prevent deadlock
 * 
 * LOCK ORDERING (always acquire in this order):
 * 1. links_queue.mutex
 * 2. pages_queue.mutex  
 * 3. visited_mutex
 * 
 * Never hold multiple locks simultaneously unless absolutely necessary.
 * Always release locks before calling callbacks or doing I/O.
 * 
 * PRODUCER-CONSUMER ARCHITECTURE:
 * 1. Links Queue (bounded): Parsers produce URLs → Downloaders consume URLs
 *    - Has its own mutex and two condition variables
 *    - Bounded to control memory usage (work_queue_size parameter)
 * 
 * 2. Pages Queue (unbounded): Downloaders produce HTML → Parsers consume HTML
 *    - Has its own mutex and one condition variable
 *    - Unbounded to prevent deadlock situations
 * 
 * THREAD SAFETY:
 * - Each queue is independently thread-safe
 * - Hash sets protected by separate visited_mutex
 * - Active counters protected by their respective queue mutexes
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

// Thread-safe queue with its own mutex and condition variables
typedef struct {
    queue_node_t *head;
    queue_node_t *tail;
    int size;
    int capacity;              // -1 for unbounded
    
    pthread_mutex_t mutex;     // Protects this queue
    pthread_cond_t not_empty;  // Consumers wait here
    pthread_cond_t not_full;   // Producers wait here (only for bounded)
    
    int active_workers;        // Workers currently processing items from this queue
    bool shutdown;             // Shutdown signal
    
    // For completion detection
    int total_enqueued;        // Total items ever added to queue
    int total_dequeued;        // Total items ever removed from queue
} thread_safe_queue_t;

// Hash set node for tracking visited URLs
typedef struct hash_node {
    char *url;
    struct hash_node *next;
} hash_node_t;

// Simple hash set (NOT thread-safe by itself)
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

// Main crawler state
typedef struct {
    // Thread pools
    pthread_t *download_workers;
    pthread_t *parse_workers;
    int num_download_workers;
    int num_parse_workers;
    
    // Thread-safe queues (each has its own mutex)
    thread_safe_queue_t *links_queue;  // Parsers -> Downloaders
    thread_safe_queue_t *pages_queue;  // Downloaders -> Parsers
    
    // Separate mutex for visited/queued tracking
    pthread_mutex_t visited_mutex;
    hash_set_t *visited_urls;
    hash_set_t *queued_urls;
    
    // Callbacks
    char *(*fetch_fn)(const char *);
    void (*edge_fn)(const char *, const char *);
    
    // Statistics
    int *download_thread_work_count;
    int *parse_thread_work_count;
} crawler_state_t;

// ============================================================================
// THREAD-SAFE QUEUE OPERATIONS
// Each queue has its own mutex for fine-grained locking
// ============================================================================

// Create a new thread-safe queue
thread_safe_queue_t *ts_queue_create(int capacity) {
    thread_safe_queue_t *q = malloc(sizeof(thread_safe_queue_t));
    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
    q->capacity = capacity;
    q->active_workers = 0;
    q->shutdown = false;
    q->total_enqueued = 0;
    q->total_dequeued = 0;
    
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    
    return q;
}

// Check if queue is empty (must hold queue's mutex)
bool ts_queue_is_empty(thread_safe_queue_t *q) {
    return q->size == 0;
}

// Check if queue is full (must hold queue's mutex)
bool ts_queue_is_full(thread_safe_queue_t *q) {
    if (q->capacity == -1) return false;
    return q->size >= q->capacity;
}

// Enqueue item (thread-safe, may block if queue is full)
void ts_queue_enqueue(thread_safe_queue_t *q, void *data) {
    pthread_mutex_lock(&q->mutex);
    
    // Wait if queue is full (only for bounded queues)
    while (ts_queue_is_full(q) && !q->shutdown) {
        pthread_cond_wait(&q->not_full, &q->mutex);
    }
    
    if (q->shutdown) {
        pthread_mutex_unlock(&q->mutex);
        return;
    }
    
    // Add to queue
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
    q->total_enqueued++;
    
    // Signal consumers
    pthread_cond_signal(&q->not_empty);
    
    pthread_mutex_unlock(&q->mutex);
}

// Dequeue item (thread-safe, blocks if queue is empty)
// Returns NULL only on shutdown
void *ts_queue_dequeue(thread_safe_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    
    // Wait while queue is empty (re-check after each wake)
    while (ts_queue_is_empty(q) && !q->shutdown) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }
    
    // Only return NULL if shutting down AND queue is empty
    if (ts_queue_is_empty(q)) {
        // Must be shutdown (from while loop condition)
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }
    
    // Queue has items - dequeue one
    queue_node_t *node = q->head;
    void *data = node->data;
    q->head = node->next;
    
    if (q->head == NULL) {
        q->tail = NULL;
    }
    
    q->size--;
    q->total_dequeued++;
    free(node);
    
    // Signal producers (for bounded queues)
    pthread_cond_signal(&q->not_full);
    
    pthread_mutex_unlock(&q->mutex);
    return data;
}

// Increment active worker count (thread-safe)
void ts_queue_inc_active(thread_safe_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    q->active_workers++;
    pthread_mutex_unlock(&q->mutex);
}

// Decrement active worker count (thread-safe)
void ts_queue_dec_active(thread_safe_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    q->active_workers--;
    pthread_mutex_unlock(&q->mutex);
}

// Get queue size (thread-safe)
int ts_queue_size(thread_safe_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    int size = q->size;
    pthread_mutex_unlock(&q->mutex);
    return size;
}

// Get active worker count (thread-safe)
int ts_queue_active_count(thread_safe_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    int count = q->active_workers;
    pthread_mutex_unlock(&q->mutex);
    return count;
}

// Destroy queue
void ts_queue_destroy(thread_safe_queue_t *q) {
    while (q->head != NULL) {
        queue_node_t *node = q->head;
        q->head = node->next;
        free(node->data);
        free(node);
    }
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q);
}

// ============================================================================
// HASH SET OPERATIONS (Protected by visited_mutex)
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
int get_thread_index(pthread_t *threads, int count, pthread_t self) {
    for (int i = 0; i < count; i++) {
        if (pthread_equal(threads[i], self)) {
            return i;
        }
    }
    return -1;
}

// Check if the crawl is complete
// Must acquire both queue mutexes to check atomically
bool is_crawl_complete(crawler_state_t *state) {
    // Lock order: links -> pages
    pthread_mutex_lock(&state->links_queue->mutex);
    pthread_mutex_lock(&state->pages_queue->mutex);
    
    // Completion criteria - ALL must be true:
    // 1. links_queue empty - no URLs waiting to be downloaded
    // 2. pages_queue empty - no pages waiting to be parsed
    // 3. No active downloaders - no one producing pages
    // 4. No active parsers - no one producing links
    // 5. total_enqueued == total_dequeued for both queues (all work consumed)
    //
    // The key: We need BOTH queues empty AND no active workers.
    // If downloaders are active, they may produce pages (even if links queue is empty).
    // If parsers are active, they may produce links (even if pages queue is empty).
    
    bool links_done = ts_queue_is_empty(state->links_queue) &&
                      state->links_queue->total_enqueued == state->links_queue->total_dequeued;
    
    bool pages_done = ts_queue_is_empty(state->pages_queue) &&
                      state->pages_queue->total_enqueued == state->pages_queue->total_dequeued;
    
    bool no_work_in_progress = (state->links_queue->active_workers == 0) &&
                                (state->pages_queue->active_workers == 0);
    
    bool complete = links_done && pages_done && no_work_in_progress;
    
    pthread_mutex_unlock(&state->pages_queue->mutex);
    pthread_mutex_unlock(&state->links_queue->mutex);
    
    return complete;
}

// Extract all links from HTML content
char **extract_links(const char *html, int *count) {
    *count = 0;
    
    const char *ptr = html;
    while ((ptr = strstr(ptr, "<a href=\"")) != NULL) {
        (*count)++;
        ptr += 9;
    }
    
    if (*count == 0) return NULL;
    
    char **links = malloc(*count * sizeof(char *));
    int idx = 0;
    
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
 * Consumes URLs from links_queue, fetches content, produces pages to pages_queue
 * 
 * Locking strategy:
 * - Acquires links_queue.mutex only to dequeue work
 * - Releases lock during I/O (fetch_fn)
 * - Acquires pages_queue.mutex only to enqueue result
 * - No nested locks needed - operates on different queues at different times
 */
void *download_worker_thread(void *arg) {

    // struct timespec remaining, request = { 0, 100*1000*1000 };
    // nanosleep(&request, &remaining);

    crawler_state_t *state = (crawler_state_t *)arg;
    pthread_t self = pthread_self();
    int thread_id = -1;
    
    while (true) {
        // Dequeue work (blocks if empty, thread-safe)
        link_item_t *link_item = ts_queue_dequeue(state->links_queue);
        if (link_item == NULL) break; // Shutdown
        
        // Lazy initialize thread_id (only once)
        if (thread_id == -1) {
            thread_id = get_thread_index(state->download_workers, 
                                         state->num_download_workers, self);
        }
        
        // Increment work counter (every time we process an item)
        state->download_thread_work_count[thread_id]++;
        
        fprintf(stderr, "[D%d] Dequeued link: %s (queue: %d)\n", 
                thread_id, link_item->url, ts_queue_size(state->links_queue));
        
        // Increment active count
        ts_queue_inc_active(state->links_queue);
        
        // Do I/O (no locks held)
        fprintf(stderr, "[D%d] Fetching: %s\n", thread_id, link_item->url);
        char *content = state->fetch_fn(link_item->url);
        
        // Enqueue result if successful (thread-safe)
        if (content != NULL) {
            page_item_t *page_item = malloc(sizeof(page_item_t));
            page_item->url = strdup(link_item->url);
            page_item->content = content;
            
            ts_queue_enqueue(state->pages_queue, page_item);
            fprintf(stderr, "[D%d] Enqueued page: %s (queue: %d)\n",
                    thread_id, page_item->url, ts_queue_size(state->pages_queue));
        } else {
            fprintf(stderr, "[D%d] Broken link: %s\n", thread_id, link_item->url);
        }
        
        // Decrement active count and signal potential completion
        ts_queue_dec_active(state->links_queue);
        
        // Cleanup
        free(link_item->url);
        if (link_item->parent_url) free(link_item->parent_url);
        free(link_item);
    }
    
    fprintf(stderr, "[D%d] Thread exiting\n", thread_id);
    return NULL;
}

/*
 * Parse Worker Thread
 * 
 * Consumes pages from pages_queue, extracts links, produces URLs to links_queue
 * 
 * Locking strategy:
 * - Acquires pages_queue.mutex only to dequeue work
 * - Releases lock during parsing
 * - For each link: acquires visited_mutex briefly to check/add
 * - Acquires links_queue.mutex (via ts_queue_enqueue) to add new work
 * - Lock ordering: Always visited_mutex before any queue operation if both needed
 */
void *parse_worker_thread(void *arg) {

    // struct timespec remaining, request = { 0, 100*1000*1000 };
    // nanosleep(&request, &remaining);
    
    crawler_state_t *state = (crawler_state_t *)arg;
    pthread_t self = pthread_self();
    int thread_id = -1;
    
    while (true) {
        // Dequeue work (blocks if empty, thread-safe)
        page_item_t *page_item = ts_queue_dequeue(state->pages_queue);
        if (page_item == NULL) break; // Shutdown
        
        // Lazy initialize thread_id (only once)
        if (thread_id == -1) {
            thread_id = get_thread_index(state->parse_workers, 
                                         state->num_parse_workers, self);
        }
        
        // Increment work counter (every time we process an item)
        state->parse_thread_work_count[thread_id]++;
        
        fprintf(stderr, "[P%d] Dequeued page: %s (queue: %d)\n",
                thread_id, page_item->url, ts_queue_size(state->pages_queue));
        
        // Increment active count
        ts_queue_inc_active(state->pages_queue);
        
        // Parse (no locks held)
        fprintf(stderr, "[P%d] Parsing: %s\n", thread_id, page_item->url);
        int link_count;
        char **links = extract_links(page_item->content, &link_count);
        fprintf(stderr, "[P%d] Found %d links\n", thread_id, link_count);
        
        int new_links_added = 0;
        
        // Process each link
        for (int i = 0; i < link_count; i++) {
            // Call edge callback (no locks)
            state->edge_fn(page_item->url, links[i]);
            
            // Check if should visit (acquire visited_mutex)
            pthread_mutex_lock(&state->visited_mutex);
            bool should_visit = !hash_set_contains(state->visited_urls, links[i]) &&
                               !hash_set_contains(state->queued_urls, links[i]);
            if (should_visit) {
                hash_set_add(state->queued_urls, links[i]);
            }
            pthread_mutex_unlock(&state->visited_mutex);
            
            // Enqueue new work (thread-safe, may block if queue full)
            if (should_visit) {
                link_item_t *new_link = malloc(sizeof(link_item_t));
                new_link->url = strdup(links[i]);
                new_link->parent_url = strdup(page_item->url);
                
                ts_queue_enqueue(state->links_queue, new_link);
                new_links_added++;
            }
        }
        
        fprintf(stderr, "[P%d] Enqueued %d new links (queue: %d)\n",
                thread_id, new_links_added, ts_queue_size(state->links_queue));
        
        // Mark as visited (acquire visited_mutex)
        pthread_mutex_lock(&state->visited_mutex);
        hash_set_add(state->visited_urls, page_item->url);
        hash_set_remove(state->queued_urls, page_item->url);
        pthread_mutex_unlock(&state->visited_mutex);
        
        // Decrement active count and signal potential completion
        ts_queue_dec_active(state->pages_queue);
        
        // Cleanup
        for (int i = 0; i < link_count; i++) {
            free(links[i]);
        }
        free(links);
        free(page_item->url);
        free(page_item->content);
        free(page_item);
    }
    
    fprintf(stderr, "[P%d] Thread exiting\n", thread_id);
    return NULL;
}

// ============================================================================
// MAIN CRAWL FUNCTION
// ============================================================================

int crawl(
    const char *start_url,
    int download_workers,
    int parse_workers,
    int work_queue_size,
    char *(*fetch_fn)(const char *link),
    void (*edge_fn)(const char *from, const char *to))
{
    if (!start_url || !fetch_fn || !edge_fn) return -1;
    if (download_workers <= 0 || parse_workers <= 0 || work_queue_size <= 0) return -1;
    
    crawler_state_t *state = malloc(sizeof(crawler_state_t));
    if (!state) return -1;
    
    state->num_download_workers = download_workers;
    state->num_parse_workers = parse_workers;
    state->fetch_fn = fetch_fn;
    state->edge_fn = edge_fn;
    
    // Create thread-safe queues (each has its own mutex)
    state->links_queue = ts_queue_create(work_queue_size);  // Bounded
    state->pages_queue = ts_queue_create(-1);  // Unbounded
    
    // Initialize visited tracking mutex
    pthread_mutex_init(&state->visited_mutex, NULL);
    state->visited_urls = hash_set_create();
    state->queued_urls = hash_set_create();
    
    // Initialize statistics
    state->download_thread_work_count = calloc(download_workers, sizeof(int));
    state->parse_thread_work_count = calloc(parse_workers, sizeof(int));
    
    fprintf(stderr, "\n=== CRAWLER CONFIGURATION ===\n");
    fprintf(stderr, "Download workers: %d\n", download_workers);
    fprintf(stderr, "Parse workers: %d\n", parse_workers);
    fprintf(stderr, "Links queue: %d (bounded)\n", work_queue_size);
    fprintf(stderr, "Pages queue: unlimited\n");
    fprintf(stderr, "Locking: Fine-grained (per data structure)\n");
    fprintf(stderr, "============================\n\n");
    
    // Create worker threads
    state->download_workers = malloc(download_workers * sizeof(pthread_t));
    state->parse_workers = malloc(parse_workers * sizeof(pthread_t));
    pthread_create(&state->download_workers[0], NULL, download_worker_thread, state);
    pthread_create(&state->parse_workers[0], NULL, parse_worker_thread, state);

    for (int i = 1; i < download_workers; i++) {
        pthread_create(&state->download_workers[i], NULL, download_worker_thread, state);
    }
    
    for (int i = 1; i < parse_workers; i++) {
        pthread_create(&state->parse_workers[i], NULL, parse_worker_thread, state);
    }
    
    // Seed the crawler
    link_item_t *start_item = malloc(sizeof(link_item_t));
    start_item->url = strdup(start_url);
    start_item->parent_url = NULL;
    
    pthread_mutex_lock(&state->visited_mutex);
    hash_set_add(state->queued_urls, start_url);
    pthread_mutex_unlock(&state->visited_mutex);
    
    ts_queue_enqueue(state->links_queue, start_item);
    fprintf(stderr, "[MAIN] Seeded with: %s\n\n", start_url);
    
    // Allows threads to initialise before checking if crawl is complete
    struct timespec remaining, request = { 0, 100*1000*1000 };
    nanosleep(&request, &remaining);

    // Wait for completion - poll periodically
    while (!is_crawl_complete(state)) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 100000000;  // 100ms
        nanosleep(&ts, NULL);
        
        // Debug: print state periodically
        static int check_count = 0;
        if (check_count++ % 10 == 0) {
            pthread_mutex_lock(&state->links_queue->mutex);
            pthread_mutex_lock(&state->pages_queue->mutex);
            fprintf(stderr, "[MAIN] Checking completion: links(size=%d, enq=%d, deq=%d, active=%d) pages(size=%d, enq=%d, deq=%d, active=%d)\n",
                    state->links_queue->size, state->links_queue->total_enqueued, state->links_queue->total_dequeued, state->links_queue->active_workers,
                    state->pages_queue->size, state->pages_queue->total_enqueued, state->pages_queue->total_dequeued, state->pages_queue->active_workers);
            pthread_mutex_unlock(&state->pages_queue->mutex);
            pthread_mutex_unlock(&state->links_queue->mutex);
        }
    }
    
    fprintf(stderr, "\n=== CRAWL COMPLETE ===\n");
    fprintf(stderr, "Links: enqueued=%d, dequeued=%d\n",
            state->links_queue->total_enqueued, state->links_queue->total_dequeued);
    fprintf(stderr, "Pages: enqueued=%d, dequeued=%d\n",
            state->pages_queue->total_enqueued, state->pages_queue->total_dequeued);
    
    // Shutdown
    pthread_mutex_lock(&state->links_queue->mutex);
    state->links_queue->shutdown = true;
    pthread_cond_broadcast(&state->links_queue->not_empty);
    pthread_mutex_unlock(&state->links_queue->mutex);
    
    pthread_mutex_lock(&state->pages_queue->mutex);
    state->pages_queue->shutdown = true;
    pthread_cond_broadcast(&state->pages_queue->not_empty);
    pthread_mutex_unlock(&state->pages_queue->mutex);
    
    // Join threads
    for (int i = 0; i < download_workers; i++) {
        pthread_join(state->download_workers[i], NULL);
    }
    for (int i = 0; i < parse_workers; i++) {
        pthread_join(state->parse_workers[i], NULL);
    }
    
    // Print statistics
    fprintf(stderr, "\nDownload Thread Statistics:\n");
    int total_downloads = 0;
    for (int i = 0; i < download_workers; i++) {
        fprintf(stderr, "  D%d: %d URLs\n", i, state->download_thread_work_count[i]);
        total_downloads += state->download_thread_work_count[i];
    }
    fprintf(stderr, "  Total: %d\n", total_downloads);
    
    fprintf(stderr, "\nParse Thread Statistics:\n");
    int total_parses = 0;
    for (int i = 0; i < parse_workers; i++) {
        fprintf(stderr, "  P%d: %d pages\n", i, state->parse_thread_work_count[i]);
        total_parses += state->parse_thread_work_count[i];
    }
    fprintf(stderr, "  Total: %d\n", total_parses);
    fprintf(stderr, "======================\n\n");
    
    // Cleanup
    ts_queue_destroy(state->links_queue);
    ts_queue_destroy(state->pages_queue);
    hash_set_destroy(state->visited_urls);
    hash_set_destroy(state->queued_urls);
    
    pthread_mutex_destroy(&state->visited_mutex);
    
    free(state->download_thread_work_count);
    free(state->parse_thread_work_count);
    free(state->download_workers);
    free(state->parse_workers);
    free(state);
    
    return 0;
}