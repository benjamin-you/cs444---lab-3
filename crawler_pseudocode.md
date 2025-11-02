# Multi-threaded Web Crawler - Implementation Notes

## Overview

This is a multi-threaded web crawler library that uses a producer-consumer architecture with two thread pools: downloaders and parsers. The implementation uses a single global mutex with multiple condition variables for clean synchronization.

## Architecture

```
                         ┌─────────────┐
                         │  Main Thread│
                         │  (crawl fn) │
                         └──────┬──────┘
                                │
                                │ Initialize & seed
                                ▼
                   ┌───────────────────────────┐
                   │  Shared State (mutex)     │
                   │  - Links Queue (BOUNDED)  │
                   │  - Pages Queue (UNBOUNDED)│
                   │  - Visited/Queued Sets    │
                   │  - Active Counters        │
                   └────────┬──────────────────┘
                            │
            ┌───────────────┴────────────────┐
            ▼                                ▼
     ┌─────────────┐                  ┌─────────────┐
     │   PARSERS   │                  │ DOWNLOADERS │
     │  (PRODUCERS)│                  │ (CONSUMERS) │
     │  M threads  │                  │  N threads  │
     └──────┬──────┘                  └──────┬──────┘
            │                                │
            │ PRODUCE links                  │ CONSUME links
            │                                │
            ▼                                ▼
     ┌────────────────────────────────────────────┐
     │        LINKS QUEUE (BOUNDED)               │
     │  Parsers push ──────────▶ Downloaders pop  │
     │        (work_queue_size limit)             │
     │  Condition: links_available, links_space   │
     └────────────────────────────────────────────┘
                                │
                                │ fetch_fn()
                                │ PRODUCE pages
                                ▼
     ┌────────────────────────────────────────────┐
     │        PAGES QUEUE (UNBOUNDED)             │
     │  Downloaders push ──────▶ Parsers pop      │
     │         (no size limit)                    │
     │  Condition: pages_available                │
     └────────────────────────────────────────────┘
                                │
                                │ extract links
                                │ call edge_fn()
                                └──────┐
                                       │
                                       └─── (cycle repeats)
```

## Key Design Decisions

### Single Global Mutex
Instead of separate mutexes for different data structures, we use one global mutex that protects all shared state. This simplifies reasoning about synchronization and eliminates potential deadlock scenarios.

**Protected State:**
- Both queues (links and pages)
- Hash sets (visited_urls, queued_urls)
- Active worker counters
- Shutdown flag

### Four Condition Variables

1. **`links_available`**: Downloaders wait here when links queue is empty
2. **`links_space`**: Parsers wait here when links queue is full
3. **`pages_available`**: Parsers wait here when pages queue is empty
4. **`state_changed`**: Main thread waits for completion signal

### Bounded vs Unbounded Queues

**Links Queue (Bounded):**
- Size limited by `work_queue_size` parameter
- Prevents memory explosion from too many pending downloads
- Parsers block when full (flow control)

**Pages Queue (Unbounded):**
- No size limit
- Prevents deadlock: downloaders never block on producing pages
- Memory usage controlled indirectly by bounded links queue

### Active Worker Counting

Critical insight: Only count threads as "active" when they're actually working, not when waiting for work.

```c
// WRONG: Increment before getting work
page_item = queue_dequeue(state->pages_queue);
state->active_parsers++;  // Bad: counted as active while waiting

// CORRECT: Increment after getting work
while (queue_is_empty(state->pages_queue) && !state->shutdown) {
    pthread_cond_wait(&state->pages_available, &state->mutex);
}
page_item = queue_dequeue(state->pages_queue);
state->active_parsers++;  // Good: only counted when we have work
```

This ensures completion detection works correctly: crawler is done when queues are empty AND no active workers.

## Thread Lifecycles

### Download Worker (Consumer → Producer)

```
1. Lock mutex
2. While links_queue empty and not shutdown:
   - Wait on links_available
3. Dequeue link from links_queue
4. Increment active_downloads
5. Signal links_space (parsers can add more)
6. Unlock mutex

7. Fetch content (OUTSIDE LOCK - slow I/O)

8. Lock mutex
9. If content not null:
   - Enqueue page to pages_queue
   - Signal pages_available
10. Decrement active_downloads
11. Broadcast state_changed (completion check)
12. Unlock mutex
```

### Parse Worker (Consumer → Producer)

```
1. Lock mutex
2. While pages_queue empty and not shutdown:
   - Wait on pages_available
3. Dequeue page from pages_queue
4. Increment active_parsers
5. Unlock mutex

6. Parse HTML, extract links (OUTSIDE LOCK - CPU work)

7. Lock mutex
8. For each discovered link:
   - Unlock, call edge_fn callback, re-lock
   - Check if already visited/queued
   - If new:
     * Add to queued_urls
     * While links_queue full: wait on links_space
     * Enqueue to links_queue
     * Signal links_available
9. Mark URL as visited, remove from queued
10. Decrement active_parsers
11. Broadcast state_changed (completion check)
12. Unlock mutex
```

### Main Thread

```
1. Initialize all state, mutexes, condition variables
2. Create download and parse worker threads
3. Seed links_queue with start_url
4. Lock mutex
5. While not is_crawl_complete():
   - Timedwait on state_changed (2 second timeout)
   - Re-check completion on wake
6. Set shutdown flag
7. Broadcast all condition variables
8. Unlock mutex
9. Join all worker threads
10. Cleanup and return
```

## Completion Detection

Crawler is complete when ALL of these are true:
- `links_queue` is empty
- `pages_queue` is empty
- `active_downloads == 0`
- `active_parsers == 0`

This is checked atomically while holding the mutex, so there's no race condition between workers changing state and main thread checking completion.

Workers signal `state_changed` after:
- Completing a download
- Completing a parse
- This wakes the main thread to re-check completion

## Synchronization Correctness

### Deadlock Prevention
- **Single mutex**: No lock ordering issues
- **No nested locks**: Mutex released during callbacks
- **Unbounded pages queue**: Downloaders never block on producing

### Race Condition Prevention
- **Atomic state updates**: All state changes while holding mutex
- **Completion check**: Atomic check of all conditions
- **Active counters**: Updated in same critical section as queue operations

### Signal Loss Prevention
- **Broadcast after state change**: Every worker broadcasts state_changed
- **Timeout in wait**: Main thread uses timedwait (2 sec timeout)
- **Check before wait**: While loop checks condition before waiting

## Memory Management

**Allocated by fetch_fn:**
- HTML content strings
- **Must be freed by parse workers** after processing

**Allocated by crawler:**
- link_item_t and page_item_t structures
- Copied URL strings
- Link arrays from extract_links()
- All freed by workers after use

**Persistent:**
- Hash set buckets and nodes
- Queue nodes
- Freed during cleanup

## Testing Observations

**Parallelism Verified:**
- Multiple threads process work (shown in statistics)
- Work distributed across thread pool
- Thread IDs in debug output show concurrent activity

**Producer-Consumer Verified:**
- Parsers produce links → Downloaders consume
- Downloaders produce pages → Parsers consume
- Queue sizes fluctuate (shown in debug)
- Blocking occurs when links queue full (shown in debug)

**Correctness Verified:**
- No duplicate visits (visited_urls set)
- All edges reported via edge_fn callback
- Completes when graph fully explored
- Clean shutdown, no deadlocks

## Performance Characteristics

**Time Complexity:**
- Hash set lookups: O(1) average
- Queue operations: O(1)
- Per-URL work: O(1) + I/O time

**Space Complexity:**
- Links queue: O(work_queue_size) - bounded
- Pages queue: O(active_downloads) - roughly bounded by parallelism
- Visited set: O(total_urls) - unbounded but necessary
- Queued set: O(work_queue_size) - bounded

**Parallelism:**
- Downloaders: Limited by I/O (network/disk)
- Parsers: Limited by CPU (HTML parsing)
- Optimal thread count depends on workload characteristics

## Edge Cases Handled

1. **Broken links**: fetch_fn returns NULL, skip gracefully
2. **Cycles in graph**: visited_urls prevents revisiting
3. **Duplicate links**: queued_urls prevents duplicate enqueueing
4. **Empty pages**: No links found, mark visited, continue
5. **Queue full**: Parsers block until space available
6. **Early shutdown**: Cleanup partial work, join threads safely

## Future Improvements

1. **Per-domain rate limiting**: Add delay between requests to same domain
2. **URL normalization**: Canonicalize URLs before checking visited
3. **Robots.txt support**: Respect crawler directives
4. **Better statistics**: Track bytes downloaded, parse time, etc.
5. **Configurable hash set size**: Tune for expected number of URLs
6. **Link priority**: Breadth-first vs depth-first crawling