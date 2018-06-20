#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "filesys/buffer_cache.h"
#include <stdio.h>
#include <string.h>
#include "threads/malloc.h"

struct buffer_head buffer_head[BUFFER_CACHE_ENTRY_NB];
void* p_buffer_cache;
static int clock_hand;

/* Initialization buffer cache */
void
bc_init(void)
{

  int i;
  void* p_data;  

  /* Allocation buffer cache in Memory */
  p_buffer_cache = malloc(BLOCK_SECTOR_SIZE*BUFFER_CACHE_ENTRY_NB);
  if(p_buffer_cache == NULL){
    printf("[%s] Memory Allocation Fail.\n",__FUNCTION__);
    return;
  }
  else{
    p_data = p_buffer_cache;
  }

  /* Initialization buffer head entries */
  for(i=0; i<BUFFER_CACHE_ENTRY_NB; i++){
    buffer_head[i].dirty = false;
    buffer_head[i].valid = false;
    buffer_head[i].sector = -1;
    buffer_head[i].clock_bit = 0;
    lock_init(&buffer_head[i].lock);
    buffer_head[i].data = p_data;

    p_data = p_data + BLOCK_SECTOR_SIZE;
  }

}

void
bc_term(void)
{
  /* Flush all buffer cache entry*/
  bc_flush_all_entries();

  free(p_buffer_cache);
}

bool
bc_read (block_sector_t sector_idx, void *buffer, off_t buffer_ofs, int chunk_size, int sector_ofs)
{
  bool success = false;
  struct buffer_head *p_lookup;
  
  p_lookup = bc_lookup(sector_idx);

  if(p_lookup == NULL)
  {
    p_lookup = bc_select_victim();
    if(p_lookup == NULL){
      return success;
    }
    lock_acquire(&p_lookup->lock);
    block_read(fs_device, sector_idx, p_lookup->data);

    /* update buffer head */
    p_lookup->dirty = false;
    p_lookup->valid = true;
    p_lookup->sector = sector_idx;
    lock_release(&p_lookup->lock);
  }
  lock_acquire(&p_lookup->lock);
 
  memcpy(buffer + buffer_ofs, p_lookup->data + sector_ofs, chunk_size);

  p_lookup->clock_bit = true;
  lock_release(&p_lookup->lock);

  success = true;

  return success;
}

bool
bc_write (block_sector_t sector_idx, void *buffer, off_t buffer_ofs, int chunk_size, int sector_ofs)
{
/*********************************************************/
/* bc_read()함수를 참고하여 write 기능 (구현)            */
/* 1. buffer cache에서 SECTOR_IDX를 검색                 */
/* 2. buffer cache에 존재하지 않으면 bc_select_victime() */
/*    함수를 통해 buffer cache entry에 디스크 읽기       */
/* 3. buffer cache에 BUFFER의 데이터를 기록              */
/* 4. buffer_head update                                 */
/*                                                       */
/* return : 성공 => ture, 실패 => false                  */
/*********************************************************/
  bool success = false;
/******************/
/*      start     */
/******************/
  struct buffer_head *p_lookup;
  
  p_lookup = bc_lookup(sector_idx);

  if(p_lookup == NULL)
  {
	p_lookup = bc_select_victim();
	if(p_lookup == NULL)
	{
	  return success;
	}
	block_read(fs_device, sector_idx, p_lookup->data);
  }
 
  lock_acquire(&p_lookup->lock);
  memcpy(p_lookup->data + sector_ofs, buffer + buffer_ofs, chunk_size);

  /* update buffer head */
  p_lookup->dirty = true;
  p_lookup->valid = true;
  p_lookup->sector = sector_idx;
  p_lookup->clock_bit = true;
  lock_release(&p_lookup->lock);

  success = true;
/******************/
/*      end     */
/******************/

  return success;
}
void
bc_flush_entry(struct buffer_head *p_flush_entry)
{
  lock_acquire(&p_flush_entry->lock);
  block_write(fs_device, p_flush_entry->sector, p_flush_entry->data);
  p_flush_entry->dirty = false;
  lock_release(&p_flush_entry->lock);
}

void
bc_flush_all_entries(void)
{
  int idx;

  for(idx = 0; idx < BUFFER_CACHE_ENTRY_NB; idx++){
	if(buffer_head[idx].dirty == true)
	{
          lock_acquire(&buffer_head[idx].lock);
	  block_write(fs_device, buffer_head[idx].sector, buffer_head[idx].data);
	  buffer_head[idx].dirty = false;
          lock_release(&buffer_head[idx].lock);
	}
  }
}


/* Lookup buffer head with given sector number*/
struct buffer_head*
bc_lookup(block_sector_t sector)
{
/********************************************************************/
/* buffer head를 순회하며 SECTOR의 entry를 검색하는 함수 (구현)     */
/* buffer_head[BUFFER_CACHE_ENTRY_NB] : buffer cache의 entry 테이블 */ 
/* return : 성공 => SECTOR의 buffer_head, 실패 => NULL              */
/********************************************************************/
/******************/
/*      start     */
/******************/
  int idx;

  for(idx = 0; idx < BUFFER_CACHE_ENTRY_NB; idx++){
    if(buffer_head[idx].sector == sector){
      return &buffer_head[idx];
    }
  }

  return NULL;
/******************/
/*      end     */
/******************/
}

/* Find victim buffer cache entry and evict it from cache to disk */
struct buffer_head*
bc_select_victim(void)
{
  int idx;
  
  /* Select victim using clock algorithm */
  while(1){
    idx = clock_hand;

    if(idx == BUFFER_CACHE_ENTRY_NB)
      idx = 0;

    if(++clock_hand == BUFFER_CACHE_ENTRY_NB)
      clock_hand = 0;

    if(buffer_head[idx].clock_bit){
      lock_acquire(&buffer_head[idx].lock);
      buffer_head[idx].clock_bit = 0;
      lock_release(&buffer_head[idx].lock);
    }
    else{
      lock_acquire(&buffer_head[idx].lock);
      buffer_head[idx].clock_bit = 1;
      lock_release(&buffer_head[idx].lock);
      break;
    }
  }

  /* If the victim entry is dirty, write to disk */
  if(buffer_head[idx].dirty == true){
    bc_flush_entry(&buffer_head[idx]);
  }

  /* Update buffer head entry */
  lock_acquire(&buffer_head[idx].lock);
  buffer_head[idx].dirty = false;
  buffer_head[idx].valid = false;
  buffer_head[idx].sector = -1;
  lock_release(&buffer_head[idx].lock);

  return &buffer_head[idx];
}
