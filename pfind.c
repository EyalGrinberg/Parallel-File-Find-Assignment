#include <stdlib.h>
#include <linux/limits.h>
#include <threads.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>


/* directories queue */

typedef struct dir_node {
    char dir_path[PATH_MAX];
    struct dir_node *next;
} dir_node;

typedef struct dir_queue {
    struct dir_node *head;
    struct dir_node *tail;
} dir_queue;


/* threads queue */

typedef struct thread_node {
    char dir_to_handle[PATH_MAX];
    cnd_t cv_for_curr_thread; /* each thread will have a Condition Variable and will be sent to sleep and will be awaken by this CV */
    struct thread_node *next;
} thread_node;

typedef struct sleeping_threads_queue {
    int num_of_sleeping_threads; /* will be used for checking if the program needs to end. */ 
    struct thread_node *head;
    struct thread_node *tail;
} sleeping_threads_queue;


/* function declerations */

void insert_dir_to_tail(dir_node*);
void remove_head_dir_from_Q();
char* insert_thread_to_tail();
void remove_head_thread_from_Q(char*);
int threads_main_func();
void dir_search(char*);


/* mutex locks */

mtx_t sleeping_threads_Q_lock;
mtx_t dir_Q_lock;
mtx_t init_lock;
mtx_t num_of_matching_files_lock;


/* global variables */

int total_num_of_threads;
char *search_term;
int num_of_matching_files = 0; /* needs to be protected with num_of_matching_files_lock */
int error_occured_in_one_of_the_threads = 0; /* "boolean" flag, to know if need to end main with exit(0) or exit(1) */
int kill_all_threads = 0; /* "boolean" flag, to know when the program needs to end */
int only_one_thread_shall_pass = 1; /* "boolean" flag, to prevent all but one of the threads from entering a certain section */
thrd_t *threads_arr; /* for creating and joining threads */
/* pointers to the two queues*/
dir_queue *dir_Q;
sleeping_threads_queue *sleeping_threads_Q;


/* enqueue and dequeue functions for the directories queue */

void insert_dir_to_tail(dir_node *dir_to_insert)
/* input: pointer to dir_node.
 * memory for this dir_node was already allocated before calling the function. 
 * the dir_node's fields were already initialized also. */
{
    if (dir_Q -> head == NULL) { /* queue was empty */
        dir_Q -> head = dir_to_insert;
        dir_Q -> tail = dir_to_insert;
    }
    else { /* queue wasn't empty, update only tail */
        dir_Q -> tail -> next = dir_to_insert;
        dir_Q -> tail = dir_to_insert;
    }
}

void remove_head_dir_from_Q()
/* we already checked if the queue isn't empty.
 * also, the dir_path of the head directory was already extracted before calling the function. */
{
    dir_node *prev_head = dir_Q -> head;
    dir_Q -> head = dir_Q -> head -> next;
    if (dir_Q -> head == NULL) { /* there was just one directory in the queue */
        dir_Q -> tail = NULL;
    }
    free(prev_head);
}


/* enqueue and dequeue functions for the threads queue */

char* insert_thread_to_tail()
/* inserts thread to the tail of the queue (sends thread to sleep).
 * if a new directory is found and the thread is in the head of the queue (slept more than any other thread),
 * it will get a signal to wake up after his dir_to_handle is updated with the new directory's path.
 * the function returns the path of the new directory that was found and will be handled by the woken thread. */
{
    char *new_dir_path;
    thread_node *thread_to_insert = malloc(sizeof(thread_node)); /* by the instructions, no need to check if allocation succeded */
    cnd_init( &(thread_to_insert -> cv_for_curr_thread) ); /* need to initialize condition variable of each thread_node created */
    /* the thread_node's dir_to_handle is still NULL when inserted to the queue. */
    /* enqueue thread (sleeping_threads_Q_lock is already locked by now) */
    if (sleeping_threads_Q -> head == NULL) { /* queue was empty */
        sleeping_threads_Q -> head = thread_to_insert;
        sleeping_threads_Q -> tail = thread_to_insert;
    }
    else { /* queue wasn't empty, update only tail */
        sleeping_threads_Q -> tail -> next = thread_to_insert;
        sleeping_threads_Q -> tail = thread_to_insert;
    }
    /* increment num of sleeping and send thread to sleep */
    sleeping_threads_Q -> num_of_sleeping_threads++; 
    mtx_unlock(&dir_Q_lock); /* has to unlock dir_Q before sending thread to sleep */
    cnd_wait( &(thread_to_insert -> cv_for_curr_thread), &sleeping_threads_Q_lock );
    /* the thread will get a signal to wake up if a new directory is found and the thread is head of the queue (slept more than any other thread).
     * when this thread will get a signal to wake up it will continue from this line. */ 
    new_dir_path = thread_to_insert -> dir_to_handle; /* when thread is woken up, his dir_to_handle field will contain the path of the new directory. */
    cnd_destroy( &(thread_to_insert -> cv_for_curr_thread) ); /* thread is woken up now. */
    free(thread_to_insert); /* the remove function doesn't free the head thread, we do this here. */
    return new_dir_path; /* the path of the new directory will be returned to the while(1) loop and the thread will handle it. */
}

void remove_head_thread_from_Q(char *new_dir_path)
/* the function doesn't really remove the head thread from the queue, it just updates pointers to the new head and tail.
 * it also puts the path of the new directory that was found in the dir_to_handle field of the head thread we want to wake up,
 * and signals it to wake up.
 * the idea is that when this thread wakes up after the "wait" in the insert function, his dir_to_handle field would already be updated
 * and the insert function will return this path to the while(1) loop so that the new directory that was found would be processed. 
 * we don't need to free the head thread_node because the insert function does that before returning. */
{
    thread_node *prev_head_thread = sleeping_threads_Q -> head;
    /* update dir_to_handle field of the thread we want to wake up */
    if (new_dir_path == NULL) {
        strcpy(prev_head_thread -> dir_to_handle, "");
    }
    else {
        strcpy(prev_head_thread -> dir_to_handle, new_dir_path); 
    }
    sleeping_threads_Q -> head = sleeping_threads_Q -> head -> next;
    if (sleeping_threads_Q -> head == NULL) { /* there was only one sleeping thread in the queue */
        sleeping_threads_Q -> tail = NULL;
    }
    sleeping_threads_Q -> num_of_sleeping_threads--; /* we wake up one sleeping thread. */
    cnd_signal( &(prev_head_thread -> cv_for_curr_thread) ); /* wake the head thread up. */
}


/* primary functions */

int threads_main_func() 
{
    char new_dir_path[PATH_MAX];
    char *tmp_dir;
    /* each created thread will get stuck in this line until main will create all threads and unlock the lock */
    mtx_lock(&init_lock); 
    mtx_unlock(&init_lock);
    /* from here all threads are working */

    while(1) 
    {  
        mtx_lock(&dir_Q_lock);
        if (dir_Q -> head != NULL) { /* directories queue isn't empty, let's pop a directory and handle it. */
            strcpy(new_dir_path, dir_Q -> head -> dir_path);
            remove_head_dir_from_Q();
            mtx_unlock(&dir_Q_lock); /* it's OK to unlock because we already extracted the relevant directory to work on */
            dir_search(new_dir_path); 
        }
        else { /* directories queue is empty */
            mtx_lock(&sleeping_threads_Q_lock); /* we want to access the num_of_sleeping_threads field */
            if (total_num_of_threads != 1 + sleeping_threads_Q -> num_of_sleeping_threads) { /* need to send the thread to sleep */
                /* insert will send the thread to sleep, in the future when this thread will have a directory to handle,
                 * remove_head_thread_from_Q will update the thread's dir_to_handle field with the directory's path and signal the thread to wake up.
                 * the thread will wake up in the insert function and insert will return the directory's path from the thread's dir_to_handle field. */
                tmp_dir = insert_thread_to_tail();
                if (tmp_dir != NULL && strcmp(tmp_dir, "") != 0) {
                    strcpy(new_dir_path, tmp_dir);
                }
                /* now the thread should be awake and ready to handle the directory */
                mtx_unlock(&sleeping_threads_Q_lock);
                if (only_one_thread_shall_pass == 1) { 
                    /* not a good name for a boolean flag, but it works logically. threads go to sleep in the insert_thread_to_tail call.
                     * we want that when the stop condition is met, the last woken thread will wake all threads up 
                     * and all of them won't search (the flag will be 0). */
                    dir_search(new_dir_path);
                }
            }
            else { /* stop condition is met, the program needs to end. */
            /* only the last awaken thread will get here, and it will happen when all the other threads are idle. */
                kill_all_threads = 1;
                mtx_unlock(&sleeping_threads_Q_lock);
                mtx_unlock(&dir_Q_lock);
            }
        }
        if (kill_all_threads) { /* wake up all threads so they will exit */
            /* we want only one thread (the last awaken thread) to enter this while loop,
             * because we want just to wake all the sleeping threads so they will exit */
            if (only_one_thread_shall_pass == 1) { /* the last awaken thread will pass here */
                only_one_thread_shall_pass = 0; 
                /* and immediately switch the "flag" off so all the other threads won't be able to pass when they are awake. 
                 * instead they will go to the else and break from the while(1) */
                mtx_lock(&sleeping_threads_Q_lock);
                /* wake all the other threads up using remove_head_thread_from_Q,
                 * we don't care about all the other things remove does as long as it signals all the threads to wake up. */
                while (sleeping_threads_Q -> num_of_sleeping_threads > 0) { 
                    remove_head_thread_from_Q(NULL);
                }
                mtx_unlock(&sleeping_threads_Q_lock);
                break; /* break from the while(1) = thread will exit */
            }
            else {
                break; /* break from the while(1) = thread will exit */
            }
        }
    }
    return 0;
}


void dir_search(char *dir_path)
/* this function iterates on the directory's entries. 
 * if a file is found ---> checks if contains the search term.
 * if a directory is found:
 * either wake a sleeping thread up and let it handle it or add the directory to the queue if all threads are working. */
{
    char concatanated_path[PATH_MAX];
    DIR *dir_ptr;
    struct dirent *entry;
    struct stat statbuf;
    dir_node *new_dir;
    if (dir_path == NULL) {
        error_occured_in_one_of_the_threads = 1;
        fprintf(stderr, "Error: directory's path is NULL\n");
        return;
    }
    dir_ptr = opendir(dir_path);
    if (dir_ptr == NULL) {
        error_occured_in_one_of_the_threads = 1;
        perror("Error: opendir failed\n");
        return;
    }

    while ((entry = readdir(dir_ptr)) != NULL) {
        sprintf(concatanated_path, "%s/%s", dir_path, entry -> d_name); /* concat entry to current path */
        if ( strcmp(entry -> d_name, ".") != 0 && strcmp(entry -> d_name, "..") != 0 ) { /* ignore current and parent directories */
            /* from chatGPT */
            if (stat(concatanated_path, &statbuf) != 0) { /* get info about entry */
                error_occured_in_one_of_the_threads = 1;
                perror("Error: stat failed\n");
                continue;
            }
            /* it's a directory */
            if (S_ISDIR(statbuf.st_mode)) { 
                if (access(concatanated_path, R_OK | X_OK) != 0) { /* not searchable */
                    if (errno == EACCES) { /* by the instructions, need to treat this error differently */
                        printf("Directory %s: Permission denied.\n", concatanated_path);
                    }
                    else {
                        fprintf(stderr, "Error (not EACCES): directory isn't searchable\n");
                        error_occured_in_one_of_the_threads = 1;  
                    }
                }
                else { /* directory is searchable */
                    mtx_lock(&sleeping_threads_Q_lock);
                    if (sleeping_threads_Q -> head != NULL) { /* if there's a sleeping thread, wake it up and let it work on the directory */
                        remove_head_thread_from_Q(concatanated_path); /* this is where all the magic happens... */
                        mtx_unlock(&sleeping_threads_Q_lock);
                    }
                    else { /* there's no sleeping thread, add directory to tail of dir_Q */
                        mtx_unlock(&sleeping_threads_Q_lock); /* in case we didn't unlock in the if block */
                        /* create dir_node, update fields, and insert */
                        new_dir = malloc(sizeof(dir_node));
                        strcpy(new_dir -> dir_path, concatanated_path);
                        new_dir -> next = NULL;
                        mtx_lock(&dir_Q_lock);
                        insert_dir_to_tail(new_dir);
                        mtx_unlock(&dir_Q_lock);
                    }
                }
            }
            /* it's a file */
            else { 
                if (strstr(entry -> d_name, search_term) != NULL) { /* match found */
                    mtx_lock(&num_of_matching_files_lock);
                    num_of_matching_files++;
                    mtx_unlock(&num_of_matching_files_lock);
                    printf("%s\n", concatanated_path);
                }
            }
        }
    }
    closedir(dir_ptr);
}


int main(int argc, char *argv[])
{   
    dir_node *root_dir;
    int i, rc;

    /* handling arguments */
    if (argc != 4) {
    fprintf(stderr, "Error: expected 3 arguments\n");
    exit(1);
    }
    search_term = argv[2];
    total_num_of_threads = atoi(argv[3]);
    threads_arr = malloc(total_num_of_threads * sizeof(thrd_t));
    if (access(argv[1], R_OK | X_OK) != 0) { /* not searchable */
        printf("Directory %s: Permission denied.\n", argv[1]);
        fprintf(stderr, "Error: root directory isn't searchable\n");
        exit(1);
    }

    /* memory allocation for the two queues */
    dir_Q = malloc(sizeof(dir_queue));
    if (dir_Q == NULL) {
        fprintf(stderr, "Error: memory allocation failed\n");
        exit(1);
    }
    sleeping_threads_Q = malloc(sizeof(sleeping_threads_queue));
    if (sleeping_threads_Q == NULL) {
        fprintf(stderr, "Error: memory allocation failed\n");
        exit(1);
    }
    dir_Q -> head = NULL;
    dir_Q -> tail = NULL;
    sleeping_threads_Q -> head = NULL;
    sleeping_threads_Q -> tail = NULL;
    sleeping_threads_Q -> num_of_sleeping_threads = 0;

    /* inserting root directory to dir_Q */
    root_dir = malloc(sizeof(dir_node));
    if (root_dir == NULL) {
        fprintf(stderr, "Error: memory allocation failed\n");
        exit(1);
    }
    strcpy(root_dir -> dir_path, argv[1]);
    root_dir -> next = NULL;
    insert_dir_to_tail(root_dir); /* no need to lock because we haven't created threads yet. */

    /* initialize all mutex locks */
    mtx_init(&sleeping_threads_Q_lock, mtx_plain);
    mtx_init(&dir_Q_lock, mtx_plain);
    mtx_init(&init_lock, mtx_plain);
    mtx_init(&num_of_matching_files_lock, mtx_plain);

    /* creating all threads.
     * by locking init_lock before the for loop and unlocking it after the for loop,
     * in addition to the first two lines of "threads_main_func":
     * 1. mtx_lock(&init_lock);
     * 2. mtx_unlock(&init_lock);
     * we enforce each thread that was created to wait in the beginning of threads_main_func until all threads are created,
     * and only then the threads will start searching. */
    mtx_lock(&init_lock);
    for (i = 0; i < total_num_of_threads; i++) {
        rc = thrd_create(&threads_arr[i], threads_main_func, NULL);
        if (rc != 0) {
            fprintf(stderr, "Error: thrd_create failed\n");
            exit(1);
        }
    }
    mtx_unlock(&init_lock);

    /* join all threads when they finish */
    for (i = 0; i < total_num_of_threads; i++) {
        rc = thrd_join(threads_arr[i], NULL);
        if (rc != 0) {
            fprintf(stderr, "Error: thrd_join failed\n");
            exit(1);
        }
    }

    /* destroy and clean up everything */
    mtx_destroy(&sleeping_threads_Q_lock);
    mtx_destroy(&dir_Q_lock);
    mtx_destroy(&init_lock);
    mtx_destroy(&num_of_matching_files_lock);
    free(dir_Q);
    free(sleeping_threads_Q);
    free(threads_arr);
    
    printf("Done searching, found %d files\n", num_of_matching_files);
    if (error_occured_in_one_of_the_threads > 0) {
        exit(1);
    }
    exit(0);
}
