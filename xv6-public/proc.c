#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
// For MLFQ
int period[2] = {4, 8}; // Priority for each queue
int tick_total = 0;     // Total tick  
int boostflag = 0;          // Boost check
// MLFQ queue struct 
struct proc* q0[64];
struct proc* q1[64];
int front[2] = {0,0};
int rear[2] = {-1,-1};
int size[2] = {0,0};
void 
Enqueue(int x, struct proc* p)
{
  if(size[x] == 64)
      return ;
  size[x]++;
  rear[x] = (rear[x]+1)%64;
  switch (x){
      case 0:
          q0[rear[x]] = p;
          break;
      case 1:
          q1[rear[x]] = p;
          break;
    }
}
void
Dequeue(int x)
{
  if(size[x] == 0)
      return ;
  size[x]--;
  front[x] = (front[x]+1)%64;
}

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
int nexttid = 1;

extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;
  
  p->tick = 0;
  p->level = 0;
  p->priority = 0;
  p->monopoly = 0;
  Enqueue(0,p);

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->tick = 0;
  p->level = 0;
  p->priority = 0;
  p->monopoly = 0;
  Enqueue(0,p);
  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;
  
  p->tid = 0;
  p->root = 0;
  memset(&p->memory, 0, sizeof p->memory);

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz, oldsz;
  struct proc *curproc = myproc();
  struct proc *p = curproc;
  int flag = 0;

  acquire(&ptable.lock);

  if(curproc->root)
      p = curproc->root;

  sz = p->sz;
  oldsz = sz;

  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      flag = 1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      flag = 1;
  }

  if(flag == 0){
      p->sz = sz;
      release(&ptable.lock);
      
      switchuvm(curproc);
      return oldsz;
  }
  else{
      release(&ptable.lock);
      return -1;
  }
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }
  
  if(curproc->tid > 0)
      np->pgdir = copyuvm(curproc->pgdir, curproc->root->sz);
  else
      np->pgdir = copyuvm(curproc->pgdir, curproc->sz);

  // Copy process state from proc.
  if(np->pgdir == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd, threadnum;

  if(curproc == initproc)
    panic("init exiting");
  
  if(curproc->tid == 0){
      acquire(&ptable.lock);
      
      for(;;){
          threadnum = 0;
          for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
              if(p->root == curproc){
                  if(p->state == ZOMBIE){
		              kfree(p->kstack);
		              p->kstack = 0;
                      
                      p->root->memory.data[p->root->memory.size++] = p->address;
                      
                      p->pid = 0;
                      p->parent = 0;
                      p->root = 0;
                      p->name[0] = 0;	
                      p->killed = 0;
                      p->state = UNUSED;

		              deallocuvm(p->pgdir, p->sz, p->address);
                  }
                  else{
                      threadnum++;
                      p->killed = 1;
                      wakeup1(p);
                  }
              }
	      }
	      if(threadnum == 0){
              release(&ptable.lock);
              break;
	      }
	      sleep(curproc, &ptable.lock);
      }
  }
      
  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);
  
  if(curproc->tid == 0)
      wakeup1(curproc->parent);
  else{
      if(curproc->root != 0){
          curproc->root->killed = 1;
          wakeup1(curproc->root);
      }
  }

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();
    
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
#ifdef FCFS_SCHED
    struct proc *first=0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state == RUNNABLE){
            if (first!=0){
                if(p->pid < first->pid)
                    first = p;
            }
            else
                first = p;
        }
    }
    if (first!=0){
        p = first;
        p->tick++;
        if(p->tick >= 100){
            cprintf("killed process %d\n", p->pid);
            p->killed = 1;
        }
        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;
        swtch(&(c->scheduler),p->context);
        switchkvm();
        c->proc = 0;
    }
#elif MLFQ_SCHED
    int i;
    struct proc *temp = 0;

    if(boostflag){
        while(size[1]){
            p = q1[front[1]];
            p->tick = 0;
            p->level = 0;
            p->priority = 0;
            Enqueue(0,p);
            Dequeue(1);
        }
        p = 0;
        boostflag = 0;
        tick_total = 0;
    }
    // Level 0 queue operation
    if(size[0] != 0){
        int f = front[0],r = rear[0];
        if(f > r){
            for(i = f; i < 64; i++){
                if(q0[i]->state != RUNNABLE)
                    continue;
                p = q0[i];
                c->proc = p;
                switchuvm(p);
                p->state = RUNNING;
                swtch(&(c->scheduler), p->context);
                switchkvm();
                c->proc = 0;
                if(tick_total >= 100){
                    boostflag = 1;
                    break;
                }
            }
            f = 0;
        }

        for(i = f ; i <= r; i++){
            if(q0[i]->state != RUNNABLE)
                continue;
            p = q0[i];
            c->proc = p;
            switchuvm(p);
            p->state = RUNNING;
            swtch(&(c->scheduler), p->context);
            switchkvm();
            c->proc = 0;
            if(tick_total >= 100){
                boostflag = 1;
                break;
            }
        }
    }
    // Level 1 queue operation
    else if(size[1] != 0){
        int f = front[1], r = rear[1];
        if(f > r){
            for(i = f; i< 64; i++){
                if(q1[i]->state == RUNNABLE){
                    if(temp!=0){
                        if(temp->priority < q1[i]->priority)
                            temp = q1[i];
                        else if(temp->priority == q1[i]->priority){
                            if(temp->pid > q1[i]->pid)
                                temp = q1[i];
                        }
                    }
                    else
                        temp = q1[i];
                }
                if(tick_total >= 100){
                    boostflag = 1;
                    break;
                }
                if(size[0] != 0)
                    break;
            }
            if(temp != 0){
                p = temp;
                c->proc = p;
                switchuvm(p);
                p->state = RUNNING;
                swtch(&(c->scheduler), p->context);
                switchkvm();
                c->proc = 0;
            }
            f = 0;
        }
        if(boostflag == 1 || size[0] != 0)
            continue;
        for(i = f; i <= r; i++){
            if(q1[i]->state == RUNNABLE){
                if(temp!=0){
                    if(temp->priority < q1[i]->priority)
                        temp = q1[i];
                    else if(temp->priority == q1[i]->priority){
                        if(temp->pid > q1[i]->pid)
                            temp = q1[i];
                    }
                }
                else
                    temp = q1[i];
            }
            if(tick_total >= 100){
                boostflag = 1;
                break;
            }
            if(size[0] != 0)
                break;
        }
        if(temp != 0){
            p = temp;
            c->proc = p;
            switchuvm(p);
            p->state = RUNNING;
            swtch(&(c->scheduler), p->context);
            switchkvm();
            c->proc = 0;
        }
    }
#else
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
#endif
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid && p->tid == 0){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
void
setpriority(int pid, int priority)
{ 
  struct proc *p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->priority = priority;
      return;
    }
  }
  return;
}
void
monopolize(int password)
{
  if(password != 2015005078){
      cprintf("killed process %d\n", myproc()->pid);
      myproc()->killed = 1;
      yield();
  }
  else{
      if(myproc()->monopoly == 1)
          myproc()->monopoly = 0;
      else
          myproc()->monopoly = 1;
  }
}

int
thread_create(thread_t* thread, void* (*start_routine)(void *), void* arg)
{
  int i;
  uint sz, sp, address;
  pde_t *pgdir;
  struct proc *np;
  struct proc *curproc = myproc();
  struct proc *root = curproc;

  if(curproc->root)
      root = curproc->root;

  if((np = allocproc()) == 0)
      return -1;

  nextpid--;

  np->root = root;
  np->pid = root->pid;
  np->tid = nexttid++;

  acquire(&ptable.lock);
  pgdir = root->pgdir;
  
  if(root->memory.size)
      address = root->memory.data[--root->memory.size];
  else{
      address = root->sz;
      root->sz += 2*PGSIZE;
  }

  if((sz = allocuvm(pgdir, address, address + 2*PGSIZE)) == 0){
      np->state = UNUSED;
      return -1;
  }
  
  release(&ptable.lock);

  *np->tf = *root->tf;

  for(i = 0; i < NOFILE; i++)
      if(root->ofile[i])
          np->ofile[i] = filedup(root->ofile[i]);
  np->cwd = idup(root->cwd);

  safestrcpy(np->name, root->name, sizeof(root->name));

  sp = sz - 4;
  *((uint*)sp) = (uint)arg;
  sp -= 4;
  *((uint*)sp) = 0xffffffff;

  np->pgdir = pgdir;
  np->address = address;
  np->sz = sz;
  np->tf->eip = (uint)start_routine;
  np->tf->esp = sp;

  *thread = np->tid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return 0;
}
void
thread_exit(void* retval)
{
  struct proc *curproc = myproc();
  int fd;

  for(fd = 0; fd < NOFILE; fd++){
      if(curproc->ofile[fd]){
          fileclose(curproc->ofile[fd]);
          curproc->ofile[fd] = 0;
      }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  curproc->returnvalue = retval;

  wakeup1(curproc->root);

  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}
int
thread_join(thread_t thread, void** retval)
{
  struct proc *p;
  struct proc *curproc = myproc();
  
  if(curproc->root != 0)
      return -1;

  acquire(&ptable.lock);
  for(;;){
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
          if(p->tid != thread)
              continue;
          if(p->root != curproc){
              release(&ptable.lock);
              return -1;
          }
          
          if(p->state == ZOMBIE){
              *retval = p->returnvalue;
              
              kfree(p->kstack);
              p->kstack = 0;
              
              p->root->memory.data[p->root->memory.size++] = p->address;
              
              p->pid = 0;
              p->parent = 0;
              p->root = 0;
              p->name[0] = 0;
              p->killed = 0;
              p->state = UNUSED;
              
              deallocuvm(p->pgdir, p->sz, p->address);
              
              release(&ptable.lock);
              return 0;
          }
      }
      if(curproc->killed){
          release(&ptable.lock);
          return -1;
      }
      sleep(curproc, &ptable.lock);
  }
}
void
select(int pid, struct proc* selection)
{
  struct proc *p;

  acquire(&ptable.lock);

  if(myproc()->killed){
      release(&ptable.lock);
      return;
  }
  
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if (p->pid == pid && p != selection){
          p->killed = 1;
          p->chan = 0;
          p->state = SLEEPING;
      }
  }
  release(&ptable.lock);
}
void
clean(int pid, struct proc* selection)
{
  struct proc *p;
  int child;

  acquire(&ptable.lock);
  child = 0;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if (p->pid == pid && p != selection){
          p->state = RUNNABLE;
          
          if(p->parent){
              p->parent = selection;
              child = 1;
          }
      }
  }
  release(&ptable.lock);

  if(child == 1)
      wait();
}
