#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <stdio.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/buffer_cache.h"
#include "filesys/directory.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define INDIRECT_BLOCK_ENTRIES (BLOCK_SECTOR_SIZE / sizeof (block_sector_t))
#define DIRECT_BLOCK_ENTRIES 123

int I_BLOCK_ENTRY_NB;

enum direct_t
{
  NORMAL_DIRECT,
  INDIRECT,
  DOUBLE_INDIRECT,
  OUT_LIMIT
};

struct sector_location{
  enum direct_t directness;
  int index1;
  int index2;
};

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t direct_map_table[DIRECT_BLOCK_ENTRIES];
    block_sector_t indirect_block_sec;
    block_sector_t double_indirect_block_sec;
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    uint32_t is_dir;
  };

/* indirect inode. */
struct inode_indirect_block
{
    block_sector_t map_table [INDIRECT_BLOCK_ENTRIES];
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

static inline off_t
map_table_offset (int index)
{
  struct inode_indirect_block id;
  off_t base = (uint32_t) ((uint8_t *) &(id.map_table) - (uint8_t *) &(id));

  return base + index * sizeof (block_sector_t);
}


/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct lock extend_lock;
  };

static bool get_disk_inode(const struct inode* inode, struct inode_disk *inode_disk);
static void locate_byte (off_t pos, struct sector_location *sec_loc);
static bool register_sector (struct inode_disk *inode_disk, block_sector_t new_sector, struct sector_location sec_loc);
static block_sector_t byte_to_sector (const struct inode_disk *inode_disk, off_t pos);
bool inode_update_file_length(struct inode_disk *, off_t, off_t);
static void free_inode_sectors (struct inode_disk *inode_disk);

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode_disk *inode_disk, off_t pos) 
{
  block_sector_t result_sec;
  ASSERT (inode_disk != NULL);
  if (pos < inode_disk->length)
  {
    struct inode_indirect_block *ind_block;
    struct sector_location sec_loc;
    locate_byte (pos, &sec_loc);
    switch (sec_loc.directness)
    {
      case NORMAL_DIRECT:
        result_sec = inode_disk->direct_map_table[sec_loc.index1];
        break;

      case INDIRECT:
        ASSERT (inode_disk->indirect_block_sec);
        ind_block = (struct inode_indirect_block *) malloc (BLOCK_SECTOR_SIZE);
        if (ind_block)
        {
          bc_read(inode_disk->indirect_block_sec, ind_block, 0, BLOCK_SECTOR_SIZE, 0);
          result_sec = ind_block->map_table[sec_loc.index1];
        }
        else
          result_sec = 0;
        free (ind_block);
        break;

      case DOUBLE_INDIRECT:
        ASSERT (inode_disk->indirect_block_sec);
        ind_block = (struct inode_indirect_block *) malloc (BLOCK_SECTOR_SIZE);
        if (ind_block)
        {
          /* read in the first level indirect block */
          bc_read(inode_disk->double_indirect_block_sec, ind_block, 0, BLOCK_SECTOR_SIZE, 0);
          ASSERT (ind_block->map_table[sec_loc.index1]);
          /* read in the second level indirect block */
          bc_read(ind_block->map_table[sec_loc.index1], ind_block, 0, BLOCK_SECTOR_SIZE, 0);
          result_sec = ind_block->map_table[sec_loc.index2];
        }
        else
          result_sec = 0;
        free (ind_block);
        break;

      default:
        result_sec = 0;
    }
  }
  else
    result_sec = 0;      // pos > inode length

  return result_sec;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);

  I_BLOCK_ENTRY_NB = BLOCK_SECTOR_SIZE / sizeof(block_sector_t);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, uint32_t is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_dir = is_dir;

      if(length > 0)
        if(!inode_update_file_length(disk_inode, 0, length-1))
          return false;
      
      bc_write(sector, disk_inode, 0, BLOCK_SECTOR_SIZE, 0);
      free(disk_inode);
      success = true;
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;

  lock_init(&inode->extend_lock);

  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
        struct inode_disk *disk_inode = malloc (sizeof (struct inode_disk));
        if (disk_inode == NULL)
          PANIC ("Failed to close inode. Could not allocate disk_inode.\n");
        get_disk_inode (inode, disk_inode);
        free_inode_sectors (disk_inode);
        free_map_release (inode->sector, 1);
        free (disk_inode);
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  struct inode_disk *disk_inode = malloc(sizeof(struct inode_disk));
  if(disk_inode == NULL)
    return 0;
  get_disk_inode(inode, disk_inode);

  while (size > 0) 
    {
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = disk_inode->length - offset;

      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (disk_inode, offset);
      if(sector_idx == 0){
        break;
      }

      bc_read(sector_idx, buffer, bytes_read, chunk_size, sector_ofs);
 
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  free(disk_inode);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  struct inode_disk *disk_inode = malloc(sizeof(struct inode_disk));
  if(disk_inode == NULL)
    return 0;
  get_disk_inode(inode, disk_inode);

  if(inode->deny_write_cnt){
    free(disk_inode);
    return 0;
  }

  lock_acquire(&inode->extend_lock);
  int old_length = disk_inode->length;
  int write_end = offset +  size - 1;
  if(write_end > old_length -1 ){
    disk_inode->length = write_end + 1;
    if(!inode_update_file_length(disk_inode, old_length, write_end)){
      free(disk_inode);
      return 0;
    }
  }
  lock_release(&inode->extend_lock);

  while (size > 0) 
    {
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = disk_inode->length - offset;

      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
      
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (disk_inode, offset);
      if(sector_idx == 0)
        break;

      bc_write(sector_idx, (void*)buffer, bytes_written, chunk_size, sector_ofs);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  bc_write(inode->sector, disk_inode, 0, BLOCK_SECTOR_SIZE, 0);
  free(disk_inode);
 
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  off_t result;
  struct inode_disk *disk_inode = malloc (sizeof (struct inode_disk));
  if (disk_inode == NULL)
    return 0;
  get_disk_inode (inode, disk_inode);
  result = disk_inode->length;
  free (disk_inode);
  return result;
}

bool inode_update_file_length(struct inode_disk* inode_disk, off_t start_pos, off_t end_pos)
{
  ASSERT(start_pos <= end_pos);

  block_sector_t sector_idx; 
  off_t offset = start_pos;
  int size = end_pos - start_pos + 1;
  uint8_t *zeroes = calloc(sizeof(uint8_t), BLOCK_SECTOR_SIZE);
  if(zeroes == NULL)
    return false;

  while(size > 0)
  {
    /* Sector to write, starting byte offset within sector. */
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Number of bytes to actually write into this sector. */
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int chunk_size = size < sector_left ? size : sector_left;
    if (chunk_size <= 0)
      break;

    if(sector_ofs > 0){
      sector_idx = byte_to_sector(inode_disk, offset);
      ASSERT(sector_idx != 0);

      bc_write(sector_idx, zeroes, 0, sector_left, sector_ofs);
    }
    else{
      if(free_map_allocate(1, &sector_idx)){
        struct sector_location cur_loc;
        locate_byte(offset, &cur_loc);
        if(!register_sector(inode_disk, sector_idx, cur_loc))
        {
          free_map_release(sector_idx, 1);
          free(zeroes);
          return false;
        }
      }
      else{
        free(zeroes);
        return false;
      }
      bc_write(sector_idx, zeroes, 0, BLOCK_SECTOR_SIZE, 0);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
  }
  free(zeroes);


  return true;
}

static void
free_inode_sectors (struct inode_disk *inode_disk)
{
  int i, j;

  /* Release double indirect blocks */
  if (inode_disk->double_indirect_block_sec > 0)
  {
    /* Temporary holder for the indirect blocks */
    struct inode_indirect_block *ind_block_1 = malloc (BLOCK_SECTOR_SIZE);
    struct inode_indirect_block *ind_block_2 = malloc (BLOCK_SECTOR_SIZE);
    if (ind_block_1 == NULL || ind_block_2 == NULL)
    {
      printf ("Cannot allocate sectors to hold ind-blocks. Free sectors failed!\n");
      return;
    }
    /* Read in 1st level block */
    bc_read (inode_disk->double_indirect_block_sec, ind_block_1, 0, BLOCK_SECTOR_SIZE, 0);
    i = 0;        /* index to the 1st level block */
    while (ind_block_1->map_table[i] > 0)
    {
      /* Read in the 2nd level block */
      bc_read (ind_block_1->map_table[i], ind_block_2, 0, BLOCK_SECTOR_SIZE, 0);
      j = 0;
      while (ind_block_2->map_table[j] > 0)
      {
        free_map_release (ind_block_2->map_table[j], 1);
        j++;
      }
      free_map_release (ind_block_1->map_table[i], 1);
      i++;
    }
    free_map_release (inode_disk->double_indirect_block_sec, 1);
    free (ind_block_1);
    free (ind_block_2);
  }
  /* Release indirect blocks */
  if (inode_disk->indirect_block_sec > 0)
  {
    /* Temporary holder for the indirect block */
    struct inode_indirect_block *ind_block = malloc (BLOCK_SECTOR_SIZE);
    if (ind_block == NULL)
    {
      printf ("Cannot allocate a sector to hold ind-blocks. Free sectors failed!\n");
      return;
    }
    /* Read in the indirect block */
    bc_read (inode_disk->double_indirect_block_sec, ind_block, 0, BLOCK_SECTOR_SIZE, 0);
    i = 0;        /* index to the indirect block */
    while (ind_block->map_table[i] > 0)
    {
      free_map_release (ind_block->map_table[i], 1);
      i++;
    }
    free_map_release (inode_disk->indirect_block_sec, 1);
    free (ind_block);
  }

  /* Release the direct blocks */
  i = 0;
  while (inode_disk->direct_map_table[i] > 0)
  {
    free_map_release (inode_disk->direct_map_table[i], 1);
    i++;
  }
}

bool inode_is_dir(const struct inode *inode)
{
  bool result;
  struct inode_disk *disk_inode = malloc(sizeof(struct inode_disk));
  if(disk_inode == NULL)
    return 0;

  bc_read(inode->sector, (void *)disk_inode, 0, BLOCK_SECTOR_SIZE, 0);

  result = disk_inode->is_dir;

  free(disk_inode);

  return result;
}

bool inode_is_removed(const struct inode *inode)
{
  return inode->removed;
}

static bool
get_disk_inode(const struct inode *inode, struct inode_disk *inode_disk)
{
  bc_read(inode->sector, inode_disk, 0, BLOCK_SECTOR_SIZE, 0);
  return true;
}

/* Calculate where in the inode structure we can find byte# "pos".
   This is a pure maths function, so no "inode" input argument is needed.
   Store result into structure pointed by *sec_loc */
static void
locate_byte (off_t pos, struct sector_location *sec_loc)
{
  off_t pos_sector = pos / BLOCK_SECTOR_SIZE;
  if (pos_sector < DIRECT_BLOCK_ENTRIES)
  {
    sec_loc->directness = NORMAL_DIRECT;
    sec_loc->index1 = pos_sector;
  }
  else if (pos_sector < (off_t) (DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES))
  {
    /***********************************************************************/
    /* sec_loc 자료구조의 변수 값 업데이트 (구현)                          */
    /***********************************************************************/
    /******************/
    /*      start     */
    /******************/
    sec_loc->directness = INDIRECT;
    sec_loc->index1 = pos_sector - DIRECT_BLOCK_ENTRIES;
    /******************/
    /*      end       */
    /******************/
  }
  else if (pos_sector < (off_t) (DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES * (INDIRECT_BLOCK_ENTRIES + 1)))
  {
    /***********************************************************************/
    /* sec_loc 자료구조의 변수 값 업데이트 (구현)                          */
    /***********************************************************************/
    /******************/
    /*      start     */
    /******************/
    sec_loc->directness = DOUBLE_INDIRECT;
    pos_sector = pos_sector - DIRECT_BLOCK_ENTRIES - INDIRECT_BLOCK_ENTRIES;
    sec_loc->index1 = pos_sector / INDIRECT_BLOCK_ENTRIES;
    sec_loc->index2 = pos_sector % INDIRECT_BLOCK_ENTRIES;
    /******************/
    /*      end     */
    /******************/
  }
  else
    sec_loc->directness = OUT_LIMIT;
}

/* Register a new sector to inode */
static bool
register_sector (struct inode_disk *inode_disk, block_sector_t new_sector, struct sector_location sec_loc)
{
  ASSERT (inode_disk != NULL);

  struct inode_indirect_block *new_block = NULL;
  block_sector_t ind_block_2nd_level;

  switch (sec_loc.directness)
  {
    case NORMAL_DIRECT:
      inode_disk->direct_map_table[sec_loc.index1] = new_sector;
      break;

    case INDIRECT:
      new_block = malloc (BLOCK_SECTOR_SIZE);
      if (new_block == NULL)
        return false;

      /* Current ind_block unavailable */
      if (inode_disk->indirect_block_sec == 0)
      {
        /***********************************************************************/
        /* 인덱스 블록에 새로 할당 받은 블록 번호 저장(구현)                   */
        /***********************************************************************/
	/******************/
	/*      start     */
	/******************/
        if (free_map_allocate (1, &(inode_disk->indirect_block_sec)))
        {
          memset (new_block, 0, BLOCK_SECTOR_SIZE);
          new_block->map_table[sec_loc.index1] = new_sector;
          bc_write(inode_disk->indirect_block_sec, new_block, 0, BLOCK_SECTOR_SIZE, 0);
        }
        else
        {
          free (new_block);
          return false;
        }
	/******************/
	/*      end       */
	/******************/
      }
      else    /* indirect block sector already exists */
      {
        bc_write(inode_disk->indirect_block_sec, &new_sector, 0, sizeof(block_sector_t), map_table_offset(sec_loc.index1));
      }
      break;


    case DOUBLE_INDIRECT:
      new_block = malloc (BLOCK_SECTOR_SIZE);
      if (new_block == NULL)
        return false;

      /***********************************************************************/
      /* 2차 인덱스 블록에 새로 할당 받은 블록 주소 저장후,                  */
      /* 각 인덱스 블록을 buffer cache에 기록(구현)                          */
      /***********************************************************************/
      /******************/
      /*      start     */
      /******************/
      /* Create the 1st level of double_direct_block if it does not exist */
      /* Current double_ind_block unavailable */
      if (inode_disk->double_indirect_block_sec == 0)
      {
        if (free_map_allocate (1, &(inode_disk->double_indirect_block_sec)))
        {
          memset (new_block, 0, BLOCK_SECTOR_SIZE);

          bc_write(inode_disk->double_indirect_block_sec, new_block, 0, BLOCK_SECTOR_SIZE, 0);
        }
        else
        {
          free (new_block);
          return false;
        }
      }

      /* Read in ind_block_2nd_level */
      bc_read(inode_disk->double_indirect_block_sec, &ind_block_2nd_level, 0, sizeof(block_sector_t), map_table_offset(sec_loc.index1));

      /* 2nd level indirect block does not exist */
      if (ind_block_2nd_level == 0)
      {
        if (free_map_allocate (1, &ind_block_2nd_level))
        {
          /* Update 1st level */
          bc_write(inode_disk->double_indirect_block_sec, &ind_block_2nd_level, 0, sizeof(block_sector_t), map_table_offset(sec_loc.index1));
          memset (new_block, 0, BLOCK_SECTOR_SIZE);
          new_block->map_table[sec_loc.index2] = new_sector;

          bc_write(ind_block_2nd_level, new_block, 0, BLOCK_SECTOR_SIZE, 0);
        }
        else
        {
          free (new_block);
          return false;
        }
      }
      else
      {
        /* Write new entry into 2nd level block. */
        bc_write(ind_block_2nd_level, &new_sector, 0, sizeof(block_sector_t), map_table_offset(sec_loc.index2));
      }
      /******************/
      /*      end       */
      /******************/
      break;

    default:
      return false;
  }

  free (new_block);
  return true;
}











