#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"

int
sys_getppid(void)
{
    struct proc *temp;
    temp = myproc()->parent;
    int ppid = temp->pid;

    return ppid;
}
