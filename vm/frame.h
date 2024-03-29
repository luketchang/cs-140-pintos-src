#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/palloc.h"
#include "threads/synch.h"
#include "vm/page.h"

/* Frame table entry. */
struct frame_entry
  {
     void *page_kaddr;       /* Kernel virtual address of the frame. */
     struct spte *spte;      /* Reference to SPT entry for page in frame. */
     struct thread *thread;  /* Reference to process using the frame. */
     struct lock lock;       /* A lock to allow a process to pin the frame. */
  };

struct frame_entry *page_kaddr_to_frame_addr (void *page_kaddr);
void frame_table_init (void);
void *frame_alloc_page (enum palloc_flags flags, struct spte *spte);
void frame_free_page (void *page_kaddr);

void pin_frames (const void *buffer, int length);
void unpin_frames (const void *buffer, int length);

#endif /* vm/frame.h */
