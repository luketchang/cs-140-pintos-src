#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/fd.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/p_info.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/frame.h"
#include "vm/mmap.h"
#include "vm/page.h"
#include "vm/swap.h"

/* Limit on size of individual command-line argument. */
#define ARGLEN_MAX 128

/* Argument passing. 

   The command line is parsed into arguments separated by
   null terminator characters. All whitespace characters
   are discarded. */
static char *cmd_args;         /* The parsed command line. */
static int cmd_args_len;       /* Length of CMD_ARGS. */
static int argc;               /* Argument count. */

static thread_func start_process NO_RETURN;
static bool load (const char *cmd_args, void (**eip) (void), void **esp);

static void free_child_p_info_list (void);

/* Traverse through current thread's child_p_info_list and 
   deallocate resources of all p_info structs. */
static void
free_child_p_info_list (void)
{
  struct thread *t = thread_current ();

  while (!list_empty (&t->child_p_info_list))
    {
      struct list_elem *curr = list_pop_front (&t->child_p_info_list);
      struct p_info *p_info = list_entry (curr, struct p_info, elem);
      list_remove (curr);
      free (p_info->sema);
      free (p_info);
      p_info = NULL;
    }
}

/* Starts a new thread running a user program loaded from the 
   file that is the first argument in CMD. The new thread may
   be scheduled (and may even exit) before process_execute()
   returns.  Returns the new process's thread id, or TID_ERROR
   if the thread cannot be created. */
tid_t
process_execute (const char *cmd) 
{
  char *cmd_copy;
  tid_t tid;

  /* Make a copy of CMD. Otherwise there's a race between the
     caller and load(). */
  cmd_copy = palloc_get_page (0);
  if (cmd_copy == NULL)
    return TID_ERROR;
  strlcpy (cmd_copy, cmd, PGSIZE);

  cmd_args_len = 0;
  argc = 0;
  char *arg;
  char *save_ptr;

  /* Parse CMD_COPY and store in CMD_ARGS. Obtain length of
     CMD_ARGS and number of arguments and store in CMD_ARGS_LEN
     and ARGC, respectively. */
  cmd_args = palloc_get_page (0);
  if (cmd_args == NULL) 
    {
      palloc_free_page (cmd_copy);
      return TID_ERROR;
    }
  for (arg = strtok_r (cmd_copy, " ", &save_ptr); arg != NULL;
       arg = strtok_r (NULL, " ", &save_ptr), argc++)
    {
      int next_arg_len = strlen (arg) + 1;

      /* Verify each argument is less than 128 bytes, which is the
         limit on what the pintos utility can pass to the kernel. */
      if (next_arg_len > ARGLEN_MAX) 
        {
          palloc_free_page (cmd_copy);
          palloc_free_page (cmd_args);
          return TID_ERROR;
        }
      strlcpy (cmd_args + cmd_args_len, arg, next_arg_len);
      cmd_args_len += next_arg_len;
    }

  /* Check that the stack page will not overflow from putting
     arguments onto the stack according to the 80x86 calling
     convention. The value 4 makes room for the null pointer
     sentinel, argv, argc, and the return address. */
  int bytes_total = cmd_args_len + PTR_SIZE * (argc + 4); 
  if (bytes_total > PGSIZE)
    {
      palloc_free_page (cmd_copy);
      palloc_free_page (cmd_args);
      return TID_ERROR;
    }

  /* Create a new thread to execute CMD. */
  tid = thread_create (cmd_args, PRI_DEFAULT, start_process, NULL);

  /* Block on child's p_info semaphore until child has confirmed
     successful load in call to start_process. Return -1 if failed. */
  struct p_info *child_p_info = child_p_info_by_tid (tid);
  sema_down (child_p_info->sema);
  if (!child_p_info->load_succeeded || tid == TID_ERROR)
    {
      palloc_free_page (cmd_copy);
      return TID_ERROR;
    }
  
  palloc_free_page (cmd_copy);
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *aux UNUSED)
{
  struct intr_frame if_;
  bool success;

  struct thread *t = thread_current ();

  /* Initialize fd list, child process info list,
     supplemental page table, and mmap_list. */
  t->fd_counter = 2;
  list_init (&t->fd_list);
  list_init (&t->child_p_info_list);
  spt_init (&t->spt);
  list_init (&t->mmap_list);
  t->mapid_counter = 0;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  // lock_acquire (&filesys_lock);
  success = load (cmd_args, &if_.eip, &if_.esp);
  // lock_release (&filesys_lock);
  palloc_free_page (cmd_args);

  /* If load was successful, set load_succeeded to true. */
  if (success)
    thread_current ()->p_info->load_succeeded = true;
  
  /* Notify parent that load finished regardless of success/fail. */
  sema_up (thread_current ()->p_info->sema);

  /* If load failed, release resources and quit. */
  if (!success) 
    thread_exit ();

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting. */
int
process_wait (tid_t tid) 
{
  struct p_info *child_p_info = child_p_info_by_tid (tid);

  /* Already waited on child and freed struct. */
  if (child_p_info == NULL) 
    return -1;
  
  /* Down child's semaphore. Once unblocked, free p_info
     struct and return exit status. If this process tries to
     wait on same tid again, it will hit p_info == NULL and 
     return -1 as intended. */
  sema_down (child_p_info->sema);

  int exit_status = child_p_info->exit_status;
  list_remove (&child_p_info->elem);
  free (child_p_info);
  child_p_info = NULL;

  return exit_status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *t = thread_current ();
  uint32_t *pd;

/* Free all process resources. Unmap mmapped files, 
   free open fd table, free child p_info structs, 
   free spt table, and close file to allow writes to 
   executable again. */ 
  munmap_all ();
  free_fd_list ();
  free_child_p_info_list ();
  spt_free_table (&t->spt);

  // if (!lock_held_by_current_thread (&filesys_lock))
    // lock_acquire (&filesys_lock);
  file_close (t->executable);
  // lock_release (&filesys_lock);
  
  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = t->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      t->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable which is the first
   argument in CMD_ARGS into the current thread.
   Sets up arguments on the stack using CMD_ARGS.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *cmd_args, void (**eip) (void), void **esp)
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (cmd_args);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", cmd_args);
      goto done; 
    }
  
  /* If valid executable to be run, deny writes and set
     struct thread's executable field. */
  file_deny_write (file);
  t->executable = file;

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", cmd_args);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Maps a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are mapped and recorded in the supplemental page table
   of the process, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   The pages will be lazily loaded into physical memory when the
   process faults on them.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  off_t pos = ofs;
  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Create supplemental page table entry for this page
         and add it to SPT of current process. */
      enum location loc = (page_zero_bytes == PGSIZE) ? ZERO : DISK;
      struct spte *spte = spte_create (upage, loc, file, pos, SWAP_DEFAULT,
                                       page_read_bytes, writable, false);
      spt_insert (&thread_current ()->spt, &spte->elem);

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
      pos += PGSIZE;
    }

  return true;
}

/* Create a stack by mapping a zeroed page at the top of user
   virtual memory and placing arguments on the stack according
   to the 80x86 calling convention. */
static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;
  
  /* Allocate and initialize first stack page at load time. */
  uint8_t *upage = ((uint8_t *) PHYS_BASE) - PGSIZE;
  struct spte *spte = spte_create (upage, STACK, NULL, 0, SWAP_DEFAULT,
                                   PGSIZE, true, true);
  spt_insert (&thread_current ()->spt, &spte->elem);

  kpage = frame_alloc_page (PAL_USER | PAL_ZERO, spte);
  if (kpage != NULL)
    {
      success = install_page (upage, kpage, true);
      if (success)
        {
          *esp = PHYS_BASE - 1;
          int arg_len[argc];
          int index = 0;
          int len = 0;

          /* Push argument bytes onto stack in reverse order and get
             length of each argument (including null terminator). */
          for (char *cmd_char = cmd_args + cmd_args_len - 1;
               cmd_char != cmd_args - 1; (*esp)--)
            {
              *((uint8_t *) *esp) = *cmd_char--;
              len++;
              if (*cmd_char == 0 && cmd_char != cmd_args - 1)
                {
                  arg_len[index++] = len;
                  len = 0;
                }
            }
          arg_len[index] = len;

          /* Round stack pointer down to multiple of 4. */
          int word_align = (PTR_SIZE - cmd_args_len) % PTR_SIZE;
          for (int i = 0; i < word_align; i++, (*esp)--) 
            *((uint8_t *) *esp) = 0;
          (*esp) -= PTR_SIZE - 1;

          /* Push null pointer sentinel to ensure that argv[argc]
             is a null pointer, as required by the C standard. */
          *((uintptr_t *) *esp) = 0;
          (*esp) -= PTR_SIZE;

          /* Push address of each argument string. */
          void *start = PHYS_BASE;
          for (int argnum = 0; argnum < argc; argnum++) 
            {
              *((uintptr_t *) *esp) = (uintptr_t) (start - arg_len[argnum]);
              (*esp) -= PTR_SIZE;
              start -= arg_len[argnum];
            }

          /* Push argv (address of argv[0]), argc, and a "return address",
             in that order, to complete the stack. */
          *((uintptr_t *) *esp) = (uintptr_t) (*esp + PTR_SIZE);
          (*esp) -= PTR_SIZE;
          *((uintptr_t *) *esp) = argc;
          (*esp) -= PTR_SIZE;
          *((uintptr_t *) *esp) = 0;
        }
      else
        frame_free_page (kpage);
    }

  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
