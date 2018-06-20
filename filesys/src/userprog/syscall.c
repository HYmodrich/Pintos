#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <list.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/directory.h"
#include "filesys/inode.h"


static void syscall_handler (struct intr_frame *);
void get_argument(void *esp , int *arg, int count);
void check_address(void *addr);
void check_buffer(void *buffer, unsigned size, void *esp, bool to_write);
void check_string(const void *str, void *esp);

void halt(void);
void exit(int status);
tid_t exec(const char *cmd_line);
int wait (tid_t tid);
bool create(const char *file, unsigned initial_size);
bool remove(const char *file);
int open(const char *file);
int filesize(int fd);
int read(int fd, void *buffer, unsigned size);
int write(int fd, const void *buffer, unsigned size);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd);

bool sys_chdir(const char *dir);
bool sys_mkdir(const char *dir);
bool sys_readdir(int fd, char *name);
bool sys_isdir(int fd);
int sys_inumber(int fd);


void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&filesys_lock);
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  int arg[3];
  uint32_t *sp = f -> esp;
  check_address(sp);
  int syscall_n = *sp;

  switch(syscall_n)
  {
	case SYS_HALT : 
	halt();
	break;

	case SYS_EXIT :
	get_argument(sp , arg , 1);
	exit(arg[0]);
	f -> eax = arg[0];
	break;

	case SYS_EXEC :
	get_argument(sp , arg , 1);
	check_address((void *)arg[0]);
	f -> eax = exec((const char *)arg[0]);
	break;

	case SYS_WAIT :
	get_argument(sp , arg , 1);
	f -> eax = wait(arg[0]);
	break;

	case SYS_CREATE :
	get_argument(sp , arg , 2);
	check_address(arg[0]);
	f -> eax = create((const char *)arg[0] , (unsigned)arg[1]);	
	break;

	case SYS_REMOVE :
	get_argument(sp , arg , 1);
	check_address(arg[0]);
	f -> eax = remove((const char *)arg[0]);
	break;

	case SYS_OPEN :
	get_argument(sp, arg , 1);
	check_address(arg[0]);
	f -> eax = open((const char *)arg[0]);
	break;
	
	case SYS_FILESIZE :
	get_argument(sp, arg , 1);
	f -> eax = filesize(arg[0]);
	break;

	case SYS_READ :
	get_argument(sp, arg , 3);
	check_address(arg[1]);
	f -> eax = read(arg[0] , (void *)arg[1] , (unsigned)arg[2]);
	break;

	case SYS_WRITE :
	get_argument(sp, arg , 3);
	check_address(arg[1]);
	f -> eax = write(arg[0] , (const void *)arg[1] , (unsigned)arg[2]);
	break;

	case SYS_SEEK :
	get_argument(sp , arg , 2);
	seek(arg[0] , (unsigned) arg[1]);
	break;

	case SYS_TELL :
	get_argument(sp , arg , 1);
	f -> eax = tell(arg[0]);
	break;

	case SYS_CLOSE :
	get_argument(sp , arg , 1);
	close(arg[0]);	
	break;

        case SYS_CHDIR:
        get_argument(sp , arg , 1);
	check_address(arg[0]);
        f -> eax = sys_chdir((const char *)arg[0]);
        break;

        case SYS_MKDIR:
        get_argument(sp , arg , 1);
	check_address(arg[0]);
        f -> eax = sys_mkdir((const char *)arg[0]);
        break;

        case SYS_READDIR:
        get_argument(sp , arg , 2);
	check_address(arg[1]);
        f -> eax = sys_readdir(arg[0], (char *)arg[1]);
        break;

        case SYS_ISDIR:
        get_argument(sp , arg , 1);
        f -> eax = sys_isdir(arg[0]);
        break;

        case SYS_INUMBER:
        get_argument(sp , arg , 1);
        f -> eax = sys_inumber(arg[0]);
        break;

	default :
	printf("not\n");
  	printf ("system call!\n");
	thread_exit ();
	}	

}

void get_argument(void *esp, int *arg , int count)
{
	int i;
	int *ptr;

	for(i = 0 ; i < count ; i++)
	{
		ptr = (int *)esp + i + 1;
		check_address(ptr);
		arg[i] = *ptr;
	}
}

void check_address(void *addr)
{
	if(addr < (void *)0x08048000 || addr >= (void *)0xc0000000)
	{	 		
		exit(-1);
	}
}


void halt(void)
{
	printf("system halt\n");	
	shutdown_power_off();
}

void exit(int status)
{
	struct thread *cur = thread_current();   	
	cur -> cp -> status = status;
	printf("%s: exit(%d)\n" , cur -> name , status);
	thread_exit();

}

tid_t exec(const char *cmd_line)
{
	tid_t tid = process_execute(cmd_line);
	struct child_process *cp = get_child_process(tid);
	if(!cp)
	{
		return -1;
	}

	if(cp -> load == NO_LOAD)
	{
		sema_down(&cp -> sema_load);
	}

	if(cp -> load == FAIL_LOAD)
	{
		return -1;
	}
		
	return tid;	
}

int wait(tid_t tid)
{
	return process_wait(tid);
}

bool create(const char *file , unsigned initial_size)
{
	lock_acquire(&filesys_lock);
	bool success = filesys_create(file , initial_size);
	lock_release(&filesys_lock);
	return success;
}

bool remove(const char *file)
{
	lock_acquire(&filesys_lock);
	bool success = filesys_remove(file);
	lock_release(&filesys_lock);
	return success;
}


int open(const char *file)
{
	lock_acquire(&filesys_lock);
	struct file *f = filesys_open(file);
	if(!f)
	{
	        lock_release(&filesys_lock);
		return -1;
	}
	int fd = process_add_file(f);
	lock_release(&filesys_lock);
	return fd;
}

int filesize (int fd)
{
	lock_acquire(&filesys_lock);
	struct file *f = process_get_file(fd);
	if(!f)
	{
		lock_release(&filesys_lock);
		return -1;
	}
	int size = file_length(f);
	lock_release(&filesys_lock);
	return size;
}

int read (int fd, void *buffer, unsigned size)
{
	unsigned i;
        if (fd == STDOUT_FILENO)
                return -1; 
	if (fd == STDIN_FILENO)
	{
		uint8_t *local_buffer = (uint8_t *) buffer;
		for(i = 0; i < size ; i++)
		{
			local_buffer[i] = input_getc();
		}
	return size;
	}

	lock_acquire(&filesys_lock);
	struct file *f = process_get_file(fd);

	if(!f)
	{
		lock_release(&filesys_lock);
		return -1;
	}
	lock_release(&filesys_lock);

	int bytes = file_read(f, buffer, size);
	return bytes;
}

int write(int fd, const void *buffer, unsigned size)
{
        if (fd == STDIN_FILENO)
                return -1;
	if(fd == STDOUT_FILENO)
	{
		putbuf((const char *)buffer , size);
		return size;	
	}

	lock_acquire(&filesys_lock);
	struct file *f = process_get_file(fd);
	if(!f)
	{
		lock_release(&filesys_lock);
		return -1;
	}
        if(inode_is_dir(file_get_inode(f)))
        {
                lock_release(&filesys_lock);
                return -1;
        }
	int bytes = file_write(f, buffer, size);
	lock_release(&filesys_lock);
	return bytes;
}

void seek (int fd, unsigned position)
{
	lock_acquire(&filesys_lock);
	struct file *f = process_get_file(fd);
	if(!f)
	{
		lock_release(&filesys_lock);
		return;
	}
	file_seek(f, position);
	lock_release(&filesys_lock);
}

unsigned tell (int fd)
{
	lock_acquire(&filesys_lock);
	struct file *f = process_get_file(fd);
	if(!f)
	{
		lock_release(&filesys_lock);
		return -1;
	}
	off_t offset = file_tell(f);
	lock_release(&filesys_lock);
	return offset;
}

void close (int fd)
{
	lock_acquire(&filesys_lock);
	process_close_file(fd);
	lock_release(&filesys_lock);
}

bool sys_chdir(const char *dir)
{
  /***********************************************************************/
  /* chdir 시스템 콜 (구현)                                              */
  /* 프로세스의 현재 작업 디렉터리를 DIR 경로로 변경(절대/상대경)        */
  /* 1. DIR 경로의 디렉터리를 open                                       */
  /* 2. thread 자료구조의 cur_dir을 dir 디렉터리로 설정                  */
  /* return : 성공 true, 실패 false                                      */
  /***********************************************************************/
  /******************/
  /*      start     */
  /******************/
  struct file *p_file = filesys_open(dir);
  if(p_file == NULL)
    return false;

  struct inode *p_inode = inode_reopen(file_get_inode(p_file));
  struct dir *p_cur_dir = dir_open(p_inode);

  file_close(p_file);
  if(p_cur_dir == NULL)
    return false;

  dir_close(thread_current()->cur_dir);
  thread_current()->cur_dir = p_cur_dir;
  /******************/
  /*      end       */
  /******************/
  return true;
}

bool sys_mkdir(const char *dir)
{
  return filesys_create_dir(dir);
}

bool sys_readdir(int fd, char *name)
{
  lock_acquire(&filesys_lock);
  struct file *p_file = process_get_file(fd);
  if(p_file == NULL && !inode_is_dir(file_get_inode(p_file)))
  {
    lock_release(&filesys_lock);
    return false; 
  }
  
  struct dir *p_dir = (struct dir*)p_file;
  
  bool success = false;
  do
  {
    success = dir_readdir(p_dir, name);
  }
  while(success && (strcmp(name,".") == 0 || strcmp(name,"..") == 0));

  lock_release(&filesys_lock);
  return success;
}

bool sys_isdir(int fd)
{	
  struct file *p_file = process_get_file(fd);

  if(p_file != NULL)
    return inode_is_dir(file_get_inode(p_file));
  else
    return false;
  
}

int sys_inumber(int fd)
{
  struct file *p_file = process_get_file(fd);
  if(p_file != NULL)
    return (uint32_t)inode_get_inumber(file_get_inode(p_file));
  else
    return -1;
}
