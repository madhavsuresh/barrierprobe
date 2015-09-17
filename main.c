#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
#include <pthread.h>  // pthread api
#include <unistd.h>   // unix standard apis
#include <sched.h>
#include "main.h"
/* WISHLIST
 *
 * TODO: 
 * -phi barrier paper mentions that > 60 cycles of sleep led 
 *  to poor performance. There might be analysis there that can 
 *  figure out why. 
 *  -if we can communicate without sideeffects, then we can fill in the wait time with communication. 
 *      -probably not possible, how to minimize sideeffects
 *          -gossip protocol?
 *
 */

#define REGION_LENGTH 100
pthread_t *tid;  // array of thread ids
uint64_t num_threads = 8;
char * leader_region;
uint64_t * global_max;
uint64_t global_count; 
uint64_t barrier_assoc;

struct barrier_entry { 
    uint64_t wait_time;
    uint64_t prefix_cycles;
    void (*func)(void *);
    struct barrier_entry * next;
} typedef barrier_entry_t;

//keep track of a tail too...ugh 

//TODO: allocate thread local barrier_entry 

//TODO: need to account for change in input size. 
struct  linked_list {
    struct linked_list * next;
    struct linked_list * prev;
    void (*func)(void *);
    uint64_t cycles;

} typedef func_info_t;

func_info_t * tail; 
func_info_t * front;

typedef struct barrier_data {
  long count[2];
  long cur;
} barrier_data_t;

barrier_data_t barrier_data;

//TODO: Use rdtscp
static inline uint64_t __attribute__((always_inline))
rdtsc (void)
{
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi));
    return lo | ((uint64_t)(hi) << 32);
}


int counter(void (*func)(void *), void * data){
    uint64_t start, end;
    start = rdtsc();
    func(NULL);
    end = rdtsc();
    return end-start;
}

func_info_t * find_timed_function(void (*func)(void *)){
    func_info_t * cur = front;
    while (cur != NULL) {
        if (cur->func == func) { 
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

//TODO: Need to hold locks on global context. 
//Maybe make local copies everywhere. paxos on local cores. 
///re
func_info_t * insert_timed_function_gctx(void (*func)(void *)){
    uint64_t cycles;
    func_info_t * to_insert = malloc(sizeof(func_info_t));
    tail->next = to_insert;
    cycles = counter(func, NULL);
    to_insert->func = func;
    to_insert->cycles = cycles; 
    to_insert->next = NULL;
    tail = to_insert;
    return to_insert; 
}

//TODO: a bit of a misnomer, rename
func_info_t * register_timed_function_gctx(void (*func)(void *)) {
    func_info_t * func_info; 
    if ((func_info = find_timed_function(func)) == NULL) {
        func_info = insert_timed_function_gctx(func);
    }
    return func_info;
}

void big_barrier()
{
  long old;
  volatile long * globalcur = &barrier_data.cur;
  long mycur = *globalcur;
  volatile long * mycount = &barrier_data.count[mycur];
 
  old = __sync_fetch_and_add(mycount,1);

  if (old==num_threads-1) { 
    // I'm the last to the party
    /* resolve approxmiate barrier wait times */

    /* end barrier wait resolution */
    *globalcur ^= 0x1;
    *mycount = 0;
  } else {
   // k1om compiler does not know what "volatile" means
   // hence this hand-coding.
   do { 
   	__asm__ __volatile__( "movq %1, %0" : "=r"(old) : "m"(*mycount) : );
   } while (old);

  }
}

void register_barrier_tctx(void (*func)(void*), 
        barrier_entry_t ** barrier_tail, int * num_barriers) {

    func_info_t * func_info = register_timed_function_gctx(func);
    barrier_entry_t * ptr = malloc(sizeof(barrier_entry_t));
    bzero(ptr, sizeof(barrier_tail));
    ptr->prefix_cycles = func_info->cycles;
    ptr->func = func;
    ptr->next = NULL;
    if (*barrier_tail == NULL) {
        (*barrier_tail) = ptr;
    } else {
        (*barrier_tail)->next = ptr;
        (*barrier_tail) = ptr;
    }
    (*num_barriers)++;
}

//TODO: need to test this. 
void write_to_leader_page_tctx(uint64_t thread_id, barrier_entry_t * barrier_head) {
    char * region_ptr = leader_region+(thread_id*REGION_LENGTH*sizeof(uint64_t));
    barrier_entry_t * cur = barrier_head;
    while (cur != NULL) {
        *((uint64_t *)region_ptr)  = cur->prefix_cycles;
        region_ptr += sizeof(uint64_t);
        cur = cur->next;
    }
}

//TODO: really should write tests
void test_write_to_leader_page_tctx(char * leader_region, uint64_t num_threads, int num_cycles) { 

}

//TODO: this is not implemented in a cache friendly way. 
void leader_get_max_tctx(uint64_t thread_id) { 
    if (thread_id == 0) {
        //TODO: this is not cache friendly at all 
        int num_iter = 100, i, j;
        //TODO: FREE MAX_ARRAY
        uint64_t * max_array = malloc(sizeof(uint64_t) * num_threads * REGION_LENGTH);
        uint64_t  * leader_ptr = (uint64_t *) leader_region;
        for (i = 0; i < num_iter; i++) {
            uint64_t max = 0;
            for (j = 0; j < num_threads; j++) { 
                if (leader_ptr[i+j*(sizeof(uint64_t)*REGION_LENGTH)] > max) {
                    max  = leader_ptr[i+j*(sizeof(uint64_t)*REGION_LENGTH)];
                }
            }
            max_array[i] = max;
        }
        global_max = max_array;
    }
    big_barrier();
}

void update_barrier_stat_info_tctx(barrier_entry_t * barrier_head, int thread_id) {
    barrier_entry_t * cur = barrier_head;
    int i = 0;
    while (cur != NULL) {
        cur->wait_time = global_max[i] - cur->prefix_cycles;
        cur = cur->next;
        i++;
    }
}

//global resolution of barriers, should only be called by one? 
void resolve_barriers_tctx(uint64_t thread_id, barrier_entry_t * barrier_head) {
    int leader = thread_id == 0 ? 1 : 0;
    if (leader) { 
        //allocate page for everyone to write pointers 
        //to their structures (or just memcpy stuff over?)
        //maybe this should be reserved 
        //this will cause cache flush and invalidation
        char * mem_write_area = (char *)malloc(sizeof(uint64_t) * REGION_LENGTH * num_threads);
        leader_region = mem_write_area;
        bzero(mem_write_area, sizeof(uint64_t)*REGION_LENGTH*num_threads);
    }
    big_barrier();
    write_to_leader_page_tctx(thread_id, barrier_head);
    big_barrier();
    leader_get_max_tctx(thread_id);
    update_barrier_stat_info_tctx(barrier_head, thread_id);
}

void pop_barrier_entry(barrier_entry_t ** barrier_head) { 
    barrier_entry_t * next = (*barrier_head)->next;
    free(*barrier_head);
    (*barrier_head) = next;
}

void barrier(barrier_entry_t ** barrier_head) {
    barrier_entry_t * cur = *barrier_head;
    uint64_t wait_time = cur->wait_time;
    cur->func(NULL);
    barrier_sleep(1999);
    barrier_sleep(1999);
    barrier_sleep(1999);
    barrier_sleep(1999);
    barrier_sleep(1999);
    barrier_sleep(1999);
    barrier_sleep(1999);
    barrier_sleep(1999);
    barrier_sleep(1999);
    barrier_sleep(1999);
    barrier_sleep(1999);
    barrier_sleep(1999);
    barrier_sleep(1999);
    //barrier_sleep(wait_time);
    pop_barrier_entry(barrier_head);
}


void initialize_linked_list() {
    front = malloc(sizeof(func_info_t));
    bzero(front, sizeof(func_info_t));
    tail = front; 
}


//TODO: pace with 1 thread
void * barrier_shell_tctx(void * arg) {
    int thread_id = (long) arg;
    int adding = (thread_id+1)*100;
    int num_barriers = 0;
    int sync_result;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(thread_id,&set);
    if (sched_setaffinity(0,sizeof(set),&set)<0) { // do it
        perror("Can't setaffinity");  // hopefully doesn't fail
        exit(-1);
  }
    barrier_entry_t * barrier_tail = NULL;
    barrier_entry_t * barrier_head = NULL; //= malloc(sizeof(barrier_entry_t));
    //barrier_entry_t * barrier_head = barrier_tail;
    if (thread_id % 2 == 0) {
        register_barrier_tctx(test2, &barrier_tail, &num_barriers);
    } else {
        register_barrier_tctx(test3, &barrier_tail, &num_barriers);
    }
    barrier_head = barrier_tail;
    //end body, resolve barriers
    resolve_barriers_tctx(thread_id, barrier_head);
    big_barrier();
    printf("before barriers %d\n", thread_id);
    //TODO: fflush sideeffects? added time? 
    __sync_fetch_and_add(&barrier_assoc, adding);
    while (num_barriers > 0) {
        barrier(&barrier_head);
        num_barriers--;
    }
    sync_result = __sync_val_compare_and_swap(&barrier_assoc, 3600, 3601);
}

//optimiality for data transfer size 
int main() { 
    //TODO: Num threads
    uint64_t output;
    int i, rc;
    initialize_linked_list();
    global_count = 0;
    func_info_t * info = insert_timed_function_gctx(*test2);
    //func_info_t * info = insert_timed_function_gctx(*test);
    printf("counter: %d\n", counter(*fflush, NULL));
    
    printf("%lu\n", info->cycles);
    info = insert_timed_function_gctx(*test3);
    printf("**%lu\n", info->cycles);
    tid = (pthread_t*)malloc(sizeof(pthread_t)*num_threads);
    
    for (i=0; i < num_threads; i++) {
        rc = pthread_create(&(tid[i]), NULL, *barrier_shell_tctx, (void *) i);
    }

  for (i=0;i<num_threads;i++) {
    if (tid[i]!=0xdeadbeef) { 
     // printf("Joining with %ld, tid %lu\n", i, tid[i]);
     rc =  pthread_join(tid[i],NULL);   // 
      if (rc!=0) { 
	//printf("Failed to join with %ld!\n",i);
	perror("join failed");
      } else { 
	//printf("Done joining with %ld\n",i);
      }
    } else { 
      printf("Skipping %ld (wasn't started successfully)\n",i);
    }
  }
}
