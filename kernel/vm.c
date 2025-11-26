#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// Initialize the one kernel_pagetable
void kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= MAXVA)
    panic("walk");

  for (int level = 2; level > 0; level--)
  {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V)
    {
      pagetable = (pagetable_t)PTE2PA(*pte);
    }
    else
    {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if (size == 0)
    panic("mappages: size");

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for (;;)
  {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
// mp3 TODO
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  /* MP3-3: 放寬檢查以支援 lazy allocation
   * 修改內容：
   * 1. 允許未映射的頁面（不再 panic）
   * 2. 支援釋放 swap 空間的頁面
   * 3. 移除嚴格的 leaf 檢查
   * MP3-4: 修正鎖問題：收集所有 swapped blocks 後批次釋放
   */
  
  // 第一步：收集需要釋放的 swapped blocks
  uint swapped_blocks[512]; // 最多 512 個頁面
  int swapped_count = 0;
  
  for (a = va; a < va + npages * PGSIZE; a += PGSIZE)
  {
    if ((pte = walk(pagetable, a, 0)) == 0)
    {
      // 此虛擬位址沒有頁表頁面，跳過（支援 lazy allocation）
      continue;
    }
    if ((*pte & PTE_V) == 0 && (*pte & PTE_S) == 0)
    {
      // 既未映射也未交換，跳過（lazy allocation 的洞）
      continue;
    }
    
    // 收集 swapped blocks
    if (do_free && (*pte & PTE_S))
    {
      swapped_blocks[swapped_count++] = (uint)PTE2BLOCKNO(*pte);
    }
    
    // 釋放物理記憶體
    if (do_free && (*pte & PTE_V))
    {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    
    *pte = 0;
  }
  
  // 第二步：批次釋放所有 swapped blocks（一次事務）
  if (swapped_count > 0)
  {
    begin_op();
    for (int i = 0; i < swapped_count; i++)
    {
      bfree_page(ROOTDEV, swapped_blocks[i]);
    }
    end_op();
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if (sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE)
  {
    mem = kalloc();
    if (mem == 0)
    {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) != 0)
    {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz))
  {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
    {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    }
    else if (pte & PTE_V)
    {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE)
  {
    if ((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char *)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0)
    {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0)
  {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0)
    {
      if (*p == '\0')
      {
        *dst = '\0';
        got_null = 1;
        break;
      }
      else
      {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null)
  {
    return 0;
  }
  else
  {
    return -1;
  }
}

/* Print multi layer page table. */
/* MP3-1: 多層頁表列印功能
 * 實作說明：遞迴遍歷三層 Sv39 頁表結構
 * 修改內容：新增 vmprint_recursive 輔助函數來處理遞迴遍歷
 */
static void vmprint_recursive(pagetable_t pagetable, int level, uint64 va_base)
{
  // 每個頁表有 512 個 PTE
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    
    // 跳過無效項目（除非是 swapped 頁面）
    if ((pte & PTE_V) == 0 && (pte & PTE_S) == 0)
      continue;

    // 計算此項目的虛擬位址
    uint64 va = va_base | ((uint64)i << (12 + 9 * level));
    
    // 列印縮排：2 * (3 - level) 個空格
    for (int j = 0; j < (3 - level) * 2; j++)
      printf(" ");
    
    // 列印索引和 PTE 值
    printf("%d: pte=%p va=%p ", i, pte, va);
    
    // 判斷是 swapped 頁面還是有效頁面
    if (pte & PTE_S)
    {
      // Swapped 頁面：列印 block number
      // 根據規格，PTE 高位儲存的是 blockno，示範輸出顯示 pa=blockno<<12
      // 先前錯誤使用 <<10，造成測試檢查不一致，改為 <<12。
      uint64 blockno = PTE2BLOCKNO(pte);
      printf("pa=%p blockno=%p", blockno << 12, blockno);
    }
    else
    {
      // 有效頁面：列印物理位址
      uint64 pa = PTE2PA(pte);
      printf("pa=%p", pa);
    }
    
    // 列印旗標
    if (pte & PTE_V) printf(" V");
    if (pte & PTE_R) printf(" R");
    if (pte & PTE_W) printf(" W");
    if (pte & PTE_X) printf(" X");
    if (pte & PTE_U) printf(" U");
    if (pte & PTE_S) printf(" S");
    printf("\n");
    
    // 如果是有效的非葉子 PTE（指向頁表），遞迴處理
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0 && level > 0)
    {
      uint64 child = PTE2PA(pte);
      vmprint_recursive((pagetable_t)child, level - 1, va);
    }
  }
}

void vmprint(pagetable_t pagetable)
{
  /* MP3-1 & MP3-4: 列印多層頁表
   * 實作方式：從 level 2 開始遞迴遍歷三層頁表
   * 支援顯示一般頁面和 swapped 頁面（PTE_S 旗標）
   */
  printf("page table %p\n", pagetable);
  vmprint_recursive(pagetable, 2, 0);
}

/* Map pages to physical memory or swap space. */
int madvise(uint64 base, uint64 len, int advice)
{
  /* MP3-4: 實作 madvise 系統呼叫
   * 實作說明：處理三種建議模式
   * - MADV_NORMAL: 僅驗證範圍
   * - MADV_DONTNEED: 將頁面交換到磁碟
   * - MADV_WILLNEED: 從磁碟交換回來或分配新頁面
   */
  struct proc *p = myproc();
  uint64 addr;
  pte_t *pte;
  
  // 對齊到頁面邊界
  uint64 start = PGROUNDDOWN(base);
  uint64 end = PGROUNDUP(base + len);
  
  // 驗證：範圍必須在 process 記憶體內
  if (end > p->sz)
    return -1;
  
  switch (advice)
  {
  case MADV_NORMAL:
    // 只驗證範圍，不做其他動作
    return 0;
    
  case MADV_DONTNEED:
    // 將頁面交換到磁碟
    begin_op(); // 在迴圈外部開始事務
    for (addr = start; addr < end; addr += PGSIZE)
    {
      pte = walk(p->pagetable, addr, 0);
      if (pte == 0 || (*pte & PTE_V) == 0)
        continue; // 跳過未映射或已交換的頁面
      
      // 分配磁碟 block
      uint blockno = balloc_page(ROOTDEV);
      if (blockno == 0)
      {
        end_op();
        return -1;
      }
      
      // 將頁面寫入磁碟
      uint64 pa = PTE2PA(*pte);
      write_page_to_disk(ROOTDEV, (char *)pa, blockno);
      
      // 更新 PTE：設定 S 旗標，清除 V 旗標，儲存 block number
      // 減少旗標：僅保留 R/W/X/U（不保留 A/D/G 等硬體或保留位，測試預期）
      uint64 flags = PTE_FLAGS(*pte);
      uint64 keep = 0;
      if (flags & PTE_R) keep |= PTE_R;
      if (flags & PTE_W) keep |= PTE_W;
      if (flags & PTE_X) keep |= PTE_X;
      if (flags & PTE_U) keep |= PTE_U;
      // V 需被清除，加入 S
      *pte = BLOCKNO2PTE(blockno) | keep | PTE_S;
      
      // 釋放物理記憶體
      kfree((void *)pa);
    }
    end_op(); // 在迴圈外部結束事務
    return 0;
    
  case MADV_WILLNEED:
    // 將頁面帶入物理記憶體
    for (addr = start; addr < end; addr += PGSIZE)
    {
      pte = walk(p->pagetable, addr, 1);
      if (pte == 0)
        return -1;
      
      if (*pte & PTE_V)
      {
        // 已在記憶體中，不做任何事
        continue;
      }
      else if (*pte & PTE_S)
      {
        // 已交換的頁面：從磁碟讀取
        uint blockno = PTE2BLOCKNO(*pte);
        char *pa = kalloc();
        if (pa == 0)
          return -1;
        
        begin_op();
        read_page_from_disk(ROOTDEV, pa, blockno);
        bfree_page(ROOTDEV, blockno);
        end_op();
        
        // 更新 PTE：設定 V 旗標，清除 S 旗標
        uint64 flags = PTE_FLAGS(*pte);
        *pte = PA2PTE(pa) | (flags & ~PTE_S) | PTE_V;
      }
      else
      {
        // 未映射的頁面：分配新頁面
        char *pa = kalloc();
        if (pa == 0)
          return -1;
        memset(pa, 0, PGSIZE);
        
        if (mappages(p->pagetable, addr, PGSIZE, (uint64)pa, 
                     PTE_U | PTE_R | PTE_W | PTE_X) != 0)
        {
          kfree(pa);
          return -1;
        }
      }
    }
    return 0;
    
  default:
    return -1;
  }
}