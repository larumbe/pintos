#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "devices/timer.h"
#include "list.h"
#include "fixpoint.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "tests/threads/tests.h"

#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* priority queues */
static struct list ready_queues[NQ];

/* List of processes in the THREAD_BLOCKED state waiting
  for events to happen, like timer expiration */
static struct list waiting_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* represented in p.q fixed point format */
int load_avg;
static struct lock load_avg_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */
static long long total_ticks;	/* for process aging */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void)
{
  unsigned int i;

  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init (&waiting_list);

  if (thread_mlfqs) {
    for (i = 0; i < NQ; i++)
      list_init (&ready_queues[i]);
  }

  lock_init (&load_avg_lock);
  load_avg = 0;

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void)
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* idle thread should not be checked when traversing lists of all threads
     eg: when recalculating properties or when scheduling, since it has
     a separate pointer to itself, and receives no accounting information */
  if (!thread_mlfqs)
    idle_thread = list_entry (list_front (&ready_list), struct thread, elem);
  else
    idle_thread = list_entry (list_front (&ready_queues[PRI_MIN]), struct thread, elem);
  list_remove(&idle_thread->elem);
  list_remove(&idle_thread->allelem);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  printf("Waiting for semaphore\n");
  sema_down (&idle_started);
  printf("Done waiting for semaphore\n");
}

/* reckon beforeness based on nicety, we assume
t->nice has already been given the right value */
static inline int recalculate_priority(struct thread * t)
{
  int priority = convert_int_near(convert_fp(PRI_MAX) -
                                  div_fp_int(t->recent_cpu, 4) -
                                  mul_fp_int(convert_fp(t->nice), 2));
  if (priority < PRI_MIN)
    priority = PRI_MIN;
  else if (priority > PRI_MAX)
    priority = PRI_MAX;

  return priority;
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (int64_t ticks)
{
  struct thread *cur = thread_current ();
  struct thread *sleeper, *t;
  struct list_elem *e;
  int ready_threads;
  int old_priority;
  bool priority_supersded = false;

  /* Update statistics. */
  if (cur == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  if (thread_mlfqs) {
    /* each timer tick, the running thread's recent_cpu is incremented by 1 */
    if (cur != idle_thread)
      cur->recent_cpu = add_fp_int(cur->recent_cpu, 1);

    if (ticks % 4 == 0) {
      if (ticks % TIMER_FREQ == 0) {
        /* replace this with O(1) */
        /* need to reduce the time spent in the irq handler slightly */
        ready_threads = 0;
        list_foreach(e, t, all_list, allelem) {
          if (t->status == THREAD_READY ||
              t->status == THREAD_RUNNING)
            ready_threads++;
        }
        /* load average */
        load_avg = mul_fp(div_fp(convert_fp(59),convert_fp(60)), load_avg) +
          mul_fp(div_fp(convert_fp(1),convert_fp(60)), convert_fp(ready_threads));
      }

      /* Each thread's  priority is recalculated every fourth tick */
      list_foreach(e, t, all_list, allelem) {
        if (t->status != THREAD_NASCENT) {
          if (ticks % TIMER_FREQ == 0)
            t->recent_cpu = add_fp_int(mul_fp(div_fp(mul_fp_int(load_avg, 2),add_fp_int(mul_fp_int(load_avg, 2),1)),t->recent_cpu) , t->nice);

          old_priority = t->priority;
          t->priority = recalculate_priority(t);

          if (t->status == THREAD_READY && (old_priority != t->priority)) {
            list_remove (&t->elem);
            list_push_back(&ready_queues[t->priority], &t->elem);

            if (t->priority > cur->priority)
              priority_supersded = true;
          }
        }
      }
    }
  } else {
      /* priority aging */
    total_ticks++;
    if ((total_ticks % (TIME_SLICE * 4)) == 0) {
      for (e = list_begin (&ready_list);
           e != list_end (&ready_list);
           e = list_next (e)) {
        t = list_entry (e, struct thread, elem);
        if (t->priority < PRI_MAX)
          t->priority++;
      }
    }
  }

  /* control this with a debug macro, rather than commenting it out */
  /* printf("Inside timer interrupt, thread kicks %d\n", thread_ticks); */

  /* iterate over list of sleeping processes and decrease their ticks */
  if (!list_empty(&waiting_list)) {
    e = list_begin (&waiting_list);
    while (e != list_end(&waiting_list)) {
      sleeper = list_entry (e, struct thread, elem);
      sleeper->ticks_wait--;
      if (sleeper->ticks_wait == 0) {
        /* 'e' is now the next element to the deleted one */
        e = list_remove (e);
        if (!thread_mlfqs)
          list_push_back (&ready_list, &sleeper->elem);
        else
          list_push_back (&ready_queues[sleeper->priority], &sleeper->elem);
        sleeper->status = THREAD_READY;
        /* check that priority is higher, then yield on return */
        if (sleeper->priority > cur->priority)
          priority_supersded = true;
        break;
      }
      else
        e = list_next(e);
    }
  }

  /* Enforce preemption. */
  if ((++thread_ticks >= TIME_SLICE) || priority_supersded)
    {
      /* printf("Timer has expired, about next time yield\n"); */
      intr_yield_on_return ();
    }

  /* printf("Thread kicks %d\n", thread_ticks); */

}

/* Prints thread statistics. */
void
thread_print_stats (void)
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux)
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);

  return tid;
}

void thread_wait (int64_t ticks)
{
  struct thread *cur = thread_current ();

  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  cur->status = THREAD_BLOCKED;
  cur->ticks_wait = ticks;

  list_push_back (&waiting_list, &cur->elem);
  schedule ();
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void)
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t)
{
  enum intr_level old_level;
  struct thread *cur;

  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_BLOCKED ||
          t->status == THREAD_NASCENT);

  old_level = intr_disable ();
  cur = thread_current();

  if (!thread_mlfqs)
    list_push_back (&ready_list, &t->elem);
  else
    list_push_back (&ready_queues[t->priority], &t->elem);

  t->status = THREAD_READY;

  if (t->priority > cur->priority
      /* why was I doing this? */
      && !intr_context()) {

    cur->status = THREAD_READY;

    if (!thread_mlfqs)
      list_push_back (&ready_list, &cur->elem);
    else
      list_push_back (&ready_queues[cur->priority], &cur->elem);

    schedule ();
  }

  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void)
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void)
{
  struct thread *t = running_thread ();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void)
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void)
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void)
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;

  ASSERT (!intr_context ());

  old_level = intr_disable ();

  if (cur != idle_thread) {
    if (!thread_mlfqs)
      list_push_back (&ready_list, &cur->elem);
    else
      list_push_back (&ready_queues[cur->priority], &cur->elem);
  }
  cur->status = THREAD_READY;
  schedule ();

  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

bool priority_less (const struct list_elem *a,
                    const struct list_elem *b,
                    void *aux)
{
  (void) aux;

  struct thread *l1 = list_entry (a, struct thread, elem);
  struct thread *l2 = list_entry (b, struct thread, elem);

  return (l1->priority < l2->priority);
}

static inline void
thread_assign_priority(int new_priority, struct thread *cur)
{
  struct thread *t;
  int old_priority;
  int i;

  old_priority = cur->priority;
  cur->priority = new_priority;

  /* T: this has to change to reflect mq's*/
  if (thread_mlfqs && (new_priority < old_priority)) {
    for (i = old_priority; i > new_priority; i--) {
      if (!list_empty (&ready_queues[i]))
        thread_yield();
    }
  } else {
    cur->priority_orig = new_priority;
    /* yield if no longer max priority in the ready list */
    t = list_entry (list_max (&ready_list, priority_less, NULL),
                    struct thread, elem);
    if (cur->priority < t->priority)
      thread_yield ();
  }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority)
{
  struct thread *cur = thread_current ();

  if (thread_mlfqs)
    return;

  /* check we aren't being donated priority through a lock */
  if (cur->num_lock_donors &&
      new_priority <= cur->priority)
    cur->priority_orig = new_priority;
  else
    thread_assign_priority(new_priority, cur);
}

/* Returns the current thread's priority. */
int
thread_get_priority (void)
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice)
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;

  ASSERT (nice >= -20 && nice <= 20);

  if (thread_mlfqs) {
    cur->nice = nice;
    old_level = intr_disable ();
    thread_assign_priority (recalculate_priority (cur), cur);
    intr_set_level (old_level);
  }
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void)
{
  return thread_current()->nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void)
{
  int32_t lah;                   /* load average hundred times */
  enum intr_level old_level;

  old_level = intr_disable ();
  lah = convert_int_near(mul_fp_int(load_avg, 100));
  intr_set_level (old_level);

  return lah;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void)
{
  return (convert_int_near(mul_fp_int(thread_current()->recent_cpu, 100)));
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED)
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;)
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
        /* printf ("Idle started.\n"); */

      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux)
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void)
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

static inline bool
is_main_thread (struct thread *t)
{
  return !strcmp(t->name, "main");
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_NASCENT;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;

  t->parent = (!is_main_thread(t)) ? thread_current() : t;

  /* The initial thread starts with a nice value of zero.  Other threads start
     with a nice value inherited from their parent thread. */
  if (!thread_mlfqs)
    t->priority_orig = t->priority = priority;
  else {
    if (!is_main_thread(t)) {
      t->nice = thread_current()->nice;
      t->recent_cpu = thread_current()->recent_cpu;
    } else {
      t->nice = 0;
      t->recent_cpu = 0;
    }
    if (strcmp(name, "idle"))
      t->priority = recalculate_priority(t);
    else
      t->priority = priority;

    /* donation isn't tested, but whatever */
    t->priority_orig = t->priority;
  }

  t->magic = THREAD_MAGIC;
  t->elem.next = t->elem.prev = NULL;

  t->num_lock_donors = 0;
  list_init (&t->donlocklist);
  t->waitlock = NULL;

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size)
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void)
{
  struct list_elem *max;
  int i;

  if (!thread_mlfqs) {
    /* find a way to always keep the threads sorted
       by priority, with a binary tree */
    if (list_empty (&ready_list))
      return idle_thread;
    else {
      max = list_max (&ready_list, priority_less, NULL);
      list_remove (max);
      return list_entry (max, struct thread, elem);
    }
  } else {
    for (i = PRI_MAX; i >= 0; i--) {
      if (!list_empty(&ready_queues[i])) {
        max = list_front(&ready_queues[i]);
        list_remove(max);
        return list_entry(max, struct thread, elem);
      }
    }
    /* all queues were empty */
    return idle_thread;
  }
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();

  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* restore original priority */
  if ((cur->num_lock_donors == 0) && !thread_mlfqs)
    cur->priority = cur->priority_orig;

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread)
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void)
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void)
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid;
  if (next_tid == INT_MAX)
    next_tid = 2;
  else
    next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
