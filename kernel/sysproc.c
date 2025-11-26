#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

// mp3 TODO
uint64
sys_sbrk(void)
{
  /* MP3-3: 實作 Lazy Allocation
   * 修改內容：sbrk 只更新 process size，不立即分配實體記憶體
   * 變更說明：
   * - n > 0: 只增加 p->sz，延遲到 page fault 時才分配
   * - n < 0: 呼叫 uvmdealloc 釋放頁面
   * - 實體頁面會在 handle_pgfault() 中按需分配
   */
  uint64 addr;
  int n;
  struct proc *p = myproc();

  argint(0, &n);
  addr = p->sz;
  
  if (n > 0)
  {
    // Lazy allocation: 只增加 size，不分配實體記憶體
    p->sz += n;
  }
  else if (n < 0)
  {
    // 縮小：釋放頁面
    p->sz = uvmdealloc(p->pagetable, p->sz, p->sz + n);
  }
  
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n)
  {
    if (killed(myproc()))
    {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

#ifdef LAB_PGTBL
int sys_pgaccess(void)
{
  // lab pgtbl: your code here.
  return 0;
}
#endif

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
