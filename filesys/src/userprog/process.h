#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

#define NO_LOAD 0
#define SUCCESS_LOAD 1
#define FAIL_LOAD 2

struct process_file {
	struct file *file;
	int fd;
	struct list_elem elem;
};

struct child_process {
	int pid;
	int load;
	bool wait;
	bool exit;
	struct semaphore sema_exit;
	struct semaphore sema_load;
	int status;
	struct list_elem elem;
};

struct child_process *add_child_process (int pid);
struct child_process *get_child_process (int pid);
void remove_child_process (struct child_process *cp);
void remove_child_processes(void);
int process_add_file (struct file *f);
struct file* process_get_file (int fd);
void process_close_file (int fd);
tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);
void argument_stack(char **parse , int count , void **esp);

#endif /* userprog/process.h */
