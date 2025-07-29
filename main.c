#include <assert.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "halloc.h"

#define UNTAG(p) (ChunkHeader*)((WORD)p & ~1) // 1's compliment
#define CHUNK_SIZE 1024 - 16
#define OFF_SB_STAT_FIELD 28 - 3

#define DO_LOG
#ifdef DO_LOG
#define LOG(...) \
	printf(__VA_ARGS__)
#else
#define LOG(...)
#endif

static size_t refs_count = 0;
#define MAX_REFERENCES 1024
static void *refs[MAX_REFERENCES];

int leak_heap(ssize_t acc) {
	// LOG("--- references ---\n");
	for (int i = 0; i < acc; ++i) {
		void *obj = halloc(CHUNK_SIZE);
		if (i % 3 == 0) {
			// LOG("reference at %p -> %p\n", (char*)refs + WORD_SIZE * refs_count, refs[refs_count]);
			refs[refs_count++] = obj;
		}
		if (!obj) {
			perror("halloc");
			return -1;
		}
	}
	// LOG("--- references ---\n");
	return 0;
}

// expects untagged used_chunks
void span_region(ChunkHeader *used_chunks, WORD *start, WORD *end) {
	for (WORD *ptr = start; ptr < end; ptr++) {
		WORD ptr_addr = *ptr;
		ChunkHeader *curr = used_chunks;
		do {
			if ((WORD)(curr + 1) <= ptr_addr && ptr_addr < (WORD)((char*)(curr + 1) + curr->size)) {	
				curr->next = (ChunkHeader*)((WORD)curr->next | 1); // tag next because can't tag current!
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
	WORD start = (WORD)used_chunks; // end is dynamic
	LOG("start: 0x%lx\n", start);

	int i = 0;
	ChunkHeader *prev = used_chunks, *curr, *next;
	for (curr = UNTAG(prev->next);; curr = next, ++i) {
		int skip = (WORD)(curr->next) & 0x1;
		// int leaked = i % 3 != 0;
		LOG("{ %p: used_size: %zu, used_next: %p, skip: %s }\n", curr, curr->size, curr->next, skip ? "true" : "false");
		// assert(leaked != skip && "ERROR: skipping LEAKED\n");
		if (skip) {
			prev = curr;
			next = curr->next = UNTAG(curr->next);
		} else {
			next = curr->next;
			if (prev == curr) {
				set_used_chunks(NULL);
			} else {
				prev->next = curr->next;
			}
			hfree(curr + 1);
			LOG("Clear\n");
		}
		if ((WORD)curr == start) break;
	}

	if ((curr = get_used_chunks())) {
		do {
			LOG("%p is still there\n", curr);
		} while ((curr = UNTAG(curr->next)) != used_chunks);
	}
}

static char buf[1024];
extern char __tdata_start, end;

int collect_trash() {
	char *start, *token;
	FILE *statfp;
	WORD stack_top, stack_bottom;
	size_t counter = 0;

	// https://gcc.gnu.org/onlinedocs/gcc/Extended-Asm.html
	asm volatile ("mov %%rsp, %0" : "=r" (stack_top));

	// parse to find stack_bottom
	statfp = fopen("/proc/self/stat", "r");
	assert(statfp != NULL);
	fgets(buf, sizeof buf, statfp);
    	fclose(statfp);
	LOG("buffer: %s\n", buf);
	start = strchr(buf, ')');
	assert(start);
	token = strtok(++start, " "); // ignore process state
	do { token = strtok(NULL, " "); } while (token && ++counter != OFF_SB_STAT_FIELD);
	assert(counter == OFF_SB_STAT_FIELD);
	stack_bottom = atoll(token);

	// Unreliable... maybe
    	// fscanf(statfp,
    	//       "%*d %*s %*c %*d %*d %*d %*d %*d %*u "
    	//       "%*lu %*lu %*lu %*lu %*lu %*lu %*ld %*ld "
    	//       "%*ld %*ld %*ld %*ld %*llu %*lu %*ld "
    	//       "%*lu %*lu %*lu %lu", &stack_bottom);

	// LOG("	.data start (etext)      %p\n", &__tdata_start);
	// LOG("	.bss end (end)  %p\n", &end) ;
	// LOG("	stack(top -- start) %p\n", (WORD*)stack_top);
	// LOG("	stack(bottom -- end) %p\n", (WORD*)stack_bottom);

	ChunkHeader *used_chunks = UNTAG(get_used_chunks());
	span_region(used_chunks, (WORD*)&__tdata_start, (WORD*)&end);
	span_region(used_chunks, (WORD*)stack_top, (WORD*)stack_bottom);
	span_heap  (used_chunks);

	sweep_heap (used_chunks);
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
