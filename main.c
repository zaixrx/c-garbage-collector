#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "halloc.h"

#define UNTAG(p) (ChunkHeader*)((WORD)p & ~1) // 1's compliment
#define CHUNK_SIZE 1024 - 16
#define MAX_REFERENCES 1024

#define DO_LOG
#ifdef DO_LOG
#define LOG(...) \
	printf(__VA_ARGS__)
#else
#define LOG(...)
#endif

static void *refs[MAX_REFERENCES];

int leak_heap(ssize_t acc) {
	for (int i = 0; i < acc; ++i) {
		void *obj = halloc(CHUNK_SIZE);
		if (i % 4 == 0) {
			refs[i] = obj;
		}
		if (!obj) {
			perror("halloc");
			return -1;
		}
	}
	return 0;
}


// expects untagged used_chunks
void span_region(ChunkHeader *used_chunks, WORD *start, WORD *end) {
	for (WORD *ptr = start; ptr < end; ptr++) {
		WORD ptr_addr = *ptr;
		ChunkHeader *curr = used_chunks;
		do {
			if ((WORD)(curr + 1) <= ptr_addr && ptr_addr < (WORD)((char*)(curr + 1) + curr->size)) {
				// tag next because can't tag current!
				curr->next = (ChunkHeader*)((WORD)curr->next | 1);
				break;
			}
		} while ((curr = UNTAG(curr->next)) != used_chunks);
	}
}

// expects untagged used_chunks
void span_heap(ChunkHeader *used_chunks) {
	ChunkHeader *curr = used_chunks;
	do {
		span_region(used_chunks, (WORD*)curr, (WORD*)((char*)(curr + 1) + curr->size));
	} while ((curr = UNTAG(curr->next)) != used_chunks);
}

void sweep_heap(ChunkHeader *used_chunks) {
	TODO("sweep_heap");
}

int collect_trash() {
	WORD stack_top, stack_bottom;
	extern char __tdata_start, end;
	FILE *statfp;

	// https://gcc.gnu.org/onlinedocs/gcc/Extended-Asm.html
	asm volatile ("mov %%rsp, %0" : "=r" (stack_top));

	statfp = fopen("/proc/self/stat", "r");
	assert(statfp != NULL);
    	fscanf(statfp,
    	       "%*d %*s %*c %*d %*d %*d %*d %*d %*u "
    	       "%*lu %*lu %*lu %*lu %*lu %*lu %*ld %*ld "
    	       "%*ld %*ld %*ld %*ld %*llu %*lu %*ld "
    	       "%*lu %*lu %*lu %lu", &stack_bottom);
    	fclose(statfp);

	LOG("    .data start (etext)      %p\n", &__tdata_start);
	LOG("    .bss end (end)  %p\n", &end);
	LOG("    stack(top -- start) %p\n", (WORD*)stack_top);
	LOG("    stack(bottom -- end) %p\n", (WORD*)stack_bottom);

	ChunkHeader *used_chunks = UNTAG(get_used_chunks());
	span_region(used_chunks, (WORD*)&__tdata_start, (WORD*)&end);
	span_region(used_chunks, (WORD*)stack_top, (WORD*)stack_bottom);
	span_heap  (used_chunks);

	int i = 0;
	ChunkHeader *curr = used_chunks;
	do {
		LOG("{ used_size: %zu, used_next: %p, leaked: %s, tagged: %s }\n",
				curr->size, curr->next, i % 4 != 0 ? "true" : "false",
				(WORD)curr->next & 0x1 ? "true" : "false");
		i++;
	} while ((curr = UNTAG(curr->next)) != used_chunks);

	sweep_heap (used_chunks);

	return 0;
}

int main(int argc, char **argv) {
	size_t acc;
	if (argc != 2) {
		fprintf(stderr, "usage: %s <allocated_chunks_count> \n", argv[0]);
		return EXIT_FAILURE;
	}
	acc = atoi(argv[1]);
	if (acc <= 0 || MAX_REFERENCES <= acc) {
		fprintf(stderr, "0 < allocated_chunks_count < %d\n", MAX_REFERENCES);
		return EXIT_FAILURE;
	}
	if (leak_heap(acc) == -1) return EXIT_FAILURE;
	if (collect_trash() == -1) return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
