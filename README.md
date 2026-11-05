# Memory-Allocator

A custom dynamic memory allocator implemented in **C**, providing alternatives to the standard memory-management functions `malloc`, `calloc`, `realloc`, and `free`.

The project focuses on understanding what happens internally when a program requests dynamic memory. Instead of relying on the allocator provided by the standard C library, the implementation manages memory blocks directly and communicates with the operating system through mechanisms such as `sbrk`, `mmap`, and `munmap`.

The allocator keeps its own metadata for every managed block, searches for reusable memory, splits blocks when they are larger than necessary, merges neighboring free regions, expands the heap when additional space is required, and handles large allocations separately using memory mapping.

The main purpose of the project is to explore heap management, memory fragmentation, allocation strategies, pointer arithmetic, and the lower-level mechanisms behind functions that are normally used without having to think about their internal implementation.

## Memory Management Strategy

The allocator uses two different approaches depending on the size of the requested allocation.

Smaller allocations are managed through the process heap using `sbrk`. The allocator maintains a linked structure of memory blocks and attempts to reuse previously allocated regions whenever possible.

Larger allocations are handled independently using `mmap`. These blocks do not participate in the normal heap block list and are released directly using `munmap`.

For `os_malloc`, the boundary between the two strategies is 128 KB. Requests smaller than this threshold are normally handled through the heap, while requests equal to or larger than the threshold are mapped separately.

This makes it possible to manage small allocations efficiently inside a reusable heap while avoiding unnecessary heap growth for very large memory requests.

## Heap Preallocation and Block Metadata

When the first small allocation is requested, the allocator reserves an initial heap region of 128 KB using `sbrk`.

Instead of requesting new memory from the operating system for every small allocation, this initial region is represented as one large free block that can later be divided and reused.

Every heap block contains metadata describing the region.

The metadata stores the size of the block, its current status, and pointers to the previous and next blocks. This effectively creates a doubly linked list containing the memory regions managed by the allocator.

A block may represent allocated memory, free memory, or memory obtained separately through `mmap`.

When memory is returned to the caller, the pointer refers to the usable area immediately after the block metadata. When the allocator later receives that pointer through `os_free` or `os_realloc`, it can move backwards in memory to recover the corresponding metadata structure.

This organization makes it possible to inspect neighboring blocks, search for free regions, divide blocks, and merge unused memory.

## Alignment and Allocation

Requested sizes are aligned to 8-byte boundaries before memory is allocated.

Memory alignment ensures that returned regions are positioned appropriately for common data types and also makes the internal layout of blocks easier to manage.

When `os_malloc` receives a small allocation request, the allocator first checks whether the heap has already been initialized. If necessary, the initial heap area is created.

The allocator then searches the existing block list for a free region large enough to satisfy the request.

Instead of selecting the first available block, the implementation uses a **best-fit allocation strategy**.

The entire list is searched and the smallest free block that is still large enough for the requested allocation is selected.

This attempts to avoid using an unnecessarily large free block when a more appropriate region is already available.

If no reusable block exists, the allocator can extend the process heap using `sbrk` and create a new allocated block at the end of the existing list.

If the last block in the heap is free, the allocator can also expand that block directly rather than creating another metadata entry.

## Block Splitting

A free block may be significantly larger than the allocation that needs to be placed inside it.

In that situation, using the complete block would waste memory. The allocator therefore supports block splitting.

The beginning of the original block becomes the allocated region requested by the caller, while the remaining memory is converted into a new free block.

The new block receives its own metadata and is inserted into the linked structure between the current block and its former successor.

Splitting only occurs when enough space remains for another metadata structure together with a usable memory region.

This allows the allocator to reuse part of a larger free block while keeping the rest available for future requests.

## Freeing and Block Coalescing

The custom `os_free` implementation treats heap allocations and mapped allocations differently.

For a regular heap allocation, the block is marked as free instead of immediately returning the memory to the operating system.

Keeping freed blocks inside the managed heap allows later calls to `os_malloc` or `os_calloc` to reuse the same memory.

After a block is marked as free, the allocator checks its neighboring regions.

If the next block is also free, the two blocks are merged into a single larger region. The same check is performed for the previous block.

This process is known as **coalescing**.

Without coalescing, the heap could gradually become divided into many small free blocks. Even if the total amount of available memory were large enough, a future allocation could fail to find one sufficiently large contiguous region.

By merging adjacent free blocks, the allocator reduces this type of external fragmentation and reconstructs larger reusable sections of heap memory.

Mapped blocks behave differently. Because they were allocated independently with `mmap`, they are released directly with `munmap` rather than being inserted into the heap's free-block system.

## calloc Implementation

The allocator also implements `os_calloc`, which allocates memory for multiple elements and initializes the entire resulting region to zero.

The total requested size is calculated from the number of elements and the size of each element and is then aligned before allocation.

After the memory is obtained, the region is initialized using `memset`.

The allocation strategy used by `os_calloc` is similar to the one used by `os_malloc`, although its mapping threshold is based on the system page size.

Smaller allocations can reuse the preallocated heap and participate in block splitting and best-fit selection, while larger requests are handled through a separate mapped region.

This implementation combines the allocation logic of the custom allocator with the initialization behavior expected from `calloc`.

## realloc Implementation

`os_realloc` is one of the more involved parts of the allocator because resizing an existing block can require several different strategies.

If the supplied pointer is `NULL`, the function behaves like a new allocation.

If the requested size is zero, the existing block is freed and the function returns `NULL`.

When an allocated block is already large enough for the new request, it can remain in the same location. The allocator may split the excess portion and turn the remaining space into another free block.

If the block is the last block in the heap, the allocator can attempt to extend it directly using `sbrk`. This allows the allocation to grow without moving the existing data.

Another possibility is that one or more blocks immediately following the allocation are free. In this case, the allocator merges those neighboring regions into the current block until enough space has been obtained or no additional free block is available.

If the enlarged block becomes large enough, it is reused in place and may be split again if there is excess space.

When none of these approaches can satisfy the new size, a completely new allocation is created. The existing data is copied with `memcpy`, after which the old block is released.

Mapped allocations and requests that become large enough to use the mapping strategy may also require creating a new region and transferring the previous contents.

This makes `os_realloc` a combination of several allocator operations: allocation, block merging, heap extension, splitting, copying, and freeing.

## Fragmentation and Memory Reuse

Fragmentation is one of the main problems that a dynamic memory allocator has to manage.

Internal fragmentation occurs when an allocated region contains more space than the program actually requested. Alignment requirements and minimum block sizes naturally introduce some amount of this overhead.

External fragmentation occurs when free memory is available but is divided into separate regions that are individually too small for a new request.

The allocator addresses these problems through several mechanisms.

Best-fit allocation attempts to choose a free block that closely matches the requested size.

Block splitting prevents a large free region from being completely consumed by a much smaller allocation.

Coalescing combines neighboring free blocks into larger regions after memory is released.

Previously freed heap blocks remain available for future allocations rather than immediately being discarded.

Together, these mechanisms allow the allocator to reuse memory and maintain a more useful heap layout as allocations and deallocations occur over time.

## Interaction with the Operating System

The allocator relies directly on Linux memory-management interfaces.

`sbrk` is used to inspect and modify the program break, allowing the allocator to reserve and extend the process heap.

The first small allocation causes the allocator to reserve the initial heap area, while later allocations can extend the heap when the existing blocks cannot satisfy a request.

`mmap` is used for large allocations that should be managed independently from the regular heap.

These mapped regions are created with read and write permissions and anonymous private mappings.

`munmap` is used to release these independently mapped allocations.

Using both heap expansion and memory mapping provides a practical view of two different mechanisms through which user-space memory allocators can obtain memory from the operating system.

## Project Structure

- `osmem.c` — implementation of the custom allocator and all memory-management operations
- `Makefile` — build configuration for creating the shared library
- external `block_meta` definitions — metadata structure and block status definitions used by the allocator
- external utility code — supporting functionality referenced through the `UTILS_PATH` variable

Most of the allocator logic is concentrated in `osmem.c`.

The file contains the implementations of `os_malloc`, `os_calloc`, `os_realloc`, and `os_free`, together with helper functions responsible for heap preallocation, best-fit searching, block splitting, block coalescing, heap extension, and large mapped allocations.

## Build

The project uses **GCC** and **GNU Make**.

Running `make` builds the allocator as the shared library `libosmem.so`.

The source is compiled with position-independent code through `-fPIC`, while the final linking stage uses `-shared` to create the shared object.

The current Makefile also references supporting files through the `UTILS_PATH` variable, which by default points to `../utils`.

The project is designed around Linux-specific interfaces such as `sbrk`, `mmap`, and `munmap`, so it is intended to be built and used in a Linux environment.

## Technologies and Concepts

- C
- Linux
- Dynamic memory allocation
- Heap management
- Virtual memory
- `sbrk`
- `mmap`
- `munmap`
- Custom `malloc`
- Custom `calloc`
- Custom `realloc`
- Custom `free`
- Memory alignment
- Block metadata
- Doubly linked structures
- Best-fit allocation
- Block splitting
- Block coalescing
- Memory reuse
- Internal fragmentation
- External fragmentation
- Pointer arithmetic
- `memcpy`
- `memset`
- Shared libraries
- GCC
- GNU Make
