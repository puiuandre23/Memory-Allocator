// SPDX-License-Identifier: BSD-3-Clause

#include <sys/mman.h>
#include <unistd.h>
#include <stddef.h>
#include <string.h>
#include "../utils/printf.h"
#include "../utils/block_meta.h"
#include "printf.h"

#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))
#define SIZE_T_SIZE (ALIGN(sizeof(size_t)))
#define MMAP_THRESHOLD (128 * 1024)
#define HEAP_PREALLOC_SIZE (128 * 1024)
#define block_aligned (ALIGN(sizeof(struct block_meta)))

struct block_meta *block_list;
int heap_preallocated;

struct block_meta *get_block_ptr(void *ptr)
{
	return (struct block_meta *)((char *)ptr - block_aligned);
}

struct block_meta *preallocate_heap(void)
{
	if (heap_preallocated)
		return block_list;
	struct block_meta *initial_block = sbrk(0);
	void *request = sbrk(HEAP_PREALLOC_SIZE);

	if ((long)request < 0) {
		errno = -(long)request;
		return NULL;
	}
	initial_block->size = HEAP_PREALLOC_SIZE - block_aligned;
	initial_block->status = STATUS_FREE;
	initial_block->prev = NULL;
	initial_block->next = NULL;
	heap_preallocated = 1;
	return initial_block;
}

void coalesce(struct block_meta *block)
{
	if (block->next) {
		if (block->next->status == STATUS_FREE) {
			block->size += block->next->size + block_aligned;
			block->next = block->next->next;
			if (block->next)
				block->next->prev = block;
		}
	}
	if (block->prev) {
		if (block->prev->status == STATUS_FREE) {
			block->prev->size += block->size + block_aligned;
			block->prev->next = block->next;
			if (block->next)
				block->next->prev = block->prev;
		}
	}
}

struct block_meta *find_best_fit(size_t size)
{
	struct block_meta *current = block_list;
	struct block_meta *best_fit = NULL;

	while (current) {
		if (current->status == STATUS_FREE && current->size >= size)
			if (!best_fit || current->size < best_fit->size)
				best_fit = current;
		current = current->next;
	}
	return best_fit;
}

struct block_meta *extend_heap(size_t size)
{
	size = ALIGN(size);
	struct block_meta *block = sbrk(0);
	void *request = sbrk(block_aligned + size);

	if (request == (void *)-1)
		return NULL;
	struct block_meta *current = block_list;

	while (current->next)
		current = current->next;
	current->next = block;
	block->size = size;
	block->status = STATUS_ALLOC;
	block->prev = current;
	block->next = NULL;
	return block;
}

struct block_meta *request_large_block(size_t size)
{
	size = ALIGN(size);
	size_t total_size = size + block_aligned;
	struct block_meta *block = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (block == MAP_FAILED)
		return NULL;
	block->size = size;
	block->status = STATUS_MAPPED;
	block->next = NULL;
	block->prev = NULL;
	return block;
}

void split_block(struct block_meta *block, size_t size)
{
	size = ALIGN(size);

	if (block->size >= size + block_aligned + 8) {
		struct block_meta *new_block = (struct block_meta *)((char *)block + block_aligned + size);

		new_block->size = block->size - size - block_aligned;
		new_block->status = STATUS_FREE;
		if (block->next)
			block->next->prev = new_block;
		new_block->next = block->next;
		new_block->prev = block;
		block->size = size;
		block->next = new_block;
	}
	block->status = STATUS_ALLOC;
}

void *os_malloc(size_t size)
{
	if (size == 0)
		return NULL;
	size = ALIGN(size);

	if (!heap_preallocated && size < MMAP_THRESHOLD)
		block_list = preallocate_heap();
	struct block_meta *block;

	if (size >= MMAP_THRESHOLD) {
		block = request_large_block(size);
		return (block + 1);
	}
	block = find_best_fit(size);
	if (block) {
		split_block(block, size);
	} else {
		struct block_meta *current = block_list;

		while (current->next)
			current = current->next;
		if (current->status == STATUS_FREE) {
			size_t old_size = current->size;

			sbrk(size - old_size);
			current->size = size;
			current->status = STATUS_ALLOC;
			return (current + 1);
		}
		block = extend_heap(size);
		if (!block)
			return NULL;
	}
	return (block + 1);
}

void *os_calloc(size_t nmemb, size_t size)
{
	if (size == 0 || nmemb == 0)
		return NULL;
	size_t total_size = ALIGN(size * nmemb);
	size_t page_size = getpagesize();

	if (!heap_preallocated && total_size + block_aligned < page_size)
		block_list = preallocate_heap();
	struct block_meta *block;

	if (total_size + block_aligned >= page_size) {
		block = request_large_block(total_size);
		memset(block + 1, 0, total_size);
		return (block + 1);
	}
	block = find_best_fit(total_size);

	if (block) {
		split_block(block, total_size);
	} else {
		struct block_meta *current = block_list;

		while (current->next)
			current = current->next;
		if (current->status == STATUS_FREE) {
			size_t old_size = current->size;

			sbrk(total_size - old_size);
			current->size = total_size;
			current->status = STATUS_ALLOC;
			memset(current + 1, 0, total_size);
			return (current + 1);
		}
		block = extend_heap(total_size);
		if (!block)
			return NULL;
	}
	memset(block + 1, 0, total_size);
	return (block + 1);
}

void os_free(void *ptr)
{
	if (!ptr)
		return;
	struct block_meta *block = get_block_ptr(ptr);

	if (block->status == STATUS_FREE)
		return;
	if (block->status == STATUS_MAPPED) {
		munmap(block, block->size + block_aligned);
	} else {
		block->status = STATUS_FREE;
		coalesce(block);
	}
}

void *os_realloc(void *ptr, size_t size)
{
	if (!ptr)
		return os_malloc(size);
	if (size == 0) {
		os_free(ptr);
		return NULL;
	}
	struct block_meta *block = get_block_ptr(ptr);

	size = ALIGN(size);
	if (size >= MMAP_THRESHOLD) {
		struct block_meta *new_block = get_block_ptr(os_malloc(size));
		size_t copy_size;

		if (size < block->size)
			copy_size = size;
		else
			copy_size = block->size;
		memcpy(new_block + 1, ptr, copy_size);
		os_free(ptr);
		return (new_block + 1);
	}
	if (block->status == STATUS_FREE)
		return NULL;
	if (block->status == STATUS_MAPPED) {
		struct block_meta *new_block = get_block_ptr(os_malloc(size));

		memcpy(new_block + 1, ptr, size);
		os_free(ptr);
		return (new_block + 1);
	}
	if (block->size >= size) {
		split_block(block, size);
		return (block + 1);
	}
	struct block_meta *current = block_list;

	while (current->next)
		current = current->next;
	if (block == current) {
		size_t old_size = current->size;

		sbrk(size - old_size);
		current->size = size;
		current->status = STATUS_ALLOC;
		return (current + 1);
	}
	while (block->size < size && block->next && block->next->status == STATUS_FREE) {
		block->size += block->next->size + block_aligned;
		block->next = block->next->next;
		if (block->next)
			block->next->prev = block;
	}
	if (block->size >= size) {
		split_block(block, size);
		return (block + 1);
	}
	struct block_meta *new_block = get_block_ptr(os_malloc(size));
	size_t copy_size;

	if (size < block->size)
		copy_size = size;
	else
		copy_size = block->size;
	memcpy(new_block + 1, ptr, copy_size);
	os_free(ptr);
	return (new_block + 1);
}




