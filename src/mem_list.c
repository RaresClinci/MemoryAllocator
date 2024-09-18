// SPDX-License-Identifier: BSD-3-Clause

#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/param.h>
#include <stdlib.h>
#include <stdio.h>
#include "printf.h"
#include "block_meta.h"
#include "mem_list.h"

// functie initializare heap
void init(size_t size)
{
	if (size + ALIGN(METADATA_SIZE) < MMAP_THRESHOLD) {
		// prealocam memorie(128kB)
		heap = sbrk(MMAP_THRESHOLD);
		DIE(heap == (void *)-1, "sbrk");
		heap->status = STATUS_ALLOC;
		heap->size = size;

		// marcam ce este in afara ca memorie libera
		if (MMAP_THRESHOLD - size - ALIGN(METADATA_SIZE) > ALIGN(METADATA_SIZE)) {
			// exista memorie pe care sa o marcam
			heap->next = (struct block_meta *)((char *)heap + ALIGN(METADATA_SIZE) + ALIGN(size));
			heap->next->size = MMAP_THRESHOLD - ALIGN(size) - 2 * ALIGN(METADATA_SIZE);
			heap->next->status = STATUS_FREE;
			heap->next->next = heap;
			heap->next->prev = heap;
			heap->prev = heap->next;
		} else {
			// nu exista memorie pe care sa o marcam
			heap->next = heap;
			heap->prev = heap;
		}
	} else {
		// alocam cu mmpap
		heap = mmap(NULL, size + ALIGN(METADATA_SIZE), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		DIE(heap == (void *)-1, "mmap");
		heap->status = STATUS_MAPPED;
		heap->size = size;
		heap->next = heap;
		heap->prev = heap;
	}
}

// functie unire 2 blocuri
void merge(struct block_meta *mem1, struct block_meta *mem2)
{
	// mem1->size = mem2 - mem1 + ALIGN(METADATA_SIZE) + mem2->size;

	if (mem1->next == mem2) {
		// blocurile nu au nimic intre ele in lista
		mem1->next = mem2->next;
		mem2->next->prev = mem1;
	} else {
		// mutam ce este intre in fata
		mem2->prev->next = mem2->next;
		mem2->next->prev = mem2->prev;
	}

	// calculam noul size
	mem1->size = (char *)mem2 - (char *)mem1 + ALIGN(mem2->size);
}

// functie pentru coalesce
void coalesce(void)
{
	struct block_meta *iter = heap, *to_merge, *aux;

	// nu exista blocuri alocate
	if (heap == NULL)
		return;

	// parcurgem toate zonele de memorie libere
	do {
		if (iter->status == STATUS_FREE) {
			to_merge = iter->next;
			// cautam blocuri libere adiacente pentru a le unii cu iter
			while (to_merge != heap && to_merge->status != STATUS_ALLOC) {
				if (to_merge->status == STATUS_FREE) {
					// unim blocurile
					aux = to_merge->next;
					merge(iter, to_merge);
					to_merge = aux;
				} else {
					to_merge = to_merge->next;
				}
			}
		}
		iter = iter->next;
	} while (iter != heap);
}

// functie cautare cel mai bun loc
struct block_meta *find_best(size_t size)
{
	struct block_meta *iter = heap, *min = NULL;

	// aplicam o coalescenta
	coalesce();

	// tratam cazul in care exista doar o zona
	if (iter->next == heap) {
		if (heap->size < size)
			return NULL;
		if (iter->status == STATUS_FREE)
			return heap;
		else
			return NULL;
	}

	// cautam zona potrivita
	do {
		if (iter->status == STATUS_FREE)
			if (ALIGN(iter->size) >= size) {
				if (min == NULL)
					min = iter;
				else
					if (min->size > iter->size)
						min = iter;
			}
		iter = iter->next;
	} while (iter != heap);

	return min;
}



// functie spargere bloc
void split_block(struct block_meta *mem, size_t size)
{
	// cautam unde se termina blocul curent
	struct block_meta *end = mem->next;
	int found = 0;

	while (end != heap) {
		if (end->status != STATUS_MAPPED) {
			found = 1;
			break;
		}
		end = end->next;
	}

	// nu s-a gasit un bloc
	if (found == 0) {
		end = sbrk(0);
		DIE(end == (void *)-1, "sbrk");
	}

	if ((char *)end - (char *)mem - ALIGN(size) - ALIGN(METADATA_SIZE) > ALIGN(METADATA_SIZE)) {
		// adaugam un nou bloc la lista
		struct block_meta *new = (struct block_meta *)((char *)mem + ALIGN(METADATA_SIZE) + ALIGN(size));

		// cream legaturile
		new->prev = mem;
		new->next = mem->next;
		mem->next = new;
		new->next->prev = new;

		// determinam dimensiunile blocurilor
		new->status = STATUS_FREE;
		new->size = (char *)end - (char *)mem - ALIGN(size) - 2 * ALIGN(METADATA_SIZE);
		mem->size = ALIGN(size);
	}

	mem->status = STATUS_ALLOC;
	mem->size = size;
}

// funtcie pentru expansiunea ultimului bloc
struct block_meta *expand_last(size_t new_size)
{
	// cautam ultimul bloc free(daca exista)
	struct block_meta *last = NULL, *iter = heap;

	do {
		if (iter->status != STATUS_MAPPED)
			last = iter;
		iter = iter->next;
	} while (iter != heap);

	if (last == NULL || last->status == STATUS_ALLOC)
		return NULL;

	void *end = sbrk(0);

	DIE(end == (void *)-1, "sbrk");

	void *res = sbrk(ALIGN(new_size) - ((char *)end - (char *)last - ALIGN(METADATA_SIZE)));

	DIE(res == (void *)-1, "sbrk");

	last->size = new_size;
	last->status = STATUS_ALLOC;

	return last;
}

// functie adaugare la lista
struct block_meta *push(size_t size)
{
	struct block_meta *new, *last = heap->prev;

	// alocam memoria in functie de marime
	if (size < MMAP_THRESHOLD) {
		new = sbrk(ALIGN(size) + ALIGN(METADATA_SIZE));
		DIE(new == (void *)-1, "sbrk");
		new->status = STATUS_ALLOC;
	} else {
		new = mmap(NULL, ALIGN(size) + ALIGN(METADATA_SIZE), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		DIE(new == (void *)-1, "mmap");
		new->status = STATUS_MAPPED;
	}

	// legam la restul listei
	new->size = size;
	heap->prev = new;
	last->next = new;
	new->next = heap;
	new->prev = last;

	return new;
}

// functie eliminare din lista
void pop(struct block_meta *delete)
{
	if (delete->status == STATUS_ALLOC) {
		// schimbam statusul blocului
		delete->status = STATUS_FREE;
	} else if (delete->status == STATUS_MAPPED) {
		// eliminam delete din lista daca nu este singurul
		if (delete != delete->next) {
			if (delete == heap)
				heap = heap->next;

			delete->next->prev = delete->prev;
			delete->prev->next = delete->next;
		} else {
			heap = NULL;
		}
	}
}

// functia init adaptata pentru calloc
void init_calloc(size_t size)
{
	size_t page_size = getpagesize();

	if (size + ALIGN(METADATA_SIZE) < page_size) {
		// prealocam memorie(page size)
		heap = sbrk(MMAP_THRESHOLD);
		DIE(heap == (void *)-1, "sbrk");
		heap->status = STATUS_ALLOC;
		heap->size = size;

		// marcam ce este in afara ca memorie libera
		if (MMAP_THRESHOLD - size - ALIGN(METADATA_SIZE) > ALIGN(METADATA_SIZE)) {
			// exista memorie pe care sa o marcam
			heap->next = (struct block_meta *)((char *)heap + ALIGN(METADATA_SIZE) + ALIGN(size));
			heap->next->size = MMAP_THRESHOLD - ALIGN(size) - 2 * ALIGN(METADATA_SIZE);
			heap->next->status = STATUS_FREE;
			heap->next->next = heap;
			heap->next->prev = heap;
			heap->prev = heap->next;
		} else {
			// nu exista memorie pe care sa o marcam
			heap->next = heap;
			heap->prev = heap;
		}
	} else {
		// alocam cu mmpap
		heap = mmap(NULL, size + ALIGN(METADATA_SIZE), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		DIE(heap == (void *)-1, "mmap");
		heap->status = STATUS_MAPPED;
		heap->size = size;
		heap->next = heap;
		heap->prev = heap;
	}
}

// functie push adaptata pentru calloc
struct block_meta *push_calloc(size_t size)
{
	struct block_meta *new, *last = heap->prev;
	size_t page_size = getpagesize();

	// alocam memoria in functie de marime
	if (size < page_size) {
		new = sbrk(ALIGN(size) + ALIGN(METADATA_SIZE));
		DIE(new == (void *)-1, "sbrk");
		new->status = STATUS_ALLOC;
	} else {
		new = mmap(NULL, ALIGN(size) + ALIGN(METADATA_SIZE), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		DIE(new == (void *)-1, "mmap");
		new->status = STATUS_MAPPED;
	}

	// legam la restul listei
	new->size = size;
	heap->prev = new;
	last->next = new;
	new->next = heap;
	new->prev = last;

	return new;
}

// functie care ne spune daca adresa este alocata(de orice tip)
int in_list(void *ptr)
{
	struct block_meta *search = (struct block_meta *)((char *)ptr - ALIGN(METADATA_SIZE)), *iter;

	iter = heap;
	if (heap == NULL)
		return 0;

	do {
		if (search == iter)
			return 1;
		iter = iter->next;
	} while (iter != heap);

	return 0;
}
