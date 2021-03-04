#include "filesys/cache.h"
#include <bitmap.h>
#include "filesys/filesys.h"
#include "devices/timer.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* Base addresses of cache, cache metadata, and cache bitmap. */
static void *cache;
static struct cache_entry *cache_metadata;
static struct bitmap *cache_bitmap;

/* Lock for mutual exclusion on access to the cache. */
static struct lock cache_lock;

/* Clock hand and timeout for the eviction algorithm. */
static struct cache_entry *clock_hand;
static size_t clock_timeout;

/* A semaphore to signal the read-ahead worker thread. */
static struct semaphore read_ahead_sema;

/* List of block sectors to be pre-loaded into cache. */
static struct list read_ahead_list;

/* Thread functions for asynchronous read-ahead and periodic
   writes of dirty blocks in cache back to disk. */
static thread_func cache_read_ahead NO_RETURN;
static thread_func cache_periodic_flush NO_RETURN;

static void clock_advance (void);
static size_t clock_find (void);
static size_t cache_evict_block (void);
static size_t cache_find_block (block_sector_t sector);
static size_t cache_load (block_sector_t sector);

/* Translates CACHE_IDX into address of the corresponding
   cache slot in the cache. */
void *
cache_idx_to_cache_slot (size_t cache_idx)
{
  ASSERT (cache_idx < CACHE_SIZE);

  size_t ofs = cache_idx * BLOCK_SECTOR_SIZE;
  void *cache_slot = ((uint8_t *) cache) + ofs;

  return cache_slot;
}

/* Translates CACHE_IDX into address of the corresponding
   cache_entry in the cache_metadata. */
struct cache_entry *
cache_idx_to_cache_entry (size_t cache_idx)
{
  ASSERT (cache_idx < CACHE_SIZE);

  struct cache_entry *ce = cache_metadata + cache_idx;
  return ce;
}

/* Initializes the buffer cache.

   More specifically, allocates memory for cache, cache
   metadata, and cache bitmap. Initializes the global
   cache_lock for the eviction algorithm and individual
   rw_locks for each of the cache entries. Spawns two
   worker threads to handle asynchronous read-ahead and
   periodic writes of dirty blocks back to disk. */
void
cache_init (void)
{
  /* Allocate memory. */
  cache = malloc (BLOCK_SECTOR_SIZE * CACHE_SIZE);
  cache_metadata = malloc (sizeof (struct cache_entry) * CACHE_SIZE);
  cache_bitmap = bitmap_create (CACHE_SIZE);
  if (cache == NULL || cache_metadata == NULL || cache_bitmap == NULL)
    PANIC ("cache_init: failed memory allocation for cache data structures.");

  /* Initialize cache_lock. */
  lock_init (&cache_lock);

  /* Initialize fields including rw_lock for each cache_entry. */
  for (size_t idx = 0; idx < CACHE_SIZE; idx++)
    {
      struct cache_entry *ce = cache_metadata + idx;
      ce->sector_idx = SIZE_MAX;
      ce->cache_idx = idx;
      ce->dirty = false;
      ce->accessed = false;
      rw_lock_init (&ce->rw_lock);
    }

  /* Initialize clock hand and timeout for eviction algorithm. */
  clock_hand = cache_metadata + (CACHE_SIZE / 4);
  clock_timeout = 0;

  /* Initialize list and semaphore for read-ahead worker thread. */
  list_init (&read_ahead_list);
  sema_init (&read_ahead_sema, 0);

  /* Spawn worker threads for read-ahead and cache flushes. */
  tid_t tid_read_ahead = thread_create ("read-ahead", PRI_DEFAULT,
                                        cache_read_ahead, NULL);
  tid_t tid_periodic_flush = thread_create ("periodic-flush", PRI_DEFAULT,
                                            cache_periodic_flush, NULL);
  if (tid_read_ahead == TID_ERROR || tid_periodic_flush == TID_ERROR)
    PANIC ("cache_init: failed to spawn cache worker threads.");
}

/* Get a block with sector number SECTOR into memory, whether by
   locating it in the cache or loading it from disk.

   Upon return, the rw_lock of the cache_entry for the cache slot
   will be held in shared_acquire mode. It is the caller's
   responsibility to release the rw_lock or upgrade it to
   exclusive_acquire if necessary. */
size_t
cache_get_block (block_sector_t sector, enum sector_type type)
{
  ASSERT (!lock_held_by_current_thread (&cache_lock));

  lock_acquire (&cache_lock);
  size_t cache_idx = cache_load (sector);
  struct cache_entry *ce = cache_idx_to_cache_entry (cache_idx);
  ce->type = type;
  ce->sector_idx = sector;
  ce->accessed = true;
  lock_release (&cache_lock);

  return cache_idx;
}

/* Flushes the cache by writing all dirty blocks back to disk.
   
   The rw_lock for each cache_entry of a dirty block must be
   obtained through shared_acquire and we wait for the rw_lock
   for each dirty block rather than skipping over any. The dirty
   flag of each written block is then set back to false. */
void
cache_flush (void)
{
  for (size_t idx = 0; idx < CACHE_SIZE; idx++)
    {
      struct cache_entry *ce = cache_metadata + idx;

      rw_lock_shared_acquire (&ce->rw_lock);
      if (ce->dirty)
        {
          void *cache_slot = cache_idx_to_cache_slot (idx);
          block_write (fs_device, ce->sector_idx, cache_slot);
          ce->dirty = false;
        }
      rw_lock_shared_release (&ce->rw_lock);
    }
}

/* Advance the hand of the clock algorithm by one cache slot,
   wrapping around to the first slot if the end of the cache
   is reached. */
static void
clock_advance (void)
{
  if (++clock_hand >= cache_metadata + CACHE_SIZE)
    clock_hand = cache_metadata;
}

/* Find a block in the cache to evict using the clock algorithm.
   Returns the cache_idx of the slot occupied by the block to be
   evicted.
   
   Data blocks are chosen for eviction over inode blocks, unless
   this leads the clock hand to traverse a full cycle around the
   cache, at which point the original block pointed to by the clock
   hand is chosen for eviction. */
static size_t
clock_find (void)
{
  ASSERT (lock_held_by_current_thread (&cache_lock));

  while (true)
    {
      rw_lock_shared_acquire (&clock_hand->rw_lock);
      if (clock_hand->type == DATA || clock_timeout == CACHE_SIZE)
        {
          /* Obtain the rw_lock for the cache slot with exclusive
             acquire to prevent race conditions caused by reads and
             writes on the block while it is being evicted. */
          rw_lock_shared_to_exclusive (&clock_hand->rw_lock);
          size_t cache_idx = clock_hand->cache_idx;
          clock_advance ();
          clock_timeout = 0;
          
          return cache_idx;
        }
      
      /* Advance clock hand. */
      clock_advance ();
      clock_timeout++;
    }

  NOT_REACHED ();
}

/* Evicts a block from it's cache slot and returns the cache_idx of
   the free cache slot. If the evicted block is dirty, it is written
   back to disk. 
   
   The rw_lock of the cache slot is held in shared_acquire mode after
   this function returns. */
static size_t
cache_evict_block (void)
{
  size_t cache_idx = clock_find ();
  struct cache_entry *ce = cache_metadata + cache_idx;
  
  /* Write dirty block back to disk. */
  if (ce->dirty)
    {
      void *cache_slot = cache_idx_to_cache_slot (cache_idx);
      block_write (fs_device, ce->sector_idx, cache_slot);
    }
  
  /* Clear appropriate fields in cache_entry. */
  ce->sector_idx = SIZE_MAX;
  ce->dirty = false;
  ce->accessed = false;

  /* Convert exclusive_acquire on rw_lock to shared_acquire so
     that all paths through cache_load() return a cache slot with
     shared_acquire on the rw_lock. */
  rw_lock_exclusive_release (&ce->rw_lock);
  rw_lock_shared_acquire (&ce->rw_lock);

  return cache_idx;
}

/* Searches the cache_metadata to see if a block with sector number
   SECTOR is already loaded into the cache. If yes, the rw_lock of
   the cache_entry is obtained via shared_acquire, and the cache_idx
   of the loaded block is returned. Otherwise, return an error value
   indicating the requested block is not present in the cache. */
static size_t
cache_find_block (block_sector_t sector)
{
  for (size_t idx = 0; idx < CACHE_SIZE; idx++)
    {
      struct cache_entry *ce = cache_metadata + idx;
      rw_lock_shared_acquire (&ce->rw_lock);
      if (ce->sector_idx == sector)
        return ce->cache_idx;
      rw_lock_shared_release (&ce->rw_lock);
    }

  return BLOCK_NOT_PRESENT;
}

/* Find a block with sector number SECTOR in the cache and return
   the cache_idx of the slot it is in, or load the block from disk
   into a slot if it isn't already in the cache. Evict a different
   block from the cache if necessary. 
   
   The rw_lock of the cache slot is held in shared_acquire mode
   after this function returns. */
static size_t
cache_load (block_sector_t sector)
{
  size_t cache_idx;
  struct cache_entry *ce;

  /* Block already in cache, so just return the cache_idx. */
  cache_idx = cache_find_block (sector);
  if (cache_idx != BLOCK_NOT_PRESENT)
    return cache_idx;

  /* Block not in cache, so find a free slot and load it in. */
  cache_idx = bitmap_scan_and_flip (cache_bitmap, 0, 1, false);

  /* A free cache slot is available, so obtain the rw_lock on the
     cache_entry and set the sector_idx field. */
  if (cache_idx != BITMAP_ERROR)
    {
      ce = cache_metadata + cache_idx;
      rw_lock_shared_acquire (&ce->rw_lock);
      void *cache_slot = cache_idx_to_cache_slot (cache_idx);
      block_read (fs_device, sector, cache_slot);
      return cache_idx;
    }

  /* Cache is full, so evict a block from it's cache slot to
     obtain a free slot for the new block. */
  cache_idx = cache_evict_block ();
  void *cache_slot = cache_idx_to_cache_slot (cache_idx);
  block_read (fs_device, sector, cache_slot);
  return cache_idx;
}

/* Signals the read-ahead worker thread that a block has been
   enqueued to be loaded into the cache. */
void
read_ahead_signal (block_sector_t sector)
{
  struct sector_elem *se = malloc (sizeof (struct sector_elem));
  if (se == NULL)
    PANIC ("read_ahead_signal: memory allocation failed for sector_elem.");

  se->sector = sector;
  list_push_back (&read_ahead_list, &se->elem);
  sema_up (&read_ahead_sema);
}

/* A thread function that automatically fetches the next block
   of a file into the cache when one block of a file is read.
  
   The read-ahead worker thread keeps track of a list of blocks
   to pre-fetch, and sleeps until signaled by another process
   that the pre-fetch list is non-empty. */
static void
cache_read_ahead (void *aux UNUSED)
{
  while (true)
    {
      sema_down (&read_ahead_sema);
      ASSERT (!list_empty (&read_ahead_list));
      
      struct list_elem *e = list_pop_front (&read_ahead_list);
      struct sector_elem *s = list_entry (e, struct sector_elem, elem);

      // cache_load (s->sector);
      // READ-AHEAD NOT YET USED
      (void) s;
    }
}

/* A thread function that periodically writes all dirty blocks in
   the cache back to disk.
   
   The periodic-flush worker repeatedly sleeps for a specified
   amount of time (default 10 seconds) then wakes up and flushes
   the cache. */
static void
cache_periodic_flush (void *aux UNUSED)
{
  while (true)
    {
      timer_msleep (10 * 1000);
      cache_flush ();
    }
}
