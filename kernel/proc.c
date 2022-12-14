#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"



#ifdef LBS
int total_tickets = 0;
#endif



struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
    struct proc *p;

    for(p = proc; p < &proc[NPROC]; p++)
    {
        char *pa = kalloc();
        if(pa == 0)
        {
            panic("kalloc");
        }
        uint64 va = KSTACK((int) (p - proc));
        kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    }
}

// initialize the proc table at boot time
void
procinit(void)
{
    struct proc *p;

    initlock(&pid_lock, "nextpid");
    initlock(&wait_lock, "wait_lock");
    for(p = proc; p < &proc[NPROC]; p++)
    {
        initlock(&p->lock, "proc");
        p->state = UNUSED;
        p->kstack = KSTACK((int) (p - proc));
    }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
    int id = r_tp();
    return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
    int id = cpuid();
    struct cpu *c = &cpus[id];
    return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
    push_off();
    struct cpu *c = mycpu();
    struct proc *p = c->proc;
    pop_off();
    return p;
}

int
allocpid()
{
    int pid;

    acquire(&pid_lock);
    pid = nextpid;
    nextpid = nextpid + 1;
    release(&pid_lock);

    return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
    struct proc *p;

    for(p = proc; p < &proc[NPROC]; p++)
    {
        acquire(&p->lock);
        if(p->state == UNUSED)
        {
            goto found;
        }
        else
        {
            release(&p->lock);
        }
    }
    // printf("Please Yahan nhi.\n");
    return 0;

found:
    p->pid = allocpid();
    p->state = USED;
    p->in_tick = ticks;
    p->run_time = 0;

    // Allocate a trapframe page.
    if((p->trapframe = (struct trapframe *)kalloc()) == 0)
    {
        freeproc(p);
        release(&p->lock);
        return 0;
    }
    if((p->Sigtrapframe = (struct trapframe *)kalloc()) == 0)
    {
        freeproc(p);
        release(&p->lock);
        return 0;
    }
    
    // An empty user page table.
    p->pagetable = proc_pagetable(p);
    if(p->pagetable == 0)
    {
        freeproc(p);
        release(&p->lock);
        return 0;
    }

    // Set up new context to start executing at forkret,
    // which returns to user space.
    memset(&p->context, 0, sizeof(p->context));
    p->context.ra = (uint64)forkret;
    p->context.sp = p->kstack + PGSIZE;

    //By default, no tracing is set.
    p->mask = 0;
    //Initially, the alarm timers are all initialized to 0.
    p->alarm = 0;
    //Initially, no goal for time should be set.
    p->alarmTime = 0;
    //Initially no ticks are recorded.
    p->tickCount = 0;
    //Initially, there is no interrupt function.
    p->interruptFunction = 0;

#ifdef LBS
    p->tickets = 1;                 // Default tickets
#endif


#ifdef PBS
    p->priority = 60;               // Default priority
    p->num_sched = 0;
    p->running = 0;
    p->sleeping = 0;
#endif


#ifdef MLFQ

    
    p->queue = 0;
    p->numTicks = 0;
    p->in_tick = 0;
    p->last_tick = 0;
#endif
    return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
    if(p->trapframe)
    {
        kfree((void*)p->trapframe);
    }
    if(p->Sigtrapframe)
    {
        kfree((void*)p->Sigtrapframe); 	Scheduler 	rtime 	wtime
RR 	Round Robin 	20 	
    }
    p->trapframe = 0;
    p->Sigtrapframe = 0;

    if(p->pagetable)
    {
        proc_freepagetable(p->pagetable, p->sz);
    }
    p->pagetable = 0;
    p->sz = 0;
    p->pid = 0;
    p->parent = 0;
    p->name[0] = 0;
    p->chan = 0;
    p->killed = 0;
    p->xstate = 0;
    p->state = UNUSED;

    p->mask = 0;
    p->alarm = 0;
    p->alarmTime = 0;
    p->interruptFunction = 0;
    p->tickCount = 0;

    p->in_tick = 0;
    p->run_time = 0;
    p->end_tick = 0;
    p->priority = 0;

#ifdef LBS
    /* total_tickets -= p->tickets; */
    p->tickets = 0;
#endif



#ifdef PBS
    p->num_sched = 0;
    p->running = 0;
    p->sleeping = 0;
#endif

#ifdef MLFQ
    p->queue = 0;
    p->numTicks = 0;
    p->last_tick = 0;
#endif

}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  p->in_tick = ticks;
  p->mask = 0;
  
  #ifdef LBS
  total_tickets += p->tickets;
#endif

#ifdef MLFQ
  p->in_tick = ticks;
  p->queue = 0;
  p->last_tick = ticks;
  p->numTicks = 0;
#endif

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
    int i, pid;
    struct proc *np;
    struct proc *p = myproc();

    // Allocate process.
    if((np = allocproc()) == 0)
    {
        return -1;
    }


    // Copy user memory from parent to child.
    if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
    {
        freeproc(np);
        release(&np->lock);
        return -1;
    }
    np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);
  
  np->mask = p->mask; 
  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

    //*(np->Sigtrapframe) = *(p->Sigtrapframe);

    // Cause fork to return 0 in the child.
    np->trapframe->a0 = 0;

    // increment reference counts on open file descriptors.
    for(i = 0; i < NOFILE; i++)
    {
        if(p->ofile[i])
        {
            np->ofile[i] = filedup(p->ofile[i]);
        }
    }
    np->cwd = idup(p->cwd);

    safestrcpy(np->name, p->name, sizeof(p->name));

    pid = np->pid;
    
    release(&np->lock);

    acquire(&wait_lock);
    np->parent = p;
    release(&wait_lock);

    acquire(&np->lock);
    np->state = RUNNABLE;
#ifdef MLFQ
    np->queue = 0;
    np->numTicks = 0;
    np->last_tick = ticks;
    np->in_tick = ticks;
#ifdef YES
    printf("[%d] started process %d\n", ticks, np->pid);
#endif
#endif

    np->priority = p->priority;
#ifdef LBS
    np->tickets = p->tickets;
    total_tickets += np->tickets;
#endif

    release(&np->lock);

    return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

#ifdef YES
            printf("[%d] exited process %d\n", ticks, p->pid);
#endif
  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;
  p->end_tick = ticks;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

int waitx(uint64 addr, int *rtime, int *wtime)
{
    struct proc *np;
    int havekids, pid;
    struct proc *p = myproc();

    acquire(&wait_lock);

    for (;;)
    {
        // Scan through table looking for exited children.
        havekids = 0;
        for (np = proc; np < &proc[NPROC]; np++)
        {
            if (np->parent == p)
            {
                // make sure the child isn't still in exit() or swtch().
                acquire(&np->lock);

                havekids = 1;
                if (np->state == ZOMBIE)
                {
                    // Found one.
                    pid = np->pid;
                    *rtime = np->run_time;
                    *wtime = np->end_tick - np->in_tick - np->run_time;
                    if (addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate, sizeof(np->xstate)) < 0)
                    {
                        release(&np->lock);
                        release(&wait_lock);
                        return -1;
                    }
                    freeproc(np);
                    release(&np->lock);
                    release(&wait_lock);
                    return pid;
                }
                release(&np->lock);
            }
        }

        // No point waiting if we don't have any children.
        if (!havekids || p->killed)
        {
            release(&wait_lock);
            return -1;
        }

        // Wait for a child to exit.
        sleep(p, &wait_lock); // DOC: wait-sleep
    }
}

int Min(int a, int b)
{
    return ((a < b) ? a : b);
}
int Max(int a, int b)
{
    return ((a > b) ? a : b);
}

#ifdef PBS
int 
nice_priority(struct proc* p)
{
    // default
    int niceness = 5;
    if (p->running + p->sleeping != 0)
    {
        // Not a new process
        niceness = (p->sleeping * 10) / (p->running + p->sleeping);
    }
    return Max(0, Min(p->priority - niceness + 5, 100));
}
#endif



int
do_rand(unsigned long *ctx)
{
/*
 * Compute x = (7^5 * x) mod (2^31 - 1)
 * without overflowing 31 bits:
 *      (2^31 - 1) = 127773 * (7^5) + 2836
 * From "Random number generators: good ones are hard to find",
 * Park and Miller, Communications of the ACM, vol. 31, no. 10,
 * October 1988, p. 1195.
 */
    long hi, lo, x;

    /* Transform to [1, 0x7ffffffe] range. */
    x = (*ctx % 0x7ffffffe) + 1;
    hi = x / 127773;
    lo = x % 127773;
    x = 16807 * lo - 2836 * hi;
    if (x < 0)
        x += 0x7fffffff;
    /* Transform to [0, 0x7ffffffd] range. */
    x--;
    *ctx = x;
    return (x);
}

unsigned long rand_next = 1;

int
rand(void)
{
    return (do_rand(&rand_next));
}


// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
    // Commented out below line becaus of warning:
    // Unused variable "p"
    // struct proc *p;
    struct cpu *c = mycpu();

    c->proc = 0;
    for(;;)
    {
        // Avoid deadlock by ensuring that devices can interrupt.
        intr_on();


#ifdef RR
        for(struct proc* p = proc; p < &proc[NPROC]; p++)
        {
            acquire(&p->lock);
            if(p->state == RUNNABLE)
            {
                // Switch to chosen process.  It is the process's job
                // to release its lock and then reacquire it
                // before jumping back to us.
                p->state = RUNNING;
                c->proc = p;
                swtch(&c->context, &p->context);

                // Process is done running for now.
                // It should have changed its p->state before coming back.
                c->proc = 0;
            }
            release(&p->lock);
        }
#endif


#ifdef FCFS
        struct proc* to_run = 0;
        for (struct proc* p = proc; p < &proc[NPROC]; p++)
        {
            // Hold the process so no other core can access it
            acquire(&p->lock);
            if (p->state == RUNNABLE)
            {
                if (to_run == 0)
                {
                    to_run = p;
                    continue;
                }
                else if (p->in_tick < to_run->in_tick)
                {
                    // to_run is no longer useful so release it
                    release(&to_run->lock);
                    to_run = p;
                    continue;
                }
            }
            release(&p->lock);
        }
        if (to_run != 0)
        {
            to_run->state = RUNNING;
            c->proc = to_run;
            swtch(&c->context, &to_run->context);
            c->proc = 0;
            release(&to_run->lock);
        }
#endif



#ifdef LBS
        if (total_tickets < 0)
        {
            panic("Negative Tickets");
        }
        struct proc* to_run = 0;
        int x = rand() % total_tickets + 1;
        int prefix = 0;
        for (struct proc* p = proc; p < &proc[NPROC]; p++)
        {
            acquire(&p->lock);
            if (p->state == RUNNABLE)
            {
                if (x <= prefix + p->tickets)
                {
                    to_run = p;
                    break;
                }
                prefix += p->tickets;
            }
            release(&p->lock);
        }
        if (to_run != 0)
        {
            total_tickets -= to_run->tickets;
            to_run->state = RUNNING;
            c->proc = to_run;
            swtch(&c->context, &to_run->context);
            c->proc = 0;
            release(&to_run->lock);
        }
#endif



#ifdef PBS
        struct proc* to_run = 0;
        for (struct proc* p = proc; p < &proc[NPROC]; p++)
        {
            acquire(&p->lock);
            if (p->state == RUNNABLE)
            {
                if (to_run == 0)
                {
                    to_run = p;
                    continue;
                }
                else if (nice_priority(to_run) > nice_priority(p))
                {
                    // "p" will be favoured in comparison to "to_run"
                    release(&to_run->lock);
                    to_run = p;
                    continue;
                }
                else if (nice_priority(to_run) == nice_priority(p))
                {
                    // Compare according to number of times process is scheduled
                    if (to_run->num_sched > p->num_sched)
                    {
                        // Choosing the process scheduled less number of times
                        release(&to_run->lock);
                        to_run = p;
                        continue;
                    }
                    else if (to_run->num_sched == p->num_sched)
                    {
                        // FCFS
                        if (to_run->in_tick < p->in_tick)
                        {
                            release(&to_run->lock);
                            to_run = p;
                            continue;
                        }
                    }
                }
            }
            release(&p->lock);
        }
        if (to_run != 0)
        {
            to_run->num_sched++;
            to_run->running = 0;
            to_run->sleeping = 0;
            to_run->state = RUNNING;
            c->proc = to_run;
            swtch(&c->context, &to_run->context);
            c->proc = 0;
            release(&to_run->lock);
        }

#endif

#ifdef MLFQ

    struct proc *currProc = 0; 
    // First find the current process to be used.
    for ( struct proc *p = proc; p < &proc[NPROC]; p++ )
    {  
        acquire(&p->lock);

        if ( p->state == RUNNABLE && currProc == 0 )
        {
            currProc = p;
        }
        else if ( p->state == RUNNABLE && p->queue <= currProc->queue)
        {
            // The processes are in the same queue
            // Now, we need to check which process is older
            if ( p->queue < currProc->queue )
            {
                release(&currProc->lock);
                currProc = p;
            }
            else if ( p->in_tick < currProc->in_tick )
            {
                // The process p is older than currProc
                // therefore should be executed now.
                release(&currProc->lock);
                currProc = p;
            } 
            else 
                release(&p->lock);
        } 
        else 
            release(&p->lock);
    }
    
    // At this point, currProc stores the 
    // process that needs to be run.
    
    if(currProc != 0) 
    {
        if ( currProc->state == RUNNABLE )
        {
            currProc->last_tick = ticks;
            currProc->numTicks = 0;
            // printf("Reached Here.\n");
            currProc->state = RUNNING;
            c->proc = currProc;
            // printf("guchu guchu\n");
            swtch(&c->context, &currProc->context);
            c->proc = 0;
        }
        release(&currProc->lock);
    }

#endif
    }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;

#ifdef LBS
  total_tickets += p->tickets;
#endif

#ifdef MLFQ
    if ( p->queue < 4 && p->numTicks >= (1 << p->queue ) )
    {
        p->queue++;
#ifdef YES
        printf("[%d] queue for %d changed from %d to %d\n", ticks, p->pid, p->queue - 1, p->queue);
#endif

    }
    p->last_tick = ticks;
    p->numTicks = 0;
#endif

  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  // if ( myproc()->pid > 2 )
  //   printf("Reached Sleep.\n");
  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  sched();
  // if ( myproc()->pid > 2 )
  //   printf("Reached Sleep.\n");
  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;

#ifdef LBS
        total_tickets += p->tickets;
#endif



#ifdef MLFQ
        p->last_tick = ticks;
        p->numTicks = 0;
#endif
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
#ifdef LBS
        total_tickets += p->tickets;
#endif
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]   =  "unused",
  [USED]     =  "used",
  [SLEEPING] =  "sleep ",
  [RUNNABLE] =  "runble",
  [RUNNING]  =  "run   ",
  [ZOMBIE]   =  "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

// Returns the old priority value
int
set_priority(int new_priority, int pid)
{
    int old_priority;
    if ((new_priority < 0) || (new_priority > 100))
    {
        printf("Priority must be in range [0 - 100]\n");
        return -1;
    }
    int flag = 0;
    struct proc* req_proc;
    for (struct proc* p = proc; p < &proc[NPROC]; p++)
    {
        acquire(&p->lock);
        if (p->pid == pid)
        {
            flag = 1;
            old_priority = p->priority;
            p->priority = new_priority;
            req_proc = p;
            break;
        }
        release(&p->lock);
    }
    if (flag)
    {
        printf("Priority of [%d] : %d -> %d\n", pid, old_priority, new_priority);
        if (new_priority < old_priority)
        {
            // req_proc will be prioritised more now
#ifdef PBS
            req_proc->running = 0;
            req_proc->sleeping = 0;
            yield();
#endif
        }
        release(&req_proc->lock);
        return old_priority;
    }
    else
    {
        printf("No process found with pid = [pid]\n", pid);
        return -1;
    }
}


// Updates the attributes of proc data-structure for scheduling
void
update_time()
{
    for (struct proc* p = proc; p < &proc[NPROC]; p++)
    {
        acquire(&p->lock);
        if (p->state == RUNNING)
        {
            p->run_time++;
#ifdef PBS
            p->running++;
#endif
        }
        else if (p->state == SLEEPING)
        {
#ifdef PBS
            p->sleeping++;
#endif
        }
        release(&p->lock);
    }
}

// Returns old ticket value
int
settickets(int new_ticket)
{
    int old = -1;
#ifdef LBS
    struct proc* p = myproc();
    old = p->tickets;
    p->tickets = new_ticket;
    if (total_tickets < 0)
    {
        panic("Negative Tickets");
    }
#endif
    return old;
}
