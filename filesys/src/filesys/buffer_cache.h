#ifndef FILESYS_BUFFER_CACHE_H
#define FILESYS_BUFFER_CACHE_H

#include "threads/synch.h"


/* buffer cache entry number */
#define BUFFER_CACHE_ENTRY_NB 64

/* buffer cache entry */
struct buffer_head
{
  bool dirty;	
  bool valid;			/* buffer cache entry의 사용 여부 */
  block_sector_t sector;	/* buffer cache에 저장된 disk sector */
  bool clock_bit;		/* clock algorithm을 위한 필드 */
  struct lock lock;
  void *data;			/* buffer cache의 데이터 포인트 */
};

void bc_init(void);
void bc_term(void);
bool bc_read(block_sector_t sector_idx, void *buffer, off_t buffer_ofs, int chunk_size, int sector_ofs);
bool bc_write (block_sector_t sector_idx, void *buffer, off_t buffer_ofs, int chunk_size, int sector_ofs);
struct buffer_head* bc_lookup(block_sector_t sector);
struct buffer_head* bc_select_victim(void);

void bc_flush_entry(struct buffer_head*);
void bc_flush_all_entries(void);

#endif
