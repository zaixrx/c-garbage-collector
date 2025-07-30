#include <assert.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "halloc.h"

#define UNTAG(p) (ChunkHeader*)((WORD)p & ~0b11)
#define TAG(p) (ChunkHeader*)((WORD)p | 0b1)
#define TAGGED(p) ((WORD)((ChunkHeader*)p->next) & 0b1)
#define HTAG(p) (ChunkHeader*)((WORD)p | 0b10)
#define HTAGGED(p) ((WORD)((ChunkHeader*)p->next) & 0b10)

#define OFF_SB_STAT_FIELD 28 - 3
#define MAX_REFERENCES 1024
#define DO_LOG
#ifdef DO_LOG
#define LOG(...) \
	printf(__VA_ARGS__)
#else
#define LOG(...)
#endif

// purely for testing purposes
static size_t refs_count = 0;
static void *refs[MAX_REFERENCES];
static size_t acc = 0;
#define CHUNK_SIZE 1024 - 16
int leak_heap() {
	void *temp_refs[acc];
	for (unsigned int i = 0; i < acc; ++i) {
		void *obj = halloc(CHUNK_SIZE);
		temp_refs[i] = obj;
		if (i % 2 == 0) {
			refs[refs_count++] = obj;
		} if (!obj) {
			perror("halloc");
			return -1;
		}
		printf("ADDREES: %p\n", obj);
	}
	memcpy(temp_refs[4], &temp_refs[5], WORD_SIZE);
	memcpy(temp_refs[5], &temp_refs[3], WORD_SIZE);
	memcpy(temp_refs[5] + 1, &temp_refs[1], WORD_SIZE);
	return 0;
}

// expects untagged used_chunks
void span_region(ChunkHeader *used_chunks, WORD *start, WORD *end) {
	for (WORD *wp = start; wp < end; wp++) {
		WORD w = *wp;
		ChunkHeader *ch = used_chunks;
		do {
			if ((WORD)(ch + 1) <= w && w < (WORD)((char*)(ch + 1) + ch->size)) {	
				ch->next = TAG(ch->next); // tag next because can't tag current!
				break;
			}
		} while ((ch = UNTAG(ch->next)) != used_chunks);
	}
}

void span_heap(ChunkHeader *used_chunks) {
	size_t work;
	ChunkHeader *ch;
	// mark work too to decrease redundency
	do {
		work = 0;
		ch = used_chunks;
		do {
			// if you find work
			if (TAGGED(ch) && !HTAGGED(ch)) {
				++work;
				ch->next = HTAG(ch->next);
				WORD *wp = (WORD*)(ch + 1), *wend;
				for (wend = (WORD*)((char*)wp + ch->size); wp < wend; ++wp) {
					WORD w = *wp;
					ChunkHeader *_ch = used_chunks;
					do {
						if (
							!HTAGGED(_ch) &&
							// contains the word
							(WORD)(_ch + 1) <= w && w < (WORD)((char*)(_ch + 1) + ch->size)
						) {
							// add to work list
							ch->next = TAG(ch->next);
							++work;
							break;
						}
					} while ((ch = UNTAG(ch->next)) != used_chunks);
				}
				--work;
			}
		} while ((ch = UNTAG(ch->next)) != used_chunks);
	} while (work);
}

void sweep_heap(ChunkHeader *used_chunks) {
	WORD start = (WORD)used_chunks; // end is dynamic
	LOG("start: 0x%lx\n", start);
	int i = 1;
	ChunkHeader *prev = used_chunks, *curr, *next;
	for (curr = UNTAG(prev->next);; curr = next, ++i) {
		LOG("{ %p: used_size: %zu, used_next: %p, skip: %s }\n", curr, curr->size, curr->next, TAGGED(curr) ? "true" : "false");
		if (TAGGED(curr)) {
			prev = curr;
			next = curr->next = UNTAG(curr->next);
		} else {
			next = curr->next;
			if (prev == curr) {
				set_used_chunks(NULL);
			} else {
				prev->next = TAGGED(prev) ? TAG(curr->next) : UNTAG(curr->next);
			}
			hfree(curr + 1);
			LOG("Clear\n");
		}
		if ((WORD)curr == start) break;
	}

	used_chunks = get_used_chunks();
	if ((curr = used_chunks)) {
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
	start = strchr(buf, ')');
	assert(start);
	token = strtok(++start, " "); // ignore process state
	do { token = strtok(NULL, " "); } while (token && ++counter != OFF_SB_STAT_FIELD);
	assert(counter == OFF_SB_STAT_FIELD);
	stack_bottom = atoll(token);

	// LOG("	.data start (etext)      %p\n", &__tdata_start);
	// LOG("	.bss end (end)  %p\n", &end) ;
	// LOG("	stack(top -- start) %p\n", (WORD*)stack_top);
	// LOG("	stack(bottom -- end) %p\n", (WORD*)stack_bottom);

	ChunkHeader *used_chunks = UNTAG(get_used_chunks());
	span_region(used_chunks, (WORD*)&__tdata_start, (WORD*)&end);
	span_region(used_chunks, (WORD*)stack_top, (WORD*)stack_bottom);
	span_heap  (used_chunks);

	sweep_heap(used_chunks);

	return 0;
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s <allocated_chunks_count> \n", argv[0]);
		return EXIT_FAILURE;
	}
	acc = atoi(argv[1]);
	if (acc <= 0 || MAX_REFERENCES <= acc) {
		fprintf(stderr, "0 < allocated_chunks_count < %d\n", MAX_REFERENCES);
		return EXIT_FAILURE;
	}
	if (leak_heap() == -1) return EXIT_FAILURE;
	if (collect_trash() == -1) return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
