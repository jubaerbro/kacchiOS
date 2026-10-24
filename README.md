# 🍚 kacchiOS

A minimal, educational baremetal operating system designed for teaching OS fundamentals.

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)]()
[![Platform](https://img.shields.io/badge/platform-x86-lightgrey.svg)]()

## 📖 Overview

kacchiOS is a simple, bare-metal operating system built from scratch for educational purposes. It provides a clean foundation for students to learn operating system concepts by implementing core components themselves.

### Current Features

- ✅ **Multiboot-compliant bootloader** - Boots via GRUB/QEMU
- ✅ **Serial I/O driver** (COM1) - Communication via serial port
- ✅ **Null process** - Single process that reads and echoes input
- ✅ **Basic string utilities** - Essential string operations
- ✅ **Memory Manager** - Heap and stack allocation/deallocation, buddy page allocator, slab allocator (see [Memory Manager](#-memory-manager))
- ✅ **Clean, documented code** - Easy to understand and extend

### Future Extensions (Student Assignments)

Students will extend kacchiOS by implementing:
- ✅ **Memory Manager** (implemented)
- 📝 **Process Manager**
- 📝 **Scheduler**

## 🚀 Quick Start

### Prerequisites

```bash
# On Ubuntu/Debian
sudo apt-get install build-essential qemu-system-x86 gcc-multilib

# On Arch Linux
sudo pacman -S base-devel qemu gcc-multilib

# On macOS
brew install qemu i686-elf-gcc
```

### Build and Run

```bash
# Clone the repository
git clone https://github.com/yourusername/kacchiOS.git
cd kacchiOS

# Build the OS
make clean
make

# Run in QEMU
make run
```

You should see:
```
========================================
    kacchiOS - Minimal Baremetal OS
========================================
Hello from kacchiOS!
Memory layout (size from Multiboot):
  Kernel image  0x00100000 - 0x0010D000  (52 KB)
  Buddy pool    0x0010D000 - 0x0050D000  (4 MB)
  Heap/stacks   0x0050D000 - 0x03FE0000  (60236 KB)
Heap   : 0 blocks, 0 bytes in use
Stacks : 0 stacks, 0 bytes in use
Free   : 60236 KB in 1 block(s), largest 60236 KB
Buddy  : 1024 / 1024 pages free
Free list:
  [0x0050D000, 60236 KB]
Type 'help' for memory commands.
Running null process...

kacchiOS> 
```

Type a memory command (`help`, `meminfo`, `buddyinfo`, `slabinfo`,
`memdemo`, `memtest`) or anything else and press Enter - other input is echoed back!

## 📁 Project Structure

```
kacchiOS/
├── boot.S          # Bootloader entry point (Assembly)
├── kernel.c        # Main kernel (null process + memory commands)
├── memory.h        # Memory manager public interface
├── memory.c        # Memory detection, heap & stack allocator, stats
├── buddy.c         # Buddy page allocator (optimised allocation)
├── slab.c          # Slab allocator + kmalloc()/kfree()
├── memtest.c       # Memory manager self tests ("memtest")
├── mm_internal.h   # Helpers shared by the memory manager files
├── serial.c        # Serial port driver (COM1)
├── serial.h        # Serial driver interface
├── string.c        # String utility functions
├── string.h        # String utility interface
├── types.h         # Basic type definitions
├── io.h            # I/O port operations
├── link.ld         # Linker script
├── Makefile        # Build system
├── Checklist.pdf   # Assignment checklist
└── README.md       # This file
```

## 🧠 Memory Manager

The memory manager has two layers:

| Layer | Part of the assignment | What it provides |
|-------|------------------------|------------------|
| **Core memory** | Must include | Heap allocation (`heap_alloc`) and stack allocation (`stack_alloc`) |
| **Memory optimisation** | Bonus | Heap/stack **deallocation** with coalescing (`heap_free`, `stack_free`), a **buddy allocator** (`buddy_alloc`/`buddy_free`), a **slab allocator** (`kmem_cache_*`) and `kmalloc`/`kfree` built on them |

All of it is initialised by `mem_init()`, which `kmain()` calls right after `serial_init()`.

### Memory Detection

`boot.S` sets bit 1 of the Multiboot header flags, so the bootloader (QEMU or GRUB)
reports how much RAM the machine has. The entry code passes the Multiboot magic
(`EAX`) and info pointer (`EBX`) to `kmain(magic, info)`. `mem_init()` reads
`mem_upper` (KB of contiguous RAM above 1 MB) to find the top of memory. If no
Multiboot information is present it assumes 32 MB.

### Memory Layout

```
 mem_top  +---------------------------+  e.g. 0x03FE0000 with -m 64M
          | stacks          (grow v)  |  stack_alloc(): LAST fit, from the top
          |                           |
          |        free space         |  one address-sorted free list
          |                           |
          | heap            (grow ^)  |  heap_alloc(): FIRST fit, from the bottom
          +---------------------------+  region_start
          | buddy pool (<= 4 MB)      |  buddy_alloc(), slab pages, kmalloc()
          +---------------------------+  pool_base = page-aligned __kernel_end
          | kernel .text/.data/.bss   |  includes the 16 KB boot stack
 0x100000 +---------------------------+  1 MB (link.ld)
```

The buddy pool takes at most 1/4 of free memory, capped at 4 MB (1024 pages).
Everything else becomes one free block shared by the heap and the stacks.

### Core: Heap Allocation

The design follows XINU's `getmem()`/`freemem()`. Free memory is a singly linked
list of `struct memblk { next; length; }` nodes kept **sorted by address**. Each
node is stored inside the free block it describes.

- **`void* heap_alloc(size_t n)`**: rounds `n + 16` (the header) up to a multiple
  of 16 and takes the **first** (lowest) free block that fits, splitting off the
  rest. Each block starts with a 16-byte header `{magic, size, requested, check}`,
  so the pointer returned to the caller is 16-byte aligned. Returns `NULL` when
  `n == 0` or no block is big enough.
- **`int heap_free(void* p)`**: checks that `p` is inside the region and
  16-byte aligned, and that its header has the right magic and checksum. It then
  poisons the header and returns the block to the list (see
  [Deallocation](#optimisation-deallocation-with-coalescing)).

### Core: Stack Allocation

The design follows XINU's `getstk()`/`freestk()`. Stacks come from the **same**
free list but use **last fit**: the highest free block that fits is cut from
its top. So stacks grow from the top of RAM downward and the heap grows upward,
and they only meet when memory is really full.

```
 base                                                   base + size
 | canary 16 B | ........ usable stack (grows down) | stack_hdr 16 B |
                                                    ^
                                                    sp returned by stack_alloc()
```

- **`void* stack_alloc(size_t n)`**: returns the **initial stack pointer** (16-byte
  aligned). A process uses the `n` bytes below it. A 16-byte canary
  (`0xDEADC0DE` x 4) is written at the bottom.
- **`int stack_check(void* sp)`**: returns `MEM_ERR` if the canary has been
  overwritten, i.e. the stack overflowed. A future scheduler can call it on
  every context switch.
- **`int stack_free(void* sp)`**: checks the header above `sp`, poisons it and
  returns the whole block to the free list.

### Optimisation: Deallocation with Coalescing

`heap_free()` and `stack_free()` both call `release_block()`, which:

1. walks the address-sorted list to the insertion point (`prev < block < next`),
2. **rejects** the block if it overlaps `prev` or `next`. This catches double
   frees and corrupted headers, and the call returns `MEM_ERR`,
3. **merges** with `prev` if `prev` ends where the block starts,
4. **merges** with `next` if the block ends where `next` starts.

Since neighbours always merge, freeing everything brings the list back to a
single block. `memtest` checks this after every group of tests.

### Optimisation: Buddy Allocator (`buddy.c`)

A binary buddy system manages the pool in blocks of 2<sup>k</sup> pages
(order 0 = 4 KB … order 10 = 4 MB), with one doubly linked free list per order.

- **`buddy_alloc(n)`**: picks the smallest order with `4096 << order >= n`. If that
  list is empty, it takes a larger block and **splits** it in half repeatedly,
  putting each upper half (the *buddy*) on the next lower free list.
- **`buddy_free(p)`**: the buddy of page `p` at order `k` is `p XOR 2^k`. If the
  buddy is free and has the same order, the two **merge** into one block of order
  `k+1`, and this repeats. Freed memory therefore always merges back into large
  blocks.
- One metadata byte per page (`page_info[]`: order, HEAD, FREE and SLAB flags)
  means freeing needs only the pointer. It also catches double frees and
  pointers into the middle of a block.
- Allocation and freeing both take O(log n), at most 10 split or merge steps.
  External fragmentation is bounded, and internal fragmentation is under 50%.

### Optimisation: Slab Allocator and kmalloc (`slab.c`)

A *cache* hands out objects of one size. Each **slab** is one 4 KB page from the
buddy allocator, with a small header at its start; the rest of the page is cut
into equal objects. Free objects are chained through their first word, so
allocating and freeing take **O(1)** and there is **no per-object header**.

```
 page: | struct slab | obj 0 | obj 1 | obj 2 | ... | obj n-1 |
```

- Each cache keeps **partial**, **full** and **empty** slab lists. Allocation uses
  a partial slab first, then a cached empty one, and only then takes a new page
  from the buddy allocator.
- When a slab becomes empty, one is kept cached (so alloc/free in a loop does not
  thrash the buddy allocator) and the others go back to the buddy allocator.
- API: `kmem_cache_create(name, size)`, `kmem_cache_alloc(c)`,
  `kmem_cache_free(c, obj)`, `kmem_cache_shrink(c)`, `kmem_cache_destroy(c)`.
- **`kmalloc(n)`**: uses the size classes 16, 32, 64, 128, 256, 512 and 1024
  bytes (the `kmalloc-*` caches). Larger requests get whole buddy pages.
  **`kfree(p)`** uses `page_info[]` to tell whether `p` belongs to a slab or
  to a buddy block.

### API Summary (`memory.h`)

| Function | Returns | Description |
|----------|---------|-------------|
| `mem_init(magic, info)` | - | Detect RAM, set up the free list, buddy pool and slab caches |
| `heap_alloc(n)` / `heap_free(p)` | ptr / `MEM_OK`·`MEM_ERR` | Heap, first fit, coalescing |
| `stack_alloc(n)` / `stack_free(sp)` | sp / status | Stacks, last fit from the top of RAM |
| `stack_check(sp)` | status | Stack overflow detection (canary) |
| `buddy_alloc(n)` / `buddy_free(p)` | ptr / status | 2<sup>k</sup>-page blocks |
| `kmem_cache_create/alloc/free/shrink/destroy` | - | Slab caches |
| `kmalloc(n)` / `kfree(p)` | ptr / status | Slab for ≤ 1024 B, buddy above |
| `mem_get_stats(&st)` | - | Free bytes/blocks, largest block, heap/stack usage |
| `mem_info()`, `buddy_info()`, `slab_info()` | - | Print state over serial |
| `mem_selftest()` | number of failures | Run the self tests |

Every free function returns `MEM_ERR` (-1) instead of corrupting memory when it
gets `NULL`, a foreign pointer, a misaligned pointer or a double free.

### Shell Commands

| Command | Description |
|---------|-------------|
| `help` | List memory commands |
| `meminfo` | Memory layout, heap/stack usage, free list |
| `buddyinfo` | Free blocks per buddy order |
| `slabinfo` | Slab caches (object size, objects per slab, slabs, active objects) |
| `memdemo` | Step-by-step demo of allocation, reuse, coalescing, buddy and slab |
| `memtest` | Run the self tests (62 checks) |

Any other input is echoed back, as before.

Example `memdemo` output (`make run`, 64 MB):

```
1) Heap grows up (first fit), stacks grow down (last fit)
  heap_alloc(100)    = a            0x0050D010
  heap_alloc(200)    = b            0x0050D090
  heap_alloc(300)    = c            0x0050D170
  stack_alloc(4096)  = s1           0x03FDFFF0
  stack_alloc(4096)  = s2           0x03FDEFD0
    -> free list: 1 block(s), 61672736 bytes free
2) Free b -> a hole appears in the middle of the heap
    -> free list: 2 block(s), 61672960 bytes free
3) heap_alloc(150) reuses b's hole (first fit)
  heap_alloc(150)    = b'           0x0050D090
4) Free everything -> neighbours coalesce
    -> free list: 1 block(s), 61681664 bytes free
5) Buddy allocator: sizes rounded to 2^k pages
  buddy_alloc(4096)  -> 1 page      0x0010D000
  buddy_alloc(10000) -> 4 pages     0x00111000
    -> freed, buddies merged back: 1024 pages free
6) Slab allocator: kmalloc(24) uses the 32-byte cache
  kmalloc(24)        = k1           0x0010D020
  kmalloc(24)        = k2 (k1+32)   0x0010D040
  kfree(k1); kmalloc = k1 reused    0x0010D020
```

### Testing

`memtest` runs 62 checks in the kernel and prints `[PASS]`/`[FAIL]` for each:

- **Heap**: alignment, upward growth, no overlap, reuse of freed holes, double
  free, misaligned/foreign/`NULL` pointers, zero and huge requests, coalescing
  with both neighbours, a stress test with 32 mixed-size blocks, and allocating
  the largest free block.
- **Stacks**: alignment, allocation from the top and downward growth, a full
  4 KB stack usable, overflow detected by the canary, double free, heap and
  stack pointers rejected by the wrong free function, full recovery.
- **Buddy**: rounding to 2<sup>k</sup> pages, no overlap, double free and
  interior pointers, requests bigger than the pool, using up the whole pool one
  page at a time and freeing it, merging back into a half-pool block.
- **Slab/kmalloc**: size-class selection and dense packing, LIFO reuse, double
  free, interior pointers, 300 objects across several slabs, empty slabs going
  back to buddy, large `kmalloc` served by buddy, custom cache life cycle.
- **Leak check**: after the run the free list and the buddy pool are exactly as
  they were before, so `memtest` can be run again and again.

It was tested in QEMU with `-m 8M`, `64M`, `128M` and `512M`: **62 passed, 0 failed** every time.

### Design Notes and Limitations

- No paging: kacchiOS runs with physical addresses only, so allocators return
  physical addresses.
- Interrupts stay disabled (`cli` in `boot.S`), so the allocators have no locks.
  Once a scheduler with preemption is added, each call must run with interrupts
  disabled, as XINU does with `disable()`/`restore()`.
- The buddy pool is at most 4 MB. Slab objects can be at most one page minus the
  slab header.

## 🛠️ Build System

### Makefile Targets

| Command | Description |
|---------|-------------|
| `make` or `make all` | Build kernel.elf |
| `make run` | Run in QEMU (serial output only) |
| `make run-vga` | Run in QEMU (with VGA window) |
| `make debug` | Run in debug mode (GDB ready) |
| `make clean` | Remove build artifacts |

## 📚 Learning Resources

### Recommended Reading

- [XINU OS](https://xinu.cs.purdue.edu/) - Educational OS similar to kacchiOS
- [OSDev Wiki](https://wiki.osdev.org/) - Comprehensive OS development guide
- [The Little OS Book](https://littleosbook.github.io/) - Practical OS development
- [Operating Systems: Three Easy Pieces](https://pages.cs.wisc.edu/~remzi/OSTEP/) - OS concepts textbook

### Related Topics

- x86 Assembly Language
- Memory Management
- Process Scheduling
- System Calls
- Interrupt Handling

## 🤝 Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.

### Guidelines

1. Keep code simple and educational
2. Add comments explaining complex concepts
3. Follow existing code style
4. Test changes in QEMU before submitting

## 📄 License

This project is licensed under the MIT License.

## 👨‍🏫 About

kacchiOS was created as an educational tool for teaching operating system concepts. It provides a minimal, working foundation that students can extend to learn core OS principles through hands-on implementation.

## 🙏 Acknowledgments

- Inspired by XINU OS
- Built with guidance from OSDev community
- Thanks to all students who have contributed
