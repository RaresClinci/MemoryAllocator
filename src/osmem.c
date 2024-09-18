// SPDX-License-Identifier: BSD-3-Clause

#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include "osmem.h"
#include "block_meta.h"
#include "mem_list.h"

void *os_malloc(size_t size)
{
	/* TODO: Implement os_malloc */

	// verificam daca se doresc 0 bytes alocati
	if (size == 0)
		return NULL;

	// verificam daca lista a fost initializata
	if (heap == NULL) {
		init(size);
		return (char *)heap + ALIGN(METADATA_SIZE);
	}
	// cautam blocul perfect
	struct block_meta *place = find_best(size);

	if (place == NULL) {
		if (size + ALIGN(METADATA_SIZE) < MMAP_THRESHOLD) {
			// putem expanda ultimul bloc
			struct block_meta *last = expand_last(size);

			if (last != NULL)
				return (char *)last + ALIGN(METADATA_SIZE);
		}

		// cream un bloc nou
		return (char *)push(size) + ALIGN(METADATA_SIZE);
	}

	split_block(place, size);
	return (char *)place + ALIGN(METADATA_SIZE);
}

void os_free(void *ptr)
{
	/* TODO: Implement os_free */
	if (ptr == NULL)
		return;

	// verificam daca blocul este in lista
	if (in_list(ptr) == 0)
		return;

	// aflam valoarea pointerului block_meta
	struct block_meta *delete = ptr - ALIGN(METADATA_SIZE);

	// eliminam celula din lista
	pop(delete);

	// dealocam memoria, daca este necesar
	if (delete->status == STATUS_MAPPED) {
		int ret = munmap(delete, ALIGN(delete->size) + ALIGN(METADATA_SIZE));

		DIE(ret == -1, "munmap");
	}

	coalesce();
}

void *os_calloc(size_t nmemb, size_t size)
{
	/* TODO: Implement os_calloc */
	size_t true_size = nmemb * size;
	void *ptr = NULL;

	// verificam daca se doresc 0 bytes alocati
	if (true_size == 0)
		return NULL;

	// verificam daca lista a fost initializata
	if (heap == NULL) {
		init_calloc(true_size);
		ptr = (char *)heap + ALIGN(METADATA_SIZE);
	} else {
		// cautam blocul perfect
		struct block_meta *place = find_best(true_size);

		if (place == NULL || size + ALIGN(METADATA_SIZE) >= (unsigned long)getpagesize()) {
			if (heap->prev->status == STATUS_FREE && size + ALIGN(METADATA_SIZE) < (unsigned long)getpagesize()) {
				// putem expanda ultimul bloc
				ptr = (char *)expand_last(true_size) + ALIGN(METADATA_SIZE);
			} else {
				// cream un bloc nou
				ptr = (char *)push_calloc(true_size) + ALIGN(METADATA_SIZE);
			}
		} else {
			split_block(place, true_size);
			ptr = (char *)place + ALIGN(METADATA_SIZE);
		}
	}

	// umplem zona alocata cu 0
	memset(ptr, 0, nmemb * size);
	return ptr;
}

void *os_realloc(void *ptr, size_t size)
{
	/* TODO: Implement os_realloc */
	if (ptr != NULL && in_list(ptr) == 0) {
		// adresa nu a fost alocata anterior
		return NULL;
	}
	if (ptr == NULL) {
		return os_malloc(size);
	} else if (size == 0) {
		os_free(ptr);
	} else {
		// blocul de memorie vechi
		struct block_meta *old = (struct block_meta *)((char *)ptr - ALIGN(METADATA_SIZE)), *right;

		if (old->status == STATUS_FREE)
			return NULL;

		if (size < MMAP_THRESHOLD) {
			// memoria va fi STATUS_ALLOC
			if (old->status == STATUS_ALLOC) {
				if (old->size > size) {
					// Caz 1: trunchem memoria actuala
					split_block(old, size);
					return (char *)old + ALIGN(METADATA_SIZE);
				}

				// cautam cel mai apropiat bloc alocat cu sbrk
				right = old->next;
				while (right != heap) {
					if (right->status != STATUS_MAPPED) {
						// blocul este liber sau alocat(nu mapat)
						break;
					}
					right = right->next;
				}

				if (right == heap) {
					right = sbrk(0);
					DIE(right == (void *)-1, "sbrk");
				}

				if (right != heap && (char *)right - (char *)old - ALIGN(METADATA_SIZE) >= ALIGN(size)) {
					// Caz 2: memoria incape in zona originala
					old->size = size;
					return (char *)old + ALIGN(METADATA_SIZE);
				}

				if (right->size != 0 && right->status == STATUS_FREE &&
						(char *)right - (char *)old + ALIGN(right->size) >= ALIGN(size)) {
					// Caz 3: verificam daca dupa este un bloc liber destul de mare
					// impartim blocul drept intr-unul de marimea necesara si restul
					size_t req_size = size - ((char *)right - (char *)old - ALIGN(METADATA_SIZE));

					if (req_size >= ALIGN(METADATA_SIZE)) {
						split_block(right, req_size - ALIGN(METADATA_SIZE));

						// unim blocul de realocat cu cel nou
						old->status = STATUS_ALLOC;
						merge(old, right);
						old->size = size;
						return (char *)old + ALIGN(METADATA_SIZE);
					}

					// crestem old cu metadata_size
					right->prev->next++;
					right->next->prev++;
					memcpy(right + 1, right, right->size);
					right++;
					right->size -= ALIGN(METADATA_SIZE);
					old->size = size;
					return (char *)old + ALIGN(METADATA_SIZE);
				}

				// cautam ultimul bloc free(daca exista)
				struct block_meta *last = NULL, *iter = heap;

				do {
					if (iter->status != STATUS_MAPPED)
						last = iter;
					iter = iter->next;
				} while (iter != heap);

				if (old == last) {
					// Caz 4: blocul vechi este ultimul si putem sa il expandam
					old->status = STATUS_FREE;
					expand_last(size);
					return (char *)old + ALIGN(METADATA_SIZE);
				}

				// Caz 5: realocare normala
				// cautam size-ul minim
				size_t min_size = old->size;

				if (min_size > size)
					min_size = size;

				// realocam propriu-zis
				pop(old);
				if (old->status == STATUS_FREE)
					// prevenim o coalescenta prematura
					old->status = STATUS_ALLOC;

				void *new = os_malloc(size);

				memcpy(new, ptr, min_size);
				old->status = STATUS_FREE;
				return new;
			}

			// blocul vechi este mapped => realocare normala
			// cautam size-ul minim
			size_t min_size = old->size;

			if (min_size > size)
				min_size = size;

			// realocam propriu-zis
			pop(old);
			void *new = os_malloc(size);

			memcpy(new, ptr, min_size);
			int ret = munmap(old, ALIGN(old->size) + ALIGN(METADATA_SIZE));

			DIE(ret == -1, "munmap");
			return new;
		}

		// blocul nou este de tip STATUS_MAPPED => realocare normala
		// cautam size-ul minim
		size_t min_size = old->size;
		int old_status = old->status;

		if (min_size > size)
			min_size = size;

		// realocam propriu-zis
		pop(old);
		if (old->status == STATUS_FREE)
			// prevenim o coalescenta prematura
			old->status = STATUS_ALLOC;

		void *new = os_malloc(size);

		memcpy(new, ptr, min_size);
		if (old_status == STATUS_MAPPED) {
			int ret = munmap(old, ALIGN(old->size) + ALIGN(METADATA_SIZE));

			DIE(ret == -1, "munmap");
		} else {
			old->status = STATUS_FREE;
		}

		return new;
	}

	// aplicam o coalescenta
	coalesce();

	return NULL;
}
