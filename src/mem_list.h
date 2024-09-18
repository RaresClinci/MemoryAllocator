/* SPDX-License-Identifier: BSD-3-Clause */

#pragma once

#include <errno.h>
#include <stdio.h>
#include "printf.h"
#include "block_meta.h"

// definim macro pentru alignment
#define ALIGNMENT 8
#define ALIGN(size)  (((size) + (ALIGNMENT-1)) & ~(ALIGNMENT-1))
// macrouri luate din test-utils.h
#define METADATA_SIZE		(sizeof(struct block_meta))
#define MOCK_PREALLOC		(128 * 1024 - METADATA_SIZE - 8)
#define MMAP_THRESHOLD		(128 * 1024)
#define NUM_SZ_SM		11
#define NUM_SZ_MD		6
#define NUM_SZ_LG		4
#define MULT_KB			1024

// variabila globala ce reprezinta heapul
struct block_meta *heap;

// functii de prelucrare lista memorie
void init(size_t size);
void merge(struct block_meta* mem1, struct block_meta* mem2);
void coalesce(void);
struct block_meta* find_best(size_t size);
void split_block(struct block_meta* mem, size_t size);
struct block_meta* push(size_t size);
void pop(struct block_meta *delete);
struct block_meta* expand_last(size_t new_size);
int in_list(void *ptr);

// functii specifice calloc
void init_calloc(size_t size);
struct block_meta* push_calloc(size_t size);
