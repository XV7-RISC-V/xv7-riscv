#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

extern struct proc proc[];

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
    int which_dev = 0;

    if((r_sstatus() & SSTATUS_SPP) != 0)
    {
        panic("usertrap: not from user mode");
    }

    // send interrupts and exceptions to kerneltrap(),
    // since we're now in the kernel.
    w_stvec((uint64)kernelvec);

    struct proc *p = myproc();

    // save user program counter.
    p->trapframe->epc = r_sepc();

    if(r_scause() == 8)
    {
        // system call
        if(killed(p))
        {
            exit(-1);
        }

        // sepc points to the ecall instruction,
        // but we want to return to the next instruction.
        p->trapframe->epc += 4;

        // an interrupt will change sepc, scause, and sstatus,
        // so enable only now that we're done with those registers.
        intr_on();

        syscall();
    }
    else if ( r_scause() == 15 || r_scause() == 13 )
    {
        // Page Fault detected.
        // If the COW flag is raised, then the fault should be handled.
        // uint64 pageStart = PGROUNDDOWN( r_stval() ); 
        uint64 pageStart = r_stval();

        if ( pageStart >= MAXVA || ((uint64)pageStart>=PGROUNDDOWN(p->trapframe->sp)-PGSIZE&&(uint64)pageStart<=PGROUNDDOWN(p->trapframe->sp)) || pageStart <= 0 )
        {
            setkilled(p);
        }
        else
        {
            pte_t *pageTableEntry;
            pageTableEntry = walk( p->pagetable, pageStart, 0 );
            // No allocation is needed.

            if ( pageTableEntry == 0 )
            {
                // printf("Unavailable Address Refferenced.\n");
                printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
                printf("             sepc=%p stval=%p\n", r_sepc(), r_stval());
                setkilled(p);
                // Kill the process, since
                // this should not have happened 
                // in the COW-fork scheme.
                // usertrapret();
            }
            else if ( PTE_COW & *pageTableEntry )
            {
                char *physicalAddress = (char *)PTE2PA(*pageTableEntry);
                uint64 flags = PTE_FLAGS(*pageTableEntry);

                if ( (PTE_V & *pageTableEntry) && ( PTE_U & *pageTableEntry) && ( PTE_COW & *pageTableEntry) )
                {
                    // This is where the code should end up in case of cow-fork.

                    flags |= PTE_W;
                    flags &= (~PTE_COW);

                    char *newMemory = kalloc();

                    if ( newMemory == 0 )
                    {
                        setkilled(p);
                        exit(-1);
                    }
                    memmove( newMemory, (void*)physicalAddress, PGSIZE);

                    *pageTableEntry = PA2PTE(newMemory) | flags ;
                    // Instead of unmapping and remapping, the flag is directly modified.

                    kfree((void*)physicalAddress);
                }
            }
            else
            {
                printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
                printf("             sepc=%p stval=%p\n", r_sepc(), r_stval());
                setkilled(p);
            }
        }
    }
    else if((which_dev = devintr()) != 0)
    {
        // ok
    }
    else
    {
        printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
        printf("             sepc=%p stval=%p\n", r_sepc(), r_stval());
        setkilled(p);
    }

    if(killed(p))
    {
        exit(-1);
    }

    // give up the CPU if this is a timer interrupt.
    if(which_dev == 2)
    {
        if ( p->alarm == 1 )
        {
            p->tickCount++;

            if ( p->tickCount == p->alarmTime )
            {
                p->alarm = 0;
                *(p->Sigtrapframe) = *(p->trapframe);
                p->tickCount = 0;
                p->trapframe->epc = p->interruptFunction;
                // Function execution is passed on to the interrupt function.
            }
        }
#ifdef RR
        yield();
#endif

#ifdef MLFQ
        // printf("Pid Of CurrProc : %d, QueueLevel : %d\n", p->pid, p->queue);
        p->numTicks++;
        if ( p->numTicks >= ( 1 << ( p->queue ) ) )
        {
            yield();
        }
        else
        {
            for( struct proc * t = proc; t < &proc[NPROC]; t++ )
            {
                acquire(&t->lock);
                if ( t->state == RUNNABLE && t->queue < p->queue )
                {
                    release(&t->lock);
                    yield();
                    break;
                }
                release(&t->lock);
            }
        }
#endif



#ifdef LBS
        yield();
#endif



#ifdef FCFS
        // ok
#endif


#ifdef PBS
        // ok
#endif
    }
    
    usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to userret in trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
    int which_dev = 0;
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();

    if((sstatus & SSTATUS_SPP) == 0)
    {
        panic("kerneltrap: not from supervisor mode");
    }
    if(intr_get() != 0)
    {
        panic("kerneltrap: interrupts enabled");
    }

    if((which_dev = devintr()) == 0)
    {
        printf("scause %p\n", scause);
        printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
        panic("kerneltrap");
    }

    // give up the CPU if this is a timer interrupt.
    if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    {



#ifdef RR
        yield();
#endif

#ifdef MLFQ
        myproc()->numTicks++;

        if ( myproc()->numTicks >= ( 1 << ( myproc()->queue ) ) )
        {
            myproc()->numTicks = 0;
            yield();
        }
        else
        {
            for( struct proc * t = proc; t < &proc[NPROC]; t++ )
            {
                acquire(&t->lock);
                if ( t->state == RUNNABLE && t->queue < myproc()->queue )
                {
                    release(&t->lock);
                    yield();
                    break;
                }
                release(&t->lock);
            }
        }
#endif

#ifdef FCFS
        // ok
#endif



#ifdef LBS
        yield();
#endif



#ifdef PBS
        // ok
#endif
    }

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
    acquire(&tickslock);
    ticks++;

#ifdef MLFQ 
    // printf("yahan bhi pahunch gaya.\n"); 
    for( struct proc *p = proc; p < &proc[NPROC]; p++ )
    {
        acquire(&p->lock);
        if ( p->state != RUNNABLE )
        {
            release(&p->lock);
            continue;
        }
        if (  ticks - p->in_tick >= ( 1 << p->queue ) && p->queue < 4 )
        {
            p->last_tick = ticks;
#ifdef YES
            printf("[%d] queue for %d changed from %d to %d\n", ticks, p->pid, p->queue, p->queue + 1);
#endif
            p->queue++;
        }
        else if ( ticks - p->last_tick >= 30 && p->queue > 0 )
        {
            p->last_tick = ticks;
#ifdef YES
            printf("[%d] queue for %d changed from %d to %d\n", ticks, p->pid, p->queue, p->queue - 1);
#endif
            p->queue--;
        }
        release(&p->lock);
    }

#endif
    update_time();
    wakeup(&ticks);
    release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

