#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "proc.h"

/* Page fault handler */
int handle_pgfault()
{
    /* MP3-3 & MP3-4: 處理 page fault
     * 實作說明：處理 lazy allocation 和 swapping 的 page fault
     * 處理三種情況：
     * 1. Lazy allocated 頁面（無 PTE）：分配實體頁面
     * 2. Swapped 頁面（PTE_S 設定）：從磁碟讀回
     * 3. 無效存取：回傳 -1 終止 process
     */
    struct proc *p = myproc();
    uint64 va = r_stval();
    pte_t *pte;
    char *pa;
    
    // 對齊到頁面邊界
    va = PGROUNDDOWN(va);
    
    // 驗證：VA 必須小於 process size 且在使用者空間
    if (va >= p->sz || va >= MAXVA)
        return -1;
    
    // 取得或建立 PTE
    pte = walk(p->pagetable, va, 1);
    if (pte == 0)
        return -1;
    
    // 情況 1: 已經有效 - 不應該發生
    if (*pte & PTE_V)
        return -1;
    
    // 情況 2: 已交換的頁面 - 從磁碟讀取
    if (*pte & PTE_S)
    {
        uint blockno = PTE2BLOCKNO(*pte);
        pa = kalloc();
        if (pa == 0)
            return -1;
        
        // 從磁碟讀取頁面
        begin_op();
        read_page_from_disk(ROOTDEV, pa, blockno);
        bfree_page(ROOTDEV, blockno);
        end_op();
        
        // 更新 PTE：設定 V，清除 S，保留其他旗標
        uint64 flags = PTE_FLAGS(*pte);
        *pte = PA2PTE(pa) | (flags & ~PTE_S) | PTE_V;
        
        return 0;
    }
    
    // 情況 3: Lazy allocated 頁面 - 分配實體記憶體
    pa = kalloc();
    if (pa == 0)
        return -1;
    memset(pa, 0, PGSIZE);
    
    // 映射時使用使用者權限：U, R, W, X
    if (mappages(p->pagetable, va, PGSIZE, (uint64)pa, 
                 PTE_U | PTE_R | PTE_W | PTE_X) != 0)
    {
        kfree(pa);
        return -1;
    }
    
    return 0;
}
