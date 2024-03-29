/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Default value in a lock without any donations. */
#define NO_DONATIONS_PRI -1

static void donate_priority (struct lock *lock, int priority);

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
     decrement it.

   - up or "V": increment the value (and wake up one waiting
     thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) 
{
  ASSERT (sema != NULL);

  sema->value = value;
  list_init (&sema->waiters);
}

/* Initializes a semaphore as a list element, which allows one thread
   to signal one or more other threads through the use of a condition
   variable containing a list of such semaphores. */
void
thread_sema_pair_init (struct thread_sema_pair *thread_sema_pair)
{
  ASSERT (thread_sema_pair != NULL);

  thread_sema_pair->waiting_thread = thread_current ();
  sema_init (&thread_sema_pair->semaphore, 0);
}

/* Comparison function for semaphores in a list owned by a condition
   variable. Compares by priority of the threads associated with each
   semaphore. Sorts in descending order. */
bool
cmp_thread_sema_pair (const struct list_elem *a, const struct list_elem *b,
                      void *aux UNUSED)
{
  int a_pri = list_entry (a, struct thread_sema_pair, elem)
                          ->waiting_thread->curr_priority;
  int b_pri = list_entry (b, struct thread_sema_pair, elem)
                          ->waiting_thread->curr_priority;
  return a_pri > b_pri;
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */
void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
    {
      list_insert_ordered (&sema->waiters, &thread_current ()->elem,
                           cmp_priority, NULL);
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) 
{
  enum intr_level old_level;
  bool success;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (sema->value > 0) 
    {
      sema->value--;
      success = true; 
    }
  else
    success = false;
  intr_set_level (old_level);

  return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up the thread with highest priority waiting for
   SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  sema->value++;
  if (!list_empty (&sema->waiters)) 
    {
      list_sort (&sema->waiters, cmp_priority, NULL);
      thread_unblock (list_entry (list_pop_front (&sema->waiters),
                                  struct thread, elem));
    }
  intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) 
{
  struct semaphore sema[2];
  int i;

  printf ("Testing semaphores...");
  sema_init (&sema[0], 0);
  sema_init (&sema[1], 0);
  thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) 
    {
      sema_up (&sema[0]);
      sema_down (&sema[1]);
    }
  printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) 
{
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) 
    {
      sema_down (&sema[0]);
      sema_up (&sema[1]);
    }
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);

  lock->holder = NULL;
  lock->donated_priority = NO_DONATIONS_PRI;
  sema_init (&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread. With the round-robin scheduler enabled, if the thread
   desiring LOCK has a higher priority than the thread holding
   the lock, the higher priority is donated to the lock holder.
   Nested donation is implemented, in which donations are passed
   on to subsequent threads so long as those threads desire some
   other lock.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  enum intr_level old_level = intr_disable ();

  if (lock->holder != NULL)
    {
      thread_current ()->desired_lock = lock;
      
      /* Priority donation disabled for advanced scheduler. */
      if (!thread_mlfqs)
        {
          int acq_priority = thread_current ()->curr_priority;
          if (acq_priority > lock->holder->curr_priority) 
            {
              /* Reassign lock to avoid compiler optimizing out while loop. */
              struct lock *curr_lock = lock; 
              while (curr_lock != NULL)
                {
                  donate_priority (curr_lock, acq_priority); 
                  curr_lock = curr_lock->holder->desired_lock;
                }
            }
        }
  }

  /* Blocks thread if priority was donated. */
  sema_down (&lock->semaphore);

  /* Lock is free to be acquired. */
  lock->holder = thread_current (); 
  thread_current ()->desired_lock = NULL;
  list_push_back (&thread_current ()->held_locks, &lock->elem);

  intr_set_level (old_level);
}

/* Acquire lock if not held and return true if lock newly acquired. */
bool
lock_acquire_in_context (struct lock *lock)
{
  if (!lock_held_by_current_thread (lock))
    {
      lock_acquire (lock);
      return true;
    }

  return false;
}

/* Sets priority of lock holder to donated priority. Sets lock's
   priority to donated priority. Updates (reorders) holder's
   held_locks list. Sorts the thread library's ready queue to
   reflect new priority of donee. 
   
   Function can only called by the round-robin scheduler. */
static void 
donate_priority (struct lock *lock, int priority)
{
  enum intr_level old_level = intr_disable ();

  struct thread *holder = lock->holder;
  thread_set_donated_priority (holder, priority);

  /* A thread's num_donations corresponds to number of held locks
     with priority donations. */
  if (lock->donated_priority == NO_DONATIONS_PRI)  
    holder->num_donations++;
  
  /* Donated priority always highest so corresponding lock should 
     be moved to front of held_locks list owned by thread. */
  lock->donated_priority = priority;
  list_remove (&lock->elem);
  list_push_front (&holder->held_locks, &lock->elem);

  intr_set_level (old_level);
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock)
{
  bool success;

  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  success = sema_try_down (&lock->semaphore);
  if (success)
    lock->holder = thread_current ();
  return success;
}

/* Releases LOCK, which must be owned by the current thread. 
   If using the round-robin scheduler, the current thread also 
   releases the donation associated with LOCK, if any, and
   resets it's priority.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));
  
  enum intr_level old_level = intr_disable ();

  struct thread *t = thread_current ();
  list_remove (&lock->elem);

  /* Thread had donation for this lock, so change curr_priority
     based on whether there are other donations or not. Disabled
     for the multi-level feedback queue scheduler. */
  if (!thread_mlfqs)
    {
      if (t->num_donations > 0 && lock->donated_priority != NO_DONATIONS_PRI)
        {
          t->num_donations--;
          if (t->num_donations == 0) 
            t->curr_priority = t->owned_priority;
          else 
            {
              struct list_elem *lock_elem = list_front (&t->held_locks);
              struct lock *next = list_entry (lock_elem, struct lock, elem);
              t->curr_priority = next->donated_priority;
            }
        }
    }

  /* Signal the waiting thread of highest priority that lock is
     now available. */
  lock->holder = NULL;
  lock->donated_priority = NO_DONATIONS_PRI;
  sema_up (&lock->semaphore);

  intr_set_level (old_level);
}

/* Releases lock if RELEASE is true. */
void 
lock_conditional_release (struct lock *lock, bool release)
{
  if (release)
    lock_release (lock);
}

/* Returns true if the current thread holds LOCK, false otherwise.
   (Note that testing whether some other thread holds lock would
   be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);

  return lock->holder == thread_current ();
}

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);

  list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) 
{
  struct thread_sema_pair waiter;

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  thread_sema_pair_init (&waiter);
  list_insert_ordered (&cond->waiters, &waiter.elem,
                       cmp_thread_sema_pair, NULL);
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals the one with highest priority to wake up
   from its wait. LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  if (!list_empty (&cond->waiters)) 
    {
      list_sort (&cond->waiters, cmp_thread_sema_pair, NULL);
      sema_up (&list_entry (list_pop_front (&cond->waiters),
                            struct thread_sema_pair, elem)->semaphore);
    }
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}

/* Initializes RW_LOCK. A rw_lock allows concurrent access for
   readers, while writers require exclusive access. Thus, when
   a writer has acquired the rw_lock, all other readers/writers
   will be blocked until the writer holding the rw_lock has
   finished writing and released it.

   The rw_lock is implemented with a condition variable and an
   ordinary lock. It is fair, which means that priority is 
   adaptive and will be given to readers if there have been many
   consecutive writers, and vice versa. */
void
rw_lock_init (struct rw_lock *rw_lock)
{
  lock_init (&rw_lock->lock);
  cond_init (&rw_lock->cond);
  rw_lock->active_readers = 0;
  rw_lock->waiting_readers = 0;
  rw_lock->waiting_writers = 0;
  rw_lock->consec_readers = 0;
  rw_lock->consec_writers = 0;
  rw_lock->writer = NULL;
}

/* Acquires RW_LOCK as a reader, sleeping until it becomes available
   if necessary. Sleeping will occur if the rw_lock is currently held
   by a writer, or if there are one or more writers waiting to
   acquire the rw_lock and there have been 5 or more consecutive
   readers. After the rw_lock is acquired, the active_readers counter
   is incremented. */
void 
rw_lock_shared_acquire (struct rw_lock *rw_lock)
{  
  lock_acquire (&rw_lock->lock);
  rw_lock->waiting_readers++;

  while (rw_lock->writer != NULL || 
         (rw_lock->waiting_writers > 0 && rw_lock->consec_readers >= 5))
    cond_wait (&rw_lock->cond, &rw_lock->lock);
  
  if (rw_lock->consec_writers > 0)
    rw_lock->consec_writers = 0;
  rw_lock->consec_readers++;

  rw_lock->waiting_readers--;
  rw_lock->active_readers++;
  lock_release (&rw_lock->lock);
}

/* Attempts to acquire RW_LOCK as a reader. Returns true if
   successful and false otherwise. */
bool 
rw_lock_shared_try_acquire (struct rw_lock *rw_lock)
{
  lock_acquire (&rw_lock->lock);
  
  if (rw_lock->writer != NULL || rw_lock->waiting_writers > 0)
    {
      lock_release (&rw_lock->lock);
      return false;
    }

  rw_lock->active_readers++;
  lock_release (&rw_lock->lock);
  return true;
}

/* Releases RW_LOCK as a reader. The active_readers counter is
   decremented, and if it equals zero after the decrement,
   all readers and writers waiting for the rw_lock are signaled. */
void
rw_lock_shared_release (struct rw_lock *rw_lock)
{
  lock_acquire (&rw_lock->lock);
  rw_lock->active_readers--;

  if (rw_lock->active_readers == 0)
    cond_broadcast (&rw_lock->cond, &rw_lock->lock);

  lock_release (&rw_lock->lock);
}

/* Atomically converts shared hold on RW_LOCK to exclusive hold.
   Acquires rw_lock's internal lock, decrements the active_readers
   counter, waits to acquire exclusive hold on rw_lock, then
   releases internal lock. */
void 
rw_lock_shared_to_exclusive (struct rw_lock *rw_lock)
{
  lock_acquire (&rw_lock->lock);
  rw_lock->active_readers--;
  rw_lock->waiting_writers++;

  while (rw_lock->active_readers > 0 || rw_lock->writer != NULL)
    cond_wait (&rw_lock->cond, &rw_lock->lock);

  rw_lock->waiting_writers--;
  rw_lock->writer = thread_current ();
  lock_release (&rw_lock->lock);
}

/* Acquires RW_LOCK as a writer, sleeping until it becomes available
   if necessary. If the writer sleeps, the waiting_writers counter
   is incremented. After acquiring the rw_lock's internal lock, the
   waiting_writers counter is decremented and the writer field is set
   to the calling thread. Note that there is a cap of 10 consecutive
   writers that can run in the face of waiting readers before access
   must be transferred back to readers. */
void
rw_lock_exclusive_acquire (struct rw_lock *rw_lock)
{
  lock_acquire (&rw_lock->lock);
  rw_lock->waiting_writers++;

  while (rw_lock->writer != NULL ||
         rw_lock->active_readers > 0 || 
         (rw_lock->consec_writers >= 10 && rw_lock->waiting_readers > 0))
    cond_wait (&rw_lock->cond, &rw_lock->lock);

  if(rw_lock->consec_readers > 0)
    rw_lock->consec_readers = 0;
  rw_lock->consec_writers++;

  rw_lock->waiting_writers--;
  rw_lock->writer = thread_current ();
  lock_release (&rw_lock->lock);
}

/* Releases RW_LOCK as a writer. The writer field is set to NULL and
   all readers and writers waiting for the rw_lock are signaled. */
void
rw_lock_exclusive_release (struct rw_lock *rw_lock)
{
  lock_acquire (&rw_lock->lock);
  ASSERT (current_thread_is_writer (rw_lock));

  rw_lock->writer = NULL;
  cond_broadcast (&rw_lock->cond, &rw_lock->lock);
  lock_release (&rw_lock->lock);
}

/* Atomically converts exclusive hold on RW_LOCK to shared hold.
   Acquires rw_lock's internal lock, sets the rw_lock's writer field 
   to NULL, increments the active_readers counter, then releases
   internal lock. */
void 
rw_lock_exclusive_to_shared (struct rw_lock *rw_lock)
{
  lock_acquire (&rw_lock->lock);
  ASSERT (current_thread_is_writer (rw_lock));

  rw_lock->writer = NULL;
  rw_lock->active_readers++;
  lock_release (&rw_lock->lock);
}

/* Returns true if current thread is writer for RW_LOCK and false 
   otherwise. */
bool
current_thread_is_writer (struct rw_lock *rw_lock)
{
  return rw_lock->writer == thread_current ();
}
