/*
 * mem.h - memory management
 */

#pragma once

#include <base/types.h>

enum {
	PGSHIFT_4KB = 12,
	PGSHIFT_2MB = 21,
	PGSHIFT_1GB = 30,
};

enum {
	PGSIZE_4KB = (1 << PGSHIFT_4KB), /* 4096 bytes */
	PGSIZE_2MB = (1 << PGSHIFT_2MB), /* 2097152 bytes */
	PGSIZE_1GB = (1 << PGSHIFT_1GB), /* 1073741824 bytes */
};

#define PGMASK_4KB	(PGSIZE_4KB - 1)
#define PGMASK_2MB	(PGSIZE_2MB - 1)
#define PGMASK_1GB	(PGSIZE_1GB - 1)

/* page numbers */
#define PGN_4KB(la)	(((uintptr_t)(la)) >> PGSHIFT_4KB)
#define PGN_2MB(la)	(((uintptr_t)(la)) >> PGSHIFT_2MB)
#define PGN_1GB(la)	(((uintptr_t)(la)) >> PGSHIFT_1GB)

#define PGOFF_4KB(la)	(((uintptr_t)(la)) & PGMASK_4KB)
#define PGOFF_2MB(la)	(((uintptr_t)(la)) & PGMASK_2MB)
#define PGOFF_1GB(la)	(((uintptr_t)(la)) & PGMASK_1GB)

#define PGADDR_4KB(la)	(((uintptr_t)(la)) & ~((uintptr_t)PGMASK_4KB))
#define PGADDR_2MB(la)	(((uintptr_t)(la)) & ~((uintptr_t)PGMASK_2MB))
#define PGADDR_1GB(la)	(((uintptr_t)(la)) & ~((uintptr_t)PGMASK_1GB))

typedef unsigned long physaddr_t; /* physical addresses */
typedef unsigned long virtaddr_t; /* virtual addresses */

#ifndef MAP_FAILED
#define MAP_FAILED	((void *)-1)
#endif

extern void *mem_map_anom(void *base, int nr, int size, int node);
extern void *mem_map_file(void *base, size_t len, int fd, off_t offset);
extern int mem_lookup_page_phys_addrs(void *addr, int nr, int size,
				      physaddr_t *maddrs);

static inline int
mem_lookup_page_phys_addr(void *addr, int size, physaddr_t *maddr)
{
	return mem_lookup_page_phys_addrs(addr, 1, size, maddr);
}