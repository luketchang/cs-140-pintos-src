#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <debug.h>
#include <list.h>
#include <stdbool.h>

/* A counting semaphore. */
struct semaphore 
  {
    unsigned value;             /* Current value. */
    struct list waiters;        /* List of waiting threads. */
  };

/* Semaphore element held in condition variable's waiters list.
   Element created and inserted into condition variable waiters
   in call to cond_wait() and corresponding thread unblocked in 
   cond_signal() through call to sema_up(). */
struct thread_sema_pair 
  {

    struct semaphore semaphore;         /* This semaphore. */
    struct thread *waiting_thread;      /* Waiting thread. */
    struct list_elem elem;              /* List element. */
  };

void sema_init (struct semaphore *, unsigned value);
void thread_sema_pair_init (struct thread_sema_pair *);
void sema_down (struct semaphore *);
bool sema_try_down (struct semaphore *);
void sema_up (struct semaphore *);
void sema_self_test (void);
bool cmp_thread_sema_pair (const struct list_elem *a,
                           const struct list_elem *b, void *aux UNUSED);

/* Lock. */
struct lock 
  {
    struct thread *holder;      /* Thread holding lock (for debugging). */
    int donated_priority;       /* Highest donated priority for this lock.
                                   Has value -1 if there are no donations. */
    struct semaphore semaphore; /* Binary semaphore controlling access. */
    struct list_elem elem;      /* List element. */
  };

void lock_init (struct lock *);
void lock_acquire (struct lock *);
bool lock_acquire_in_context (struct lock *);
bool lock_try_acquire (struct lock *);
void lock_release (struct lock *);
void lock_conditional_release (struct lock *, bool);
bool lock_held_by_current_thread (const struct lock *);

/* Condition variable. */
struct condition 
  {
    struct list waiters;        /* List of waiting threads. */
  };

void cond_init (struct condition *);
void cond_wait (struct condition *, struct lock *);
void cond_signal (struct condition *, struct lock *);
void cond_broadcast (struct condition *, struct lock *);

/* Fair readers-writer lock. */
struct rw_lock
  {
    struct lock lock;             /* Lock for atomic increments/decrements. */
    struct condition cond;        /* Condition variable. */
    size_t active_readers;        /* Number of readers holding rw_lock. */
    size_t waiting_readers;       /* Number of readers waiting for rw_lock. */
    size_t waiting_writers;       /* Number of writers waiting for rw_lock. */
    struct thread *writer;        /* Writer thread holding the rw_lock. */
    size_t consec_readers;        /* Number of consecutive readers. */
    size_t consec_writers;        /* Number of consecutive writers. */
  };

void rw_lock_init (struct rw_lock *);
void rw_lock_shared_acquire (struct rw_lock *);
bool rw_lock_shared_try_acquire (struct rw_lock *);
void rw_lock_shared_release (struct rw_lock *);
void rw_lock_shared_to_exclusive (struct rw_lock *);
void rw_lock_exclusive_acquire (struct rw_lock *);
void rw_lock_exclusive_release (struct rw_lock *);
void rw_lock_exclusive_to_shared (struct rw_lock *);
bool current_thread_is_writer (struct rw_lock *);

/* Optimization barrier.

   The compiler will not reorder operations across an
   optimization barrier.  See "Optimization Barriers" in the
   reference guide for more information.*/
#define barrier() asm volatile ("" : : : "memory")

#endif /* threads/synch.h */
