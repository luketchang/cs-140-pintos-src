#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "filesys/inode.h"
#include "devices/block.h"
#include "threads/synch.h"

/* Number of sectors that fit in the cache. */
#define CACHE_SIZE 64

/* Error value indicating block not found in cache. */
#define BLOCK_NOT_PRESENT SIZE_MAX

/* Error value indicating unused sector entry. */
#define SECTOR_NOT_PRESENT SIZE_MAX

/* Sector type to distinguish between inodes and data. */
enum sector_type
  {
    INODE,  /* An inode sector. */
    DATA    /* A data sector. */
  };

/* Cache entry. */
struct cache_entry
  {
    enum sector_type type;      /* Sector type (inode or data). */
    block_sector_t sector_idx;  /* Sector number of disk location. */
    size_t cache_idx;           /* Index in the buffer cache. */
    bool dirty;                 /* Dirty flag for writes. */
    bool accessed;              /* Flag for reads or writes. */
    struct rw_lock rw_lock;     /* Readers-writer lock. */
  };

/* Block sector element that allows block sector numbers
   to be placed in a list. */
struct sector_elem 
  {
    block_sector_t sector;      /* Sector number of disk location. */
    struct list_elem elem;      /* List element. */
  };

void cache_init (void);

void *cache_idx_to_cache_block_addr (size_t cache_idx);
struct cache_entry *cache_idx_to_cache_entry (size_t cache_idx);

void *cache_get_block_exclusive (block_sector_t sector, enum inode_type type);
void *cache_get_block_shared (block_sector_t sector, enum inode_type type);
void cache_exclusive_release (void *block_addr);
void cache_shared_release (void *block_addr);

void cache_shared_to_exclusive (void *block_addr);
void cache_exclusive_to_shared (void *block_addr);
void cache_conditional_release (void *block_addr, bool exclusive);

size_t cache_get_block (block_sector_t sector, enum sector_type type);
void cache_free_slot (block_sector_t sector);
void cache_flush (void);

void read_ahead_signal (block_sector_t sector);

#endif /* filesys/cache.h */
