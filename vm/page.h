#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <debug.h>
#include <hash.h>
#include "filesys/file.h"

/* Location of a page. */
enum location
  {
    SWAP,     /* In a swap slot on the swap partition. */
    DISK,     /* Stored in a file on disk. */
    ZERO,     /* A zero page. */
    FRAME     /* Page in a frame in physical memory. */
  };

/* Supplemental page table (SPT) entry. */
struct spte
  {
    void *page_uaddr;       /* User virtual address of the page. */
    enum location loc;      /* Location of the page. */
    struct file *file;      /* Reference to file if page on disk. */
    off_t ofs;              /* Offset in file if page on disk. */
    size_t page_bytes;      /* Number of non-zero bytes in page. */
    bool writable;          /* Indicates whether page is writable. */
    struct hash_elem elem;  /* Hash element. */
  };

void spt_init (struct hash *hash_table);
void spt_insert (struct hash *spt, struct hash_elem *he);
void spt_free_table (struct hash *spt);

struct spte *spte_create (void *page_uaddr, enum location loc,
                          struct file* file, off_t ofs,
                          size_t page_bytes, bool writable);
struct spte *spte_lookup (void *page_uaddr);
void spte_free (struct hash_elem *he, void *aux UNUSED);

#endif /* vm/page.h */