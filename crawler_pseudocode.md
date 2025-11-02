# Multithreaded Web Crawler - Pseudocode Framework

## High-Level Architecture

```
                    ┌─────────────┐
                    │  Main Thread│
                    │  (crawl fn) │
                    └──────┬──────┘
                           │
                           │ Initialize
                           ▼
              ┌────────────────────────┐
              │  Shared Data Structures│
              │  - Links Queue         │
              │  - Visited Set         │
              │  - Mutexes/Conds       │
              └────────┬───────────────┘
                       │
           ┌───────────┴───────────┐
           ▼                       ▼
    ┌──────────────┐        ┌─────────────┐
    │  Download    │        │   Parse     │
    │  Worker Pool │        │  Worker Pool│
    │  (N threads) │        │  (M threads)│
    └──────┬───────┘        └──────┬──────┘
           │                       │
           │ fetch_fn()            │ extract links
           │                       │ call edge_fn()
           ▼                       ▼
    ┌──────────────┐        ┌─────────────┐
    │  HTML Content│───────▶│ New Links   │
    │              │        │  to Queue   │
    └──────────────┘        └─────────────┘
```

## Data Structures Needed

```c
// Main crawler state structure
typedef struct {
    // Thread pools
    pthread_t *download_workers;
    pthread_t *parse_workers;
    int num_download_workers;
    int num_parse_workers;
    
    // Work queues
    queue_t *links_queue;          // URLs waiting to be downloaded
    queue_t *content_queue;        // Downloaded content waiting to be parsed
    
    // Synchronization primitives
    pthread_mutex_t queue_mutex;
    pthread_mutex_t visited_mutex;
    pthread_cond_t queue_not_empty;
    pthread_cond_t queue_not_full;
    
    // Tracking structures
    hash_set_t *visited_urls;      // URLs already processed
    hash_set_t *queued_urls;       // URLs in queue (to avoid duplicates)
    
    // Thread coordination
    int active_downloads;          // Number of downloaders currently working
    int active_parsers;            // Number of parsers currently working
    bool shutdown;                 // Signal to stop all threads
    
    // Callbacks
    char *(*fetch_fn)(const char *);
    void (*edge_fn)(const char *, const char *);
    
} crawler_state_t;

// Queue item for links to download
typedef struct {
    char *url;
    char *parent_url;              // For edge_fn callback
} link_item_t;

// Queue item for content to parse
typedef struct {
    char *url;
    char *content;                 // HTML content (must be freed)
} content_item_t;
```

## Main Function Pseudocode

```c
int crawl(const char *start_url, 
          int download_workers,
          int parse_workers, 
          int work_queue_size,
          char *(*fetch_fn)(const char *link),
          void (*edge_fn)(const char *from, const char *to))
{
    // 1. INITIALIZATION PHASE
    crawler_state_t *state = allocate_and_initialize_state();
    
    state->num_download_workers = download_workers;
    state->num_parse_workers = parse_workers;
    state->fetch_fn = fetch_fn;
    state->edge_fn = edge_fn;
    
    // Initialize queues with size limits
    state->links_queue = queue_create(work_queue_size);
    state->content_queue = queue_create(work_queue_size);
    
    // Initialize synchronization primitives
    pthread_mutex_init(&state->queue_mutex);
    pthread_mutex_init(&state->visited_mutex);
    pthread_cond_init(&state->queue_not_empty);
    pthread_cond_init(&state->queue_not_full);
    
    // Initialize tracking structures
    state->visited_urls = hash_set_create();
    state->queued_urls = hash_set_create();
    
    // 2. START WORKER THREADS
    state->download_workers = malloc(download_workers * sizeof(pthread_t));
    for (i = 0; i < download_workers; i++) {
        pthread_create(&state->download_workers[i], NULL, 
                      download_worker_thread, state);
    }
    
    state->parse_workers = malloc(parse_workers * sizeof(pthread_t));
    for (i = 0; i < parse_workers; i++) {
        pthread_create(&state->parse_workers[i], NULL,
                      parse_worker_thread, state);
    }
    
    // 3. SEED THE CRAWLER
    enqueue_link(state, start_url, NULL);  // NULL parent for root
    
    // 4. WAIT FOR COMPLETION
    // The crawler is done when:
    // - links_queue is empty
    // - content_queue is empty
    // - no active downloads
    // - no active parsers
    
    pthread_mutex_lock(&state->queue_mutex);
    while (!is_crawl_complete(state)) {
        pthread_cond_wait(&state->queue_not_empty, &state->queue_mutex);
    }
    pthread_mutex_unlock(&state->queue_mutex);
    
    // 5. SHUTDOWN THREADS
    state->shutdown = true;
    pthread_cond_broadcast(&state->queue_not_empty);
    pthread_cond_broadcast(&state->queue_not_full);
    
    // Wait for all threads to finish
    for (i = 0; i < download_workers; i++) {
        pthread_join(state->download_workers[i], NULL);
    }
    for (i = 0; i < parse_workers; i++) {
        pthread_join(state->parse_workers[i], NULL);
    }
    
    // 6. CLEANUP
    cleanup_state(state);
    
    return 0;  // success
}
```

## Download Worker Thread Pseudocode

```c
void *download_worker_thread(void *arg)
{
    crawler_state_t *state = (crawler_state_t *)arg;
    
    while (true) {
        link_item_t *item = NULL;
        
        // 1. GET WORK FROM QUEUE
        pthread_mutex_lock(&state->queue_mutex);
        
        while (queue_is_empty(state->links_queue) && !state->shutdown) {
            pthread_cond_wait(&state->queue_not_empty, &state->queue_mutex);
        }
        
        if (state->shutdown && queue_is_empty(state->links_queue)) {
            pthread_mutex_unlock(&state->queue_mutex);
            break;  // Exit thread
        }
        
        item = queue_dequeue(state->links_queue);
        state->active_downloads++;
        
        pthread_cond_signal(&state->queue_not_full);  // Signal space available
        pthread_mutex_unlock(&state->queue_mutex);
        
        // 2. DOWNLOAD CONTENT (outside lock - this is the slow I/O operation)
        char *content = state->fetch_fn(item->url);
        
        // 3. HANDLE RESULT
        if (content != NULL) {
            // Successfully downloaded - enqueue for parsing
            content_item_t *content_item = create_content_item(item->url, content);
            
            pthread_mutex_lock(&state->queue_mutex);
            
            // Wait if content queue is full
            while (queue_is_full(state->content_queue) && !state->shutdown) {
                pthread_cond_wait(&state->queue_not_full, &state->queue_mutex);
            }
            
            if (!state->shutdown) {
                queue_enqueue(state->content_queue, content_item);
                pthread_cond_signal(&state->queue_not_empty);
            }
            
            state->active_downloads--;
            pthread_mutex_unlock(&state->queue_mutex);
        } else {
            // Broken link - just mark download complete
            pthread_mutex_lock(&state->queue_mutex);
            state->active_downloads--;
            pthread_cond_signal(&state->queue_not_empty);  // Might trigger completion
            pthread_mutex_unlock(&state->queue_mutex);
        }
        
        // 4. CLEANUP
        free(item->url);
        free(item->parent_url);
        free(item);
    }
    
    return NULL;
}
```

## Parse Worker Thread Pseudocode

```c
void *parse_worker_thread(void *arg)
{
    crawler_state_t *state = (crawler_state_t *)arg;
    
    while (true) {
        content_item_t *item = NULL;
        
        // 1. GET CONTENT TO PARSE
        pthread_mutex_lock(&state->queue_mutex);
        
        while (queue_is_empty(state->content_queue) && !state->shutdown) {
            pthread_cond_wait(&state->queue_not_empty, &state->queue_mutex);
        }
        
        if (state->shutdown && queue_is_empty(state->content_queue)) {
            pthread_mutex_unlock(&state->queue_mutex);
            break;  // Exit thread
        }
        
        item = queue_dequeue(state->content_queue);
        state->active_parsers++;
        
        pthread_cond_signal(&state->queue_not_full);
        pthread_mutex_unlock(&state->queue_mutex);
        
        // 2. PARSE CONTENT (outside lock)
        link_list_t *discovered_links = extract_links_from_html(item->content);
        
        // 3. PROCESS DISCOVERED LINKS
        for (each link in discovered_links) {
            // Call edge callback
            state->edge_fn(item->url, link);
            
            // Check if we should enqueue this link
            pthread_mutex_lock(&state->visited_mutex);
            
            bool should_visit = !hash_set_contains(state->visited_urls, link) &&
                               !hash_set_contains(state->queued_urls, link);
            
            if (should_visit) {
                hash_set_add(state->queued_urls, link);
            }
            
            pthread_mutex_unlock(&state->visited_mutex);
            
            if (should_visit) {
                enqueue_link(state, link, item->url);
            }
        }
        
        // 4. MARK URL AS VISITED
        pthread_mutex_lock(&state->visited_mutex);
        hash_set_add(state->visited_urls, item->url);
        hash_set_remove(state->queued_urls, item->url);
        pthread_mutex_unlock(&state->visited_mutex);
        
        // 5. CLEANUP
        pthread_mutex_lock(&state->queue_mutex);
        state->active_parsers--;
        pthread_cond_signal(&state->queue_not_empty);  // Might trigger completion
        pthread_mutex_unlock(&state->queue_mutex);
        
        free(item->url);
        free(item->content);  // Must free - allocated by fetch_fn
        free(item);
        free_link_list(discovered_links);
    }
    
    return NULL;
}
```

## Helper Functions Pseudocode

```c
// Check if crawling is complete
bool is_crawl_complete(crawler_state_t *state)
{
    // Must hold queue_mutex when calling this
    return queue_is_empty(state->links_queue) &&
           queue_is_empty(state->content_queue) &&
           state->active_downloads == 0 &&
           state->active_parsers == 0;
}

// Safely enqueue a new link
void enqueue_link(crawler_state_t *state, const char *url, const char *parent)
{
    link_item_t *item = malloc(sizeof(link_item_t));
    item->url = strdup(url);
    item->parent_url = parent ? strdup(parent) : NULL;
    
    pthread_mutex_lock(&state->queue_mutex);
    
    // Wait if queue is full
    while (queue_is_full(state->links_queue) && !state->shutdown) {
        pthread_cond_wait(&state->queue_not_full, &state->queue_mutex);
    }
    
    if (!state->shutdown) {
        queue_enqueue(state->links_queue, item);
        pthread_cond_signal(&state->queue_not_empty);
    }
    
    pthread_mutex_unlock(&state->queue_mutex);
}

// Extract links from HTML content
link_list_t *extract_links_from_html(const char *html)
{
    // Simple parsing strategy:
    // 1. Search for "link:" prefix
    // 2. Extract URL until newline
    // 3. Store in list
    
    link_list_t *links = create_link_list();
    
    const char *ptr = html;
    while ((ptr = strstr(ptr, "link:")) != NULL) {
        ptr += 5;  // Skip "link:"
        
        // Skip whitespace
        while (*ptr == ' ' || *ptr == '\t') ptr++;
        
        // Extract URL until newline
        const char *start = ptr;
        while (*ptr != '\n' && *ptr != '\0') ptr++;
        
        size_t len = ptr - start;
        char *url = malloc(len + 1);
        strncpy(url, start, len);
        url[len] = '\0';
        
        link_list_add(links, url);
        free(url);
    }
    
    return links;
}
```

## Key Synchronization Considerations

### Race Conditions to Avoid:
1. **Duplicate visits**: Multiple threads might try to visit the same URL
   - Solution: Use `visited_urls` and `queued_urls` sets with mutex protection
   
2. **Queue overflow**: Producers might outpace consumers
   - Solution: Use bounded queues with condition variables
   
3. **Premature shutdown**: Main thread thinks we're done but work is in progress
   - Solution: Track `active_downloads` and `active_parsers` counts

4. **Memory leaks**: Content from `fetch_fn` must be freed
   - Solution: Parse workers must free content after processing

### Deadlock Prevention:
- Always acquire locks in consistent order: `queue_mutex` before `visited_mutex`
- Never hold locks during slow operations (I/O, callbacks)
- Use condition variables to avoid busy-waiting

### Termination Detection:
The crawler is complete when ALL of these are true:
- `links_queue` is empty
- `content_queue` is empty  
- `active_downloads == 0`
- `active_parsers == 0`

This ensures no work is in-flight and no new work will be generated.

## Implementation Notes

### Queue Implementation:
- Use circular buffer or linked list
- Thread-safe with mutex protection
- Support for bounded size with blocking operations

### Hash Set Implementation:
- For tracking visited/queued URLs
- Could use simple hash table with chaining
- Or reuse existing library if available

### Error Handling:
- Check all malloc/pthread function return values
- Return -1 from `crawl()` on failure
- Clean up partial initialization on error

### Testing Strategy:
1. Start with single download + single parse worker
2. Test with simple graph (3-4 pages)
3. Gradually increase worker counts
4. Test with broken links
5. Test with cycles in graph
6. Load test with large work_queue_size
```