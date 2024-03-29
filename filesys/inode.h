#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <list.h>
#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"
#include "threads/synch.h"

/* The total number of sectors (indir_block or data) that
   can be pointed to by an inode_disk struct. */
#define INODE_SECTORS 125

/* The number of data sectors that can be pointed to
   directly by an inode_disk struct (direct block). */
#define NUM_DIRECT 123

/* The number of data sectors that can be pointed to by
   an indir_block struct (indirect block). */
#define NUM_INDIRECT 128

/* Constants needed for byte to sector calculations. */
#define INDIR NUM_DIRECT
#define DOUBLE_INDIR (NUM_DIRECT + 1)
#define NUM_DIR_INDIR (NUM_DIRECT + NUM_INDIRECT)
#define NUM_FILE_MAX (NUM_DIR_INDIR + (NUM_INDIRECT * NUM_INDIRECT))

/* Type of file. */
enum inode_type
  {
    FREEMAP,
    FILE,
    DIR
  };

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    off_t length;                           /* File size in bytes. */
    unsigned magic;                         /* Magic number. */
    enum inode_type type;                   /* Directory, file, or freemap? */
    block_sector_t sectors[INODE_SECTORS];  /* Sector entries. */
  };

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    enum inode_type type;               /* Directory, file, or freemap? */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct lock lock;                   /* Lock for sequential operations. */
  };

/* An indirect block contains sector numbers which refer
   to blocks that contain actual data.
   
   Must be exactly BLOCK_SECTOR_SIZE bytes in size. */
struct indir_block
  {
    block_sector_t sectors[NUM_INDIRECT];
  };

struct bitmap;

void inode_init (void);
bool inode_create (block_sector_t, off_t, enum inode_type);
struct inode *inode_open (block_sector_t);
struct inode *inode_reopen (struct inode *);
block_sector_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);

#endif /* filesys/inode.h */
