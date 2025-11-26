# MP3-report

**組員貢獻**:
- 林祐群：Print Page Table、Add a read only share page
- 蔡嘉祐：Generate a Page Fault、Demand Paging and Swapping

---

## Trace Code

### 1.1 How does xv6 run a user program？

xv6 執行使用者程式的流程如下：

#### 步驟 1: `kernel/main.c/main()`：kernel 的進入點
- 初始化各個子系統（console、記憶體分配器、檔案系統等）
- 呼叫 `userinit()` 建立第一個使用者 process
- 呼叫 `scheduler()` 開始排程 process

```c
void main()
{
  // ... 初始化各個子系統
  userinit();      // 建立第一個 process
  scheduler();     // 永不返回
}
```

#### 步驟 2: `kernel/proc.c/scheduler()`
**功能**：選擇並切換到可執行的 process
- 無窮迴圈掃描 process table
- 找到 RUNNABLE 狀態的 process
- 使用 `swtch(&c->context, &p->context)` 切換 context
- 儲存目前 CPU 的 context，載入 process 的 context

```c
for(;;) {
  for(p = proc; p < &proc[NPROC]; p++) {
    if(p->state == RUNNABLE) {
      p->state = RUNNING;
      c->proc = p;
      swtch(&c->context, &p->context);  // 切換 context
      c->proc = 0;
    }
  }
}
```

#### 步驟 3: `kernel/swtch.S`
**功能**：執行實際的 context 切換
- 儲存 scheduler 的 callee-saved 暫存器（ra, sp, s0-s11）
- 載入 process 的 callee-saved 暫存器
- 透過恢復 process 的 ra 暫存器跳到 process 的執行位置

- 不儲存/恢復 caller-saved 暫存器（由呼叫者處理）
- 只處理 callee-saved 暫存器
- 返回地址（ra）決定 context 切換後的執行位置

#### 步驟 4: `kernel/proc.c/forkret()`
**功能**：新 process 第一次被排程時的目標
- 釋放 scheduler 持有的 process lock
- 執行一次性初始化（第一次呼叫時初始化檔案系統）
- 呼叫 `usertrapret()` 返回user space

```c
void forkret(void)
{
  static int first = 1;
  release(&myproc()->lock);
  
  if (first) {
    first = 0;
    fsinit(ROOTDEV);  // 第一次才初始化檔案系統
  }
  usertrapret();
}
```

#### 步驟 5: `kernel/trap.c/usertrapret()`：準備返回user mode
- 關閉中斷（準備切換到user space）
- 設定 trapframe 的 kernel 資訊（satp, sp, trap handler）
- 設定 sstatus 暫存器以進入user mode（SPP=0, SPIE=1）
- 設定 sepc 為使用者程式計數器
- 跳到 trampoline 的 `userret`

```c
// 設定user mode
unsigned long x = r_sstatus();
x &= ~SSTATUS_SPP;  // SPP=0 表示user mode
x |= SSTATUS_SPIE;  // 在user mode啟用中斷
w_sstatus(x);

w_sepc(p->trapframe->epc);  // 設定返回地址
```

#### 步驟 6: `kernel/trampoline.S/userret`：在 trampoline page 中切換到user mode
- 切換到使用者頁表（寫入 satp）
- 從 trapframe 恢復所有使用者暫存器
- 執行 `sret` 指令返回user mode

- 對於 `userinit()`，initcode 已經被載入到 VA 0
- Trampoline page 同時映射在 kernel 和 user 空間，確保切換順利

#### 步驟 7: `kernel/exec.c/exec()`：載入並執行程式
- 讀取 ELF 檔案
- 為各段（segments）分配記憶體
- 設定使用者堆疊和參數
- 用新程式映像取代目前 process 映像
- 返回user space開始執行程式

**完整流程圖**：
```
main() → scheduler() → swtch() → forkret() → usertrapret() → userret → 使用者程式
                ↑                                                          ↓
                └──────────────────────── system call / trap ──────────────┘
```

---

### 1.2 How does xv6 allocate physical memory and map it into the process’s virtual address space?

追蹤記憶體分配與映射的完整流程：

#### 步驟 1: `user/user.h/sbrk()`：user space函式宣告
- 宣告：`char *sbrk(int n);`
- 系統呼叫的 wrapper
- 增加或減少 process heap n bytes
- 返回舊的 heap 邊界位址

#### 步驟 2: `user/usys.pl`：產生系統呼叫 stub
- Perl 腳本產生 `usys.S` 組合語言程式碼
- 對於 sbrk：載入系統呼叫號碼（SYS_sbrk）到 a7，執行 `ecall`
- 觸發 trap 進入 kernel mode

**範例 code**：
```assembly
.global sbrk
sbrk:
  li a7, SYS_sbrk
  ecall
  ret
```

#### 步驟 3: `kernel/sysproc.c/sys_sbrk()`：kernel 中的 sbrk 系統呼叫處理器
- 使用 `argint(0, &n)` 提取參數 n
- **原始行為**：呼叫 `growproc(n)` 分配/釋放記憶體
- **MP3 修改**：實作 lazy allocation
  - n > 0: 只更新 `p->sz`，不立即分配實體記憶體
  - n < 0: 呼叫 `uvmdealloc()` 釋放頁面
  
```c
if (n > 0) {
  p->sz += n;  
} else if (n < 0) {
  p->sz = uvmdealloc(p->pagetable, p->sz, p->sz + n);
}
```

#### 步驟 4: `kernel/proc.c/growproc()`：增加或縮小 process 記憶體
- 成長（n > 0）：呼叫 `uvmalloc()` 分配並映射頁面
- 縮小（n < 0）：呼叫 `uvmdealloc()` 釋放頁面
- 更新 `p->sz` 反映新大小

在 MP3 的 lazy allocation 中，正成長時不使用此函式

#### 步驟 5: `kernel/vm.c/uvmalloc()`：分配實體記憶體並建立頁表映射
- 對於範圍 [oldsz, newsz) 中的每個頁面：
  1. 呼叫 `kalloc()` 分配實體頁面
  2. 用 `memset()` 清零頁面
  3. 呼叫 `mappages()` 建立 PTE 映射 VA 到 PA
  4. 設定權限位元（PTE_R, PTE_U, PTE_W/PTE_X）
- 成功返回新 size，失敗返回 0


##### Page Table Entry (PTE) - 64 位元值：
```
 63        54 53        28 27        10 9        0
+------------+------------+------------+----------+
|  Reserved  |    PPN[2]  |   PPN[1]   | PPN[0]|Flags
+------------+------------+------------+----------+
```
- **Bits [53:10]**：Physical Page Number (PPN)
- **Bits [9:0]**：Flags
  - **V** (Valid)：PTE 有效
  - **R** (Read)：可讀
  - **W** (Write)：可寫
  - **X** (Execute)：可執行
  - **U** (User)：使用者可存取
  - **S** (Swapped)：已交換到磁碟（MP3 新增）

##### Virtual Address（39-bit Sv39）：
```
 38        30 29        21 20        12 11        0
+------------+------------+------------+----------+
|    L2      |     L1     |     L0     |  Offset  |
+------------+------------+------------+----------+
   9 bits       9 bits       9 bits      12 bits
```
- **Bits [38:30]**：L2 索引（第 2 層頁表）
- **Bits [29:21]**：L1 索引（第 1 層頁表）
- **Bits [20:12]**：L0 索引（第 0 層頁表）
- **Bits [11:0]**：頁面內 offset

**記憶體分配流程圖**：
```
使用者: sbrk(n)
    ↓
  ecall (trap to kernel)
    ↓
sys_sbrk()
    ↓
【原始】growproc() → uvmalloc() → kalloc() + mappages()
                                      ↓
【MP3】 只更新 p->sz              實體記憶體
    ↓                               ↑
返回舊位址                           │
    ↓                               │
【稍後存取】→ Page Fault → handle_pgfault()
```

---

## 實作細節

### Print Page Table：`kernel/vm.c`

實作 `vmprint()` 函式以遞迴方式遍歷並列印三層 Sv39 頁表結構。


##### 1. 遞迴遍歷設計
- 新增 `vmprint_recursive(pagetable, level, va_base)` 輔助函式
- `level` 參數：2（頂層）→ 1（中層）→ 0（底層）
- `va_base` 參數：累積虛擬位址的高位元部分

##### 2. 虛擬位址計算
**方法**：從頁表索引重建 VA
```c
uint64 va = va_base | ((uint64)i << (12 + 9 * level));
```
- Level 2: 索引左移 30 位元（12 + 9*2）
- Level 1: 索引左移 21 位元（12 + 9*1）
- Level 0: 索引左移 12 位元（12 + 9*0）

##### 3. 縮排格式：`2 * (3 - level)` 個空格
- Level 2: 0 空格（頂層）
- Level 1: 2 空格
- Level 0: 4 空格

##### 4. 旗標顯示
依序檢查並顯示：V, R, W, X, U, S

```c
static void vmprint_recursive(pagetable_t pagetable, int level, uint64 va_base)
{
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    
    // 跳過無效項目（除非是 swapped）
    if ((pte & PTE_V) == 0 && (pte & PTE_S) == 0)
      continue;

    // 計算 VA 和列印格式化資訊
    uint64 va = va_base | ((uint64)i << (12 + 9 * level));
    
    // ... 列印邏輯
    
    // 遞迴處理非葉子節點
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0 && level > 0)
    {
      uint64 child = PTE2PA(pte);
      vmprint_recursive((pagetable_t)child, level - 1, va);
    }
  }
}
```

#### 輸出範例
```
page table 0x0000000087f4e000
  0: pte=0x0000000021fd2801 va=0x0000000000000000 pa=0x0000000087f4a000 V
    0: pte=0x0000000021fd2401 va=0x0000000000000000 pa=0x0000000087f49000 V
      0: pte=0x0000000021fd2c5b va=0x0000000000000000 pa=0x0000000087f4b000 V R X U
      1: pte=0x0000000021fd2017 va=0x0000000000001000 pa=0x0000000087f48000 V R W U
```

---

### 2.2 唯讀共享頁面 (Read-Only Shared Page) ：`kernel/proc.h`, `kernel/proc.c`

為每個 process 分配一個唯讀頁面，映射到固定虛擬位址 `USYSCALL`，包含 process 資訊（如 PID）。

#### 1：資料結構（proc.h）：儲存指向 kernel 分配的 usyscall 結構的指標
```c
struct proc {
  // ... 其他欄位
  struct usyscall *usyscall;   // 映射到 USYSCALL 的唯讀頁面
};
```

####  2：分配頁面（proc.c/allocproc()）：在建立頁表之後，context 初始化之前
```c
// 分配 usyscall 頁面
if ((p->usyscall = (struct usyscall *)kalloc()) == 0)
{
  freeproc(p);
  release(&p->lock);
  return 0;
}
memset(p->usyscall, 0, PGSIZE);
p->usyscall->pid = p->pid;  // 初始化 PID
```

**錯誤處理**：分配失敗則清理並返回 0

####  3：映射頁面（proc.c/proc_pagetable()）
```c
// 映射 usyscall 頁面到 USYSCALL 位址
if (mappages(pagetable, USYSCALL, PGSIZE,
             (uint64)(p->usyscall), PTE_R | PTE_U) < 0)
{
  // 失敗時清理所有映射
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmfree(pagetable, 0);
  return 0;
}
```

**權限設定**：
- `PTE_R`：可讀
- `PTE_U`：使用者可存取
- **無 PTE_W**：唯讀，防止使用者修改

####  4：釋放頁面（proc.c/freeproc() 和 proc_freepagetable()）

**在 freeproc() 中**：
```c
if (p->usyscall)
  kfree((void *)p->usyscall);
p->usyscall = 0;
```

**在 proc_freepagetable() 中**：
```c
uvmunmap(pagetable, USYSCALL, 1, 0);  // do_free=0
```

**do_free=0**：這裡只取消映射，實體記憶體由 `freeproc()` 負責釋放。

**實作修正**：
為了避免在持有 `wait_lock` 時進行磁碟 I/O（釋放 Swapped 頁面），MP3 將 `proc_freepagetable()` 的呼叫從 `freeproc()` 移至 `exit()`。因此 `proc_freepagetable()` 會先執行取消映射，隨後 `freeproc()` 才釋放 `usyscall` 的實體記憶體。

---

### 2.3 Lazy Allocation - MP3-3：`kernel/sysproc.c`, `kernel/vm.c`, `kernel/paging.c`, `kernel/trap.c`

修改 `sbrk()` 使其只增加 process 的虛擬位址空間大小，實體頁面在第一次存取時才分配。

1. **加速 sbrk()**：從 O(n) 降為 O(1)，其中 n 是頁面數
2. **節省記憶體**：只分配實際使用的頁面
3. **減少啟動時間**：需要大 heap 但只使用少量的程式受益

#### 1：修改 sys_sbrk()（sysproc.c）

```c
if (n > 0)
{
  // Lazy allocation: 只增加 size
  p->sz += n;
}
else if (n < 0)
{
  // 縮小：釋放頁面
  p->sz = uvmdealloc(p->pagetable, p->sz, p->sz + n);
}
return addr;
```

**變更**：
- 成長時不呼叫 `growproc()`
- 只更新 `p->sz`，建立空洞
- 縮小時仍然呼叫 `uvmdealloc()` 立即釋放

####  2：放寬 uvmunmap() 檢查（vm.c）：跳過這些情況，容忍空洞

```c
for (a = va; a < va + npages * PGSIZE; a += PGSIZE)
{
  if ((pte = walk(pagetable, a, 0)) == 0)
  {
    continue;  // 無頁表頁面，跳過
  }
  if ((*pte & PTE_V) == 0 && (*pte & PTE_S) == 0)
  {
    continue;  // 未映射也未交換，跳過
  }
  // ... 釋放邏輯
}
```
Lazy allocation 會在位址空間中留下未分配的區域

####  3：實作 handle_pgfault()（paging.c）

**處理流程**：
```
1. 從 stval 暫存器讀取造成 fault 的 VA
2. 對齊到頁面邊界
3. 驗證：VA < p->sz && VA < MAXVA
4. 使用 walk(pagetable, va, alloc=1) 取得/建立 PTE
5. 判斷三種情況：
   a) PTE_V 已設定：錯誤（已映射）
   b) PTE_S 已設定：Swapped 情況（見 MP3-4）
   c) 兩者都沒有：Lazy allocation 情況
      - kalloc() 分配實體頁面
      - memset() 清零
      - mappages() 建立映射，權限 PTE_U|R|W|X
6. 成功返回 0，失敗返回 -1
```

```c
// Lazy allocated 頁面 - 分配實體記憶體
pa = kalloc();
if (pa == 0)
    return -1;
memset(pa, 0, PGSIZE);

// 映射時使用使用者權限
if (mappages(p->pagetable, va, PGSIZE, (uint64)pa, 
             PTE_U | PTE_R | PTE_W | PTE_X) != 0)
{
    kfree(pa);
    return -1;
}
```

新頁面獲得完整使用者存取權（U, R, W, X）

####  4：修改 usertrap()（trap.c）

**檢查**：捕捉 page fault 例外
```c
else if (r_scause() == 13 || r_scause() == 15)
{
  // Page fault: 13 = load, 15 = store
  if (handle_pgfault() != 0)
  {
    printf("usertrap(): page fault failed va=%p pid=%d\n", r_stval(), p->pid);
    setkilled(p);
  }
}
```

**scause 值**：
- **13**：Load page fault（讀取未映射的頁面）
- **15**：Store/AMO page fault（寫入未映射的頁面）

**流程**：Page fault handler 在 kernel mode 執行，然後恢復使用者程式碼

#### 完整的 Lazy Allocation 流程圖
```
1. 使用者呼叫 sbrk(4096)
   ↓
2. sys_sbrk() 只增加 p->sz
   ↓
3. 返回user space，此時頁面未分配
   ↓
4. 使用者存取新位址
   ↓
5. CPU 引發 page fault（無 PTE_V）
   ↓
6. usertrap() 捕捉 scause==13 或 15
   ↓
7. handle_pgfault() 分配實體頁面並映射
   ↓
8. 返回user space，重新執行導致 fault 的指令
   ↓
9. 成功存取記憶體
```

---

### 2.4 需求分頁與交換 (Demand Paging and Swapping)：`kernel/vm.c`, `kernel/paging.c`

#### 實作概述
實作 `madvise()` 系統呼叫，提供使用者程式提示記憶體使用模式，支援將頁面交換到磁碟和從磁碟交換回來。

####  1：擴充 vmprint()（vm.c）：顯示 swapped 頁面資訊

```c
if (pte & PTE_S)
{
  // Swapped 頁面：列印 block number
  uint64 blockno = PTE2BLOCKNO(pte);
  printf("pa=%p blockno=%p", blockno << 10, blockno);
}
```

**輸出範例**：
```
5: pte=0x00000000000f42de va=0x0000000000005000 pa=0x00000000003d0000 
   blockno=0x00000000000003d0 R W X U S
```

####  2：實作 madvise()（vm.c）

**函式**：
```c
int madvise(uint64 base, uint64 len, int advice)
```

##### 模式 1: MADV_NORMAL：只驗證範圍
```c
case MADV_NORMAL:
  // 檢查範圍是否在 [0, p->sz) 內
  if (end > p->sz)
    return -1;
  return 0;
```

##### 模式 2: MADV_DONTNEED（交換出）：將記憶體中的頁面移到磁碟

**algo**（對範圍內每個頁面）：
```
1. 使用 walk() 取得 PTE
2. 如果未映射：跳過（已交換或未分配）
3. 分配磁碟 block：blockno = balloc_page(ROOTDEV)
4. 寫入磁碟：write_page_to_disk(ROOTDEV, pa, blockno)
5. 更新 PTE：
   - 儲存 blockno：BLOCKNO2PTE(blockno)
   - 設定 S 旗標，清除 V 旗標
   - 保留其他旗標（R, W, X, U）
6. 釋放實體記憶體：kfree(pa)
```

```c
// 分配磁碟 block 並寫入
begin_op();
uint blockno = balloc_page(ROOTDEV);
uint64 pa = PTE2PA(*pte);
write_page_to_disk(ROOTDEV, (char *)pa, blockno);
end_op();

// 更新 PTE
uint64 flags = PTE_FLAGS(*pte);
*pte = BLOCKNO2PTE(blockno) | (flags & ~PTE_V) | PTE_S;

// 釋放記憶體
kfree((void *)pa);
```

**磁碟 I/O**：所有磁碟操作必須包在 `begin_op()` / `end_op()` 中

##### 模式 3: MADV_WILLNEED（交換入或預先分配）：確保頁面在實體記憶體中

**algo**（對範圍內每個頁面）：
```
1. 使用 walk(alloc=1) 取得 PTE
2. 三種情況：
   a) PTE_V 已設定：已在記憶體，跳過
   b) PTE_S 已設定：Swapped 頁面
      - 分配實體頁面：pa = kalloc()
      - 從磁碟讀取：read_page_from_disk(ROOTDEV, pa, blockno)
      - 釋放磁碟 block：bfree_page(ROOTDEV, blockno)
      - 更新 PTE：PA2PTE(pa)，設定 V，清除 S
   c) 兩者都沒有：未映射頁面
      - 分配實體頁面：pa = kalloc()
      - 清零：memset(pa, 0, PGSIZE)
      - 映射：mappages(pagetable, va, PGSIZE, pa, PTE_U|R|W|X)
```

**Swap-in 程式碼**：
```c
if (*pte & PTE_S)
{
  uint blockno = PTE2BLOCKNO(*pte);
  char *pa = kalloc();
  
  begin_op();
  read_page_from_disk(ROOTDEV, pa, blockno);
  bfree_page(ROOTDEV, blockno);  // 釋放磁碟空間
  end_op();
  
  // 更新 PTE：恢復為有效頁面
  uint64 flags = PTE_FLAGS(*pte);
  *pte = PA2PTE(pa) | (flags & ~PTE_S) | PTE_V;
}
```

####  3：增強 handle_pgfault()（paging.c）：處理 swapped 頁面（PTE_S 情況）

```c
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
```

#### 完整的 Page Fault 處理流程
```
使用者存取頁面
    ↓
Page Fault
    ↓
usertrap() 捕捉 scause==13 或 15
    ↓
handle_pgfault()
    ↓
檢查 PTE 狀態
    ├─ PTE_V=1: 錯誤（不應該 fault）
    ├─ PTE_S=1: Swapped 頁面
    │    ↓
    │   kalloc() + read_from_disk() + 更新 PTE
    │    ↓
    └─ 都沒有: Lazy 頁面
         ↓
        kalloc() + mappages()
         ↓
繼續執行使用者程式碼
```

#### Swapping 流程
```
1. 程式分配並使用頁面 A
   [PTE: V=1, PA=0x1000]

2. 呼叫 madvise(A, PGSIZE, MADV_DONTNEED)
   - 分配 disk block 100
   - 寫入磁碟
   [PTE: V=0, S=1, blockno=100]
   - 釋放 PA 0x1000

3. 存取頁面 A → Page Fault
   [handle_pgfault 檢測到 PTE_S=1]
   - 分配 PA 0x2000
   - 從 block 100 讀取
   - 釋放 block 100
   [PTE: V=1, S=0, PA=0x2000]

4. 繼續執行
```

---
## Bonus & edge case說明

 `grade-mp3-bonus` 檢測：
1) Lazy allocation 形成的「未分配空洞」行為是否正確；
2) Page fault 欄位與旗標是否如預期；
3) 收縮位址空間後釋放是否正確；
4) Swapped PTE 編碼與旗標一致性；

### 1. lazy_holes 測試
目的：驗證使用者呼叫 `sbrk()` 擴充後未觸發實際分配時，`vmprint()` 不會出現對應 VA 的有效 (V) PTE，也不應誤標示為 swapped。
流程：
- 呼叫 `sbrk()` 擴充多個頁面但不存取。
- 執行 `vmprint()` 收集輸出並以正規表示式確認目標 VA 範圍內無 `V` / `S` 標記。
通過條件：所有尚未存取的頁面在輸出中缺席或僅存在上層非葉子 PTE (僅 V、無 R/W/X/U)。

### 2. fault_flags 測試
目的：確保第一次存取 lazy 分配的頁面觸發 page fault 後，建立的葉子 PTE 具備 `V R W X U` 權限（符合 MP3 規範給完整使用者權限）。
流程：
- 擴充一頁 `sbrk(PGSIZE)`。
- 對該頁進行讀寫觸發 fault。
- `vmprint()` 應出現該頁面 PTE：`V R W X U`（無 S）。
錯誤指標：缺少任何一個使用者基本權限或多出 S。

### 3. shrink_remove 測試
目的：驗證對已分配頁面呼叫負成長 `sbrk(-PGSIZE)` 後，`uvmdealloc()` 釋放頁面並移除對應有效 PTE，不殘留沒有pointer 指向的 swapped PTE。
流程：
- 分配並觸發分配一頁。
- 立刻縮小釋放該頁。
- `vmprint()` 不應再出現該 VA 的葉子 PTE（上層中介頁表若仍存在屬正常暫留）。

### 4. swapped_encoding 測試
目的：確認 `MADV_DONTNEED` 將頁面交換出後，PTE：
- 取消 `PTE_V`；設定 `PTE_S`；保留原存取權 (R/W/X/U)。
- 高位 block 編碼正確：`PTE2BLOCKNO(pte)` 還原磁碟頁面編號；`vmprint()` 中顯示 `blockno`；`pa` 顯示為 `blockno << 12`。
流程：
- 分配並觸發一頁。
- 呼叫 `madvise(addr, PGSIZE, MADV_DONTNEED)`。
- `vmprint()` 中該 VA 對應列應含 `R`（如果原本可讀）、`U`、`S`，不含 `V`。
錯誤指標：`V` 未清除、`S` 缺失、`blockno` 顯示不一致或 `pa` 位移錯誤。

### 5. swapped_flags 測試
目的：驗證 `MADV_DONTNEED` 交換出頁面後，PTE 保留了原始的 `R/W/X/U` 權限旗標，僅清除 `V` 並設定 `S`。確保權限資訊在交換過程中不丟失。
流程：
- 呼叫 `madvise(..., MADV_DONTNEED)` 交換出頁面。
- 檢查 `vmprint()` 輸出中該頁面的 PTE。
- 驗證：`V` 旗標不存在，`S` 旗標存在，且 `R/W/X/U` 旗標皆保留。

