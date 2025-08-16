#ifndef __minispark_h__
#define __minispark_h__

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <unistd.h>
#include <libgen.h>


#define MAXDEPS (2)
#define TIME_DIFF_MICROS(start, end) \
  (((end.tv_sec - start.tv_sec) * 1000000L) + ((end.tv_nsec - start.tv_nsec) / 1000L))

struct RDD;
struct List;
struct ListNode;
struct ListIter;
struct TaskQueue;
struct ThreadPool;

typedef struct RDD RDD; // forward decl. of struct RDD
typedef struct ListNode ListNode;
typedef struct List List;
typedef struct ListIter ListIter;
typedef struct TaskQueue TaskQueue;
typedef struct ThreadPool ThreadPool;
typedef TaskQueue MonitorQueue;


struct ListNode{
  void *data;
  ListNode *next;
};

struct List{
  ListNode *head;
  ListNode *tail;
  int num_items;
};

struct ListIter{
  ListNode *current;
};

struct TaskQueue
{
  ListNode *head;
  ListNode *tail;
  int num_tasks;
};

struct ThreadPool
{
  TaskQueue *queue;
  MonitorQueue *monitor_queue;
  pthread_t *threads;
  pthread_t *monitor_thread;

  pthread_mutex_t queue_mutex;
  pthread_mutex_t work_mutex;
  pthread_mutex_t monitor_mutex;

  pthread_cond_t work_start;
  pthread_cond_t work_ready;
  pthread_cond_t new_work;
  pthread_cond_t new_monitor;

  bool started;
  bool shutdown;
  int numthreads;
};

// Different function pointer types used by minispark
typedef void* (*Mapper)(void* arg);
typedef int (*Filter)(void* arg, void* pred);
typedef void* (*Joiner)(void* arg1, void* arg2, void* arg);
typedef unsigned long (*Partitioner)(void *arg, int numpartitions, void* ctx);
typedef void (*Printer)(void* arg);

typedef enum {
  MAP,
  FILTER,
  JOIN,
  PARTITIONBY,
  FILE_BACKED
} Transform;

struct RDD {    
  Transform trans; // transform type, see enum
  void* fn; // transformation function
  void* ctx; // used by minispark lib functions
  List** partitions; // list of partitions
  int partitions_cnt; // number of partitions
  
  RDD* dependencies[MAXDEPS];
  int dependencies_cnt; // 0, 1, or 2

  // you may want extra data members here
  pthread_mutex_t lock;
  int materialized_cnt;
};

typedef struct {
  struct timespec created;
  struct timespec scheduled;
  size_t duration; // in usec
  RDD* rdd;
  int pnum;
} TaskMetric;

typedef struct {
  RDD* rdd;
  int pnum;
  TaskMetric* metric;
  bool finalrdd;
} Task;

//////// actions ////////

// Return the total number of elements in "dataset"
int count(RDD* dataset);

// Print each element in "dataset" using "p".
// For example, p(element) for all elements.
void print(RDD* dataset, Printer p);

//////// transformations ////////

// Create an RDD with "rdd" as its dependency and "fn"
// as its transformation.
RDD* map(RDD* rdd, Mapper fn);

// Create an RDD with "rdd" as its dependency and "fn"
// as its transformation. "ctx" should be passed to "fn"
// when it is called as a Filter
RDD* filter(RDD* rdd, Filter fn, void* ctx);

// Create an RDD with two dependencies, "rdd1" and "rdd2"
// "ctx" should be passed to "fn" when it is called as a
// Joiner.
RDD* join(RDD* rdd1, RDD* rdd2, Joiner fn, void* ctx);

// Create an RDD with "rdd" as a dependency. The new RDD
// will have "numpartitions" number of partitions, which
// may be different than its dependency. "ctx" should be
// passed to "fn" when it is called as a Partitioner.
RDD* partitionBy(RDD* rdd, Partitioner fn, int numpartitions, void* ctx);

// Create an RDD which opens a list of files, one per
// partition. The number of partitions in the RDD will be
// equivalent to "numfiles."
RDD* RDDFromFiles(char* filenames[], int numfiles);

/**
 * Frees RDD with all of its associated data in partitions. Takes into account
 * if RDD was a FILE-BACKED
 * 
 * @param rdd - rdd to free
 */
void hard_free_rdd(RDD *rdd);

/**
 * Frees RDD while sparing data inside of it (important for RDDs that do not create new memory such
 * as FILTER and PARTITION by RDD
 * 
 * @param rdd - rdd to free
 */
void soft_free_rdd(RDD *rdd);

//////// MiniSpark ////////

/**
 * Creates new task
 * 
 * @param rdd - rdd for which we want to materialize
 * @param pnum - partition number of materialization
 * @param finalrdd - indicated if it is the root rdd which we want to materialize with this task
 * 
 * @return new task to add to the queue
 */
Task* create_task(RDD *rdd, int pnum, bool finalrdd);

// Submits work to the thread pool to materialize "rdd".
void execute(RDD* rdd);

// Submits tasks to the work queue
void execute_helper(RDD* rdd);

// Creates the thread pool and monitoring thread.
void MS_Run();

// Waits for work to be complete, destroys the thread pool, and frees
// all RDDs allocated during runtime.
void MS_TearDown();

/**
 * Initializes an empty list of void * elements
 * 
 * @returns pointer to the list struct
 */
List* list_init();

/**
 * Adds new element data to the list. Importantly it copies the address of the data without cleaning it
 * 
 * @param list - list of elements
 * @param data - data pointer to add (malloc allocated)
 * @returns true if addition was successful and false otherwise
 */
bool list_add(List *list, void *data);

/**
 * Gets element at the index from linked list
 * 
 * @param list - list of elements
 * @param indx - indx of the element we want to access
 * @return element if indx is not out of bound and NULL otherwise
 */
void* list_get(List *list, int indx);

/**
 * Creates an iterator for the list object
 * 
 * @param list - list of elements
 * @return list iterator that starts from the beginning
 */
ListIter list_get_iter(List *list);

/**
 * Gets next element from the list
 * 
 * @param iter - iterator created from the list
 * @return data element or NULL if we reached the end
 */
void* iter_next(ListIter *iter);

/**
 * Frees list and all of its associated data
 * 
 * @param list - list of elements
 */
void list_free(List *list);

/**
 * Frees the list but spares the data inside of it. Important for RDDs that don't create new memory
 * 
 * @param list - list of elements
 */
void list_node_free(List *list);

/**
 * Initializes empty task queue to work with
 * 
 * @return queue to work with
 */
TaskQueue* queue_init();

/**
 * Pushes new task onto the task queue to the end
 * 
 * @param queue - queue onto which to push
 * @param task - new work task that needs to be done
 * @return true if pushing was successful and false otherwise
 */
bool queue_push(TaskQueue *queue, Task *task);

/**
 * Returns the task from the top of the queue
 * 
 * @param queue - task queue from which to pop
 */
Task* queue_pop(TaskQueue *queue);

/**
 * Main function to monitor and print out task completion time to log file
 * 
 * @param arg - unused
 */
void monitor_func(void *arg);

/**
 * Main worker function which will continuously fetch tasks from the taskqueue
 * 
 * @param arg - unused
 */
void worker_func(void *arg);

/**
 * Checks if the dependencies for the given task were resolved. It first checks if task's RDD is MAP or Filter in
 * which case it only needs one partition. If task's RDD is PARTITIONBY or JOIN, it checks that dependency RDDs
 * are fully materialized
 * 
 * @param task - task which must be done
 * @return true if dependencies are resolved and false otherwise
 */
bool validate_task(Task *task);

/**
 * Once dependancies have been checked, materializes partition depending on transit
 * 
 * @param task - work that must be done
 */
void resolve_task(Task *task);


// Helper function to print string representation of Transform enum
char *convert_enum_str(Transform t);

#endif // __minispark_h__

