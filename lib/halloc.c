#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "../halloc.h"

#ifdef DO_LOG
#define LOG(...) \
	printf(__VA_ARGS__)
#else
#define LOG(...)
#endif

// not caring about base being part of the free_chunks is fine
// it doesn't affect functionality and the performance cost
// is negligible, also &base < any_heap_addres is always true
// we do that because it allows free_chunks to not be empty at all
// expect maybe for the beginning which simplifies alot of stuff
static ChunkHeader base = {0};
static ChunkHeader *free_chunks = NULL;
static ChunkHeader *used_chunks = NULL; // useful for things like garbage colletion

extern ChunkHeader *get_used_chunks() {
	return used_chunks;
}

static inline size_t round_up_to(size_t size, size_t to) {
	if (size % to == 0) return size;
	return size + ((to) - (size % to));
}

static ChunkHeader *dchunk(size_t words) {
	ChunkHeader *chunk = sbrk(words * WORD_SIZE + HEADER_SIZE);
	if (chunk == (void*)-1) return NULL;
	chunk->size = words * WORD_SIZE;
	hfree((ChunkHeader*)chunk + 1);
	return chunk;
}

// a call with size of 0 sets up an empty page!
extern void *halloc(size_t size) {
	// cyclic singly linked list
	if (free_chunks == NULL) {
		free_chunks = &base;
		free_chunks->next = free_chunks;
	}

	if ((size = round_up_to(size, WORD_SIZE)) == 0) return NULL;

	ChunkHeader *curr, *prev = free_chunks;
	for (curr = prev->next;; curr = (prev = curr)->next) {
		if (curr->size >= size) {
			if (curr->size > HEADER_SIZE + size) {
				curr->size -= size + HEADER_SIZE;
				curr = (ChunkHeader*)(((char*)curr + HEADER_SIZE) + curr->size);
				curr->size = size;
				LOG("wrote to %p size %zu\n", curr, size);
			} else {
				LOG("completely used %zu bytes\n", curr->size);
				prev->next = curr->next;
			}

			if (used_chunks == NULL) {
				used_chunks = curr->next = curr;
			} else {
				curr->next = used_chunks->next;
				used_chunks->next = curr;
			}

			return (void*)(curr + 1);
		}

		if (curr == free_chunks) {
#ifdef USE_ONE_PAGE
			errno = ENOMEM;
			return NULL;
#else
			// on success dchunk is gonna take care of the next step
			if ((curr->next = dchunk(round_up_to(size, PAGE_SIZE) / WORD_SIZE)) == NULL) {
				return NULL;
			}
#endif
		}
	}
}

extern void hfree(void *ptr) {
	ChunkHeader *header = (ChunkHeader*)ptr - 1, *prev = free_chunks, *curr;
	for (curr = prev->next; !(header < curr || curr == free_chunks); curr = (prev = curr)->next);

	if ((char*)header + HEADER_SIZE + header->size == (char*)curr) {
		header->size += HEADER_SIZE + curr->size;
		header->next  = curr->next;
		LOG("merged RS, new size: %zu\n", header->size);
	} else {
		header->next = curr;
		LOG("merely added chunk %p\n", header);
	}

	if ((char*)prev + HEADER_SIZE + prev->size == (char*)header) {
		prev->size += HEADER_SIZE + header->size;
		prev->next  = header->next;
		LOG("merged LS, new size: %zu\n", prev->size);
	} else {
		prev->next = header;
		LOG("merely added chunk %p\n", header);
	}
}

extern void *hcalloc(size_t size) {
	size = round_up_to(size, WORD_SIZE);
	uintptr_t *data = halloc(size);
	if (data == NULL) return NULL;
	for (WORD i = 0; i < (size / WORD_SIZE); ++i) *data = 0x0; // meow hehe
	return data;
}

// used to work nice with other kinds of know size memory
extern void hbfree(void *ptr, size_t size) {
	ChunkHeader *header = (ChunkHeader*)ptr;
	header->size = size - HEADER_SIZE;
	hfree(header + 1);
}
