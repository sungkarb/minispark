#define _GNU_SOURCE
#include "minispark.h"

ThreadPool *threadpool;
FILE *fn;

// Working with metrics...
// Recording the current time in a `struct timespec`:
//    clock_gettime(CLOCK_MONOTONIC, &metric->created);
// Getting the elapsed time in microseconds between two timespecs:
//    duration = TIME_DIFF_MICROS(metric->created, metric->scheduled);
// Use `print_formatted_metric(...)` to write a metric to the logfile. 
void print_formatted_metric(TaskMetric* metric, FILE* fp) {
  fprintf(fp, "RDD %p Part %d Trans %d -- creation %10jd.%06ld, scheduled %10jd.%06ld, execution (usec) %ld\n",
	  metric->rdd, metric->pnum, metric->rdd->trans,
	  metric->created.tv_sec, metric->created.tv_nsec / 1000,
	  metric->scheduled.tv_sec, metric->scheduled.tv_nsec / 1000,
	  metric->duration);
}

int max(int a, int b)
{
  return a > b ? a : b;
}

RDD *create_rdd(int numdeps, Transform t, void *fn, ...)
{
  RDD *rdd = malloc(sizeof(RDD));
  if (rdd == NULL)
  {
    printf("error mallocing new rdd\n");
    exit(1);
  }

  va_list args;
  va_start(args, fn);

  int maxpartitions = 0;
  for (int i = 0; i < numdeps; i++)
  {
    RDD *dep = va_arg(args, RDD *);
    rdd->dependencies[i] = dep;
    maxpartitions = max(maxpartitions, dep->partitions_cnt);
  }
  va_end(args);

  rdd->dependencies_cnt = numdeps;
  rdd->trans = t;
  rdd->fn = fn;
  rdd->ctx = NULL;
  rdd->partitions = NULL;
  rdd->partitions_cnt = 0;
  pthread_mutex_init(&rdd->lock, NULL);
  rdd->materialized_cnt = 0;
  return rdd;
}

/* RDD constructors */
RDD *map(RDD *dep, Mapper fn)
{
  RDD *rdd = create_rdd(1, MAP, fn, dep);
  rdd->partitions = malloc(sizeof(List *) * rdd->dependencies[0]->partitions_cnt);
  rdd->partitions_cnt = rdd->dependencies[0]->partitions_cnt;
  return rdd;
}

RDD *filter(RDD *dep, Filter fn, void *ctx)
{
  RDD *rdd = create_rdd(1, FILTER, fn, dep);
  rdd->partitions = malloc(sizeof(List *) * rdd->dependencies[0]->partitions_cnt);
  rdd->partitions_cnt = rdd->dependencies[0]->partitions_cnt;
  rdd->ctx = ctx;
  return rdd;
}

RDD *partitionBy(RDD *dep, Partitioner fn, int numpartitions, void *ctx)
{
  RDD *rdd = create_rdd(1, PARTITIONBY, fn, dep);
  rdd->partitions = malloc(sizeof(List *) * numpartitions);
  rdd->partitions_cnt = numpartitions;
  rdd->ctx = ctx;
  return rdd;
}

RDD *join(RDD *dep1, RDD *dep2, Joiner fn, void *ctx)
{
  RDD *rdd = create_rdd(2, JOIN, fn, dep1, dep2);
  rdd->partitions = malloc(sizeof(List *) * rdd->dependencies[0]->partitions_cnt);
  rdd->partitions_cnt = rdd->dependencies[0]->partitions_cnt;
  rdd->ctx = ctx;
  return rdd;
}

/* A special mapper */
void *identity(void *arg)
{
  return arg;
}

/* Special RDD constructor.
 * By convention, this is how we read from input files. */
RDD *RDDFromFiles(char **filenames, int numfiles)
{
  RDD *rdd = malloc(sizeof(RDD));
  rdd->dependencies_cnt = 0;
  rdd->trans = FILE_BACKED;
  rdd->fn = (void *)identity;
  rdd->ctx = NULL;
  rdd->partitions = malloc(sizeof(List *) * numfiles);
  rdd->partitions_cnt = numfiles;
  pthread_mutex_init(&rdd->lock, NULL);
  rdd->materialized_cnt = numfiles;

  for (int i = 0; i < numfiles; i++)
  {
    FILE *fp = fopen(filenames[i], "r");
    if (fp == NULL) {
      perror("fopen");
      exit(1);
    }
    rdd->partitions[i] = list_init();
    list_add(rdd->partitions[i], fp);
  }
  return rdd;
}

Task* create_task(RDD *rdd, int pnum, bool finalrdd)
{
  // Initialize rdd, pnum, and finalrdd for the task
  Task *task = malloc(sizeof(Task));
  task->rdd = rdd;
  task->pnum = pnum;
  task->finalrdd = finalrdd;

  // Initialize metric for the task
  TaskMetric *metric = malloc(sizeof(TaskMetric));
  metric->pnum = pnum;
  metric->rdd = rdd;
  clock_gettime(CLOCK_MONOTONIC, &metric->created);
  task->metric = metric;
  return task;
}

void submit_tasks(RDD *rdd, bool final)
{
  // Check current RDD
  if (rdd->trans != FILE_BACKED)
  {
    for (int i = 0; i < rdd->partitions_cnt; i++)
    {
      Task *task = create_task(rdd, i, final);
      queue_push(threadpool->queue, task);
    }
  }
  else
  {
    return;
  }

  // Do recursion on each dependency
  for (int i = 0; i < rdd->dependencies_cnt; i++)
  {
    submit_tasks(rdd->dependencies[i], false);
  }
}

void execute(RDD* rdd) {
  // Check if top level is not file backed
  if (rdd->trans == FILE_BACKED)
    return;

  // Submit tasks to the queue and do signal
  submit_tasks(rdd, true);

  pthread_mutex_lock(&threadpool->work_mutex);
  threadpool->started = true;
  pthread_mutex_unlock(&threadpool->work_mutex);
  pthread_cond_broadcast(&threadpool->work_start);

  // Wait until the result is finished
  pthread_mutex_lock(&threadpool->work_mutex);
  while (threadpool->started)
    pthread_cond_wait(&threadpool->work_ready, &threadpool->work_mutex);
  pthread_mutex_unlock(&threadpool->work_mutex);

  return;
}

int count(RDD *rdd) {
  execute(rdd);

  int count = 0;
  // count all the items in rdd
  for (int i = 0; i < rdd->partitions_cnt; i++)
  {
    List *partition = rdd->partitions[i];
    ListIter iter = list_get_iter(partition);
    void *data;
    while ((data = iter_next(&iter)) != NULL)
      count += 1;
  }
  return count;
}

void print(RDD *rdd, Printer p) {
  execute(rdd);
  
  // Print all items
  for (int i = 0; i < rdd->partitions_cnt; i++)
  {
    List *partition = rdd->partitions[i];
    ListIter iter = list_get_iter(partition);
    void *data;
    while ((data = iter_next(&iter)) != NULL)
      p(data);
  }
}

void MS_Run()
{
    // Open file for logging
    fn = fopen("metrics.log", "w+");

    // Initialize threadpool
    threadpool = malloc(sizeof(ThreadPool));

    // Initialize locks and conditional variables
    pthread_mutex_init(&threadpool->queue_mutex, NULL);
    pthread_mutex_init(&threadpool->work_mutex, NULL);
    pthread_mutex_init(&threadpool->monitor_mutex, NULL);

    pthread_cond_init(&threadpool->work_start, NULL);
    pthread_cond_init(&threadpool->work_ready, NULL);
    pthread_cond_init(&threadpool->new_work, NULL);
    pthread_cond_init(&threadpool->new_monitor, NULL);

    threadpool->started = false;
    threadpool->shutdown = false;

    // Initialize the workqueue
    threadpool->queue = queue_init();

    // Compute number of CPUs and initialize threads
    cpu_set_t set;
    CPU_ZERO(&set);

    if (sched_getaffinity(0, sizeof(set), &set) == -1)
    {
        perror("sched_getaffinity");
        exit(1);
    }
    int numcpus = CPU_COUNT(&set);
    threadpool->numthreads = numcpus;
    threadpool->threads = malloc(sizeof(pthread_t) * threadpool->numthreads);
    for (int i = 0; i < threadpool->numthreads; i++)
    {
        if ((pthread_create(&threadpool->threads[i], NULL, worker_func, NULL)) != 0)
        {
            perror("Thread creation failure");
            exit(1);
        }
    }
    // Initialize monitor thread and the queue
    threadpool->monitor_queue = queue_init();
    threadpool->monitor_thread = malloc(sizeof(pthread_t));
    pthread_create(&threadpool->monitor_thread, NULL, monitor_func, NULL);
}

void MS_TearDown()
{
    // Wake up all threads that were waiting and setup the variable
    pthread_mutex_lock(&threadpool->monitor_mutex);
    pthread_mutex_lock(&threadpool->queue_mutex);
    pthread_mutex_lock(&threadpool->work_mutex);
    threadpool->shutdown = true;
    pthread_mutex_unlock(&threadpool->work_mutex);
    pthread_mutex_unlock(&threadpool->queue_mutex);
    pthread_mutex_unlock(&threadpool->monitor_mutex);

    pthread_cond_broadcast(&threadpool->work_start);
    pthread_cond_broadcast(&threadpool->new_work);
    pthread_cond_signal(&threadpool->new_monitor);

    // Wait for all threads to finish
    for (int i = 0; i < threadpool->numthreads; i++)
        pthread_join(threadpool->threads[i], NULL);
    pthread_join(threadpool->monitor_thread[0], NULL);

    // Destroy all locks and conditional variables
    pthread_mutex_destroy(&threadpool->queue_mutex);
    pthread_mutex_destroy(&threadpool->work_mutex);
    pthread_mutex_destroy(&threadpool->monitor_mutex);

    pthread_cond_destroy(&threadpool->work_start);
    pthread_cond_destroy(&threadpool->work_ready);
    pthread_cond_destroy(&threadpool->new_work);
    pthread_cond_destroy(&threadpool->new_monitor);

    // Close the log file
    fclose(fn);
}

void monitor_func(void *arg)
{
  (void *)arg;
  // Monitor processing loop
  while (1)
  {
    TaskMetric *monitor_val;
    pthread_mutex_lock(&threadpool->monitor_mutex);
    while ((monitor_val = (TaskMetric *)queue_pop(threadpool->monitor_queue)) == NULL && !threadpool->shutdown)
      pthread_cond_wait(&threadpool->new_monitor, &threadpool->monitor_mutex);
    pthread_mutex_unlock(&threadpool->monitor_mutex);

    // Check if shutdown signal was sent
    if (threadpool->shutdown)
      return;

    // Do work
    print_formatted_metric(monitor_val, fn);
  }
}

void worker_func(void *arg)
{
    (void *)arg;
    // Work processing loop
    while (1)
    {
        // Check if there is work to do
        pthread_mutex_lock(&threadpool->work_mutex);
        while (!threadpool->started && !threadpool->shutdown)
            pthread_cond_wait(&threadpool->work_start, &threadpool->work_mutex);
        pthread_mutex_unlock(&threadpool->work_mutex);

        // Fetch the task
        Task *task;
        pthread_mutex_lock(&threadpool->queue_mutex);
        while ((task = queue_pop(threadpool->queue)) == NULL && !threadpool->shutdown)
          pthread_cond_wait(&threadpool->new_work, &threadpool->queue_mutex);
        pthread_mutex_unlock(&threadpool->queue_mutex);

        if (threadpool->shutdown)
        {
          return;
        }

        // Return task back to the queue if dependencies haven't been resolved
        if (!validate_task(task))
        {
            pthread_mutex_lock(&threadpool->queue_mutex);
            queue_push(threadpool->queue, task);
            pthread_mutex_unlock(&threadpool->queue_mutex);
            pthread_cond_signal(&threadpool->new_work);
            continue;
        }

        // Work on materializing the task and recording the time
        clock_gettime(CLOCK_MONOTONIC, &task->metric->scheduled);
        resolve_task(task);
        struct timespec time_finished;
        clock_gettime(CLOCK_MONOTONIC, &time_finished);
        task->metric->duration = TIME_DIFF_MICROS(task->metric->scheduled, time_finished);

        // Send this metric to the metric pool
        pthread_mutex_lock(&threadpool->monitor_mutex);
        queue_push(threadpool->monitor_queue, (Task *)task->metric);
        pthread_mutex_unlock(&threadpool->monitor_mutex);
        pthread_cond_signal(&threadpool->new_monitor);

        // Decide if we need to stop working
        RDD *rdd = task->rdd;
        bool tostop = false;
        pthread_mutex_lock(&task->rdd->lock);
        tostop = task->finalrdd && rdd->materialized_cnt == rdd->partitions_cnt;
        pthread_mutex_unlock(&task->rdd->lock);

        // TODO: Destroy task metadata
        if (tostop)
        {
            pthread_mutex_lock(&threadpool->work_mutex);
            threadpool->started = false;
            pthread_mutex_unlock(&threadpool->work_mutex);
            pthread_cond_signal(&threadpool->work_ready);
        }
    }
}

bool validate_task(Task *task)
{
    // Check the type of RDD
    RDD *rdd = task->rdd;
    bool result = false;
    if (rdd->trans == MAP || rdd->trans == FILTER)
    {
        // Check only corresponding partition
        RDD *dependancy = rdd->dependencies[0];
        pthread_mutex_lock(&dependancy->lock);
        result = dependancy->partitions[task->pnum] != NULL;
        pthread_mutex_unlock(&dependancy->lock);
    }
    else if (rdd->trans == PARTITIONBY)
    {   
        // Check if entire dependency is materialized
        RDD *dependancy = rdd->dependencies[0];
        pthread_mutex_lock(&dependancy->lock);
        result = dependancy->materialized_cnt == dependancy->partitions_cnt;
        pthread_mutex_unlock(&dependancy->lock);
    }
    else if (rdd->trans == JOIN)
    {
        // Check if both dependencies are materialized
        RDD *dependency1 = rdd->dependencies[0];
        bool temp = false;
        pthread_mutex_lock(&dependency1->lock);
        temp = dependency1->materialized_cnt == dependency1->partitions_cnt;
        pthread_mutex_unlock(&dependency1->lock);

        if (!temp)
            return false;
        
        RDD *dependency2 = rdd->dependencies[1];
        pthread_mutex_lock(&dependency2->lock);
        result = dependency2->materialized_cnt == dependency2->partitions_cnt;
        pthread_mutex_unlock(&dependency2->lock);
    }
    else
    {
      return true;
    }
    return result;
}

void resolve_task(Task *task)
{
    RDD *rdd = task->rdd;
    switch (rdd->trans)
    {
        case MAP:
        {
            RDD *dependancy = rdd->dependencies[0];
            int pnum = task->pnum;
            List *newpartition = list_init();

            // Start iterating
            List *oldpartition = dependancy->partitions[pnum];
            ListIter iter = list_get_iter(oldpartition);
            void *data;
            while ((data = iter_next(&iter)) != NULL)
            {
                void *transformed_data;
                if (dependancy->trans == FILE_BACKED)
                {
                    while ((transformed_data = ((Mapper)rdd->fn)(data)) != NULL)
                        list_add(newpartition, transformed_data);
                }
                else
                {
                    transformed_data = ((Mapper)rdd->fn)(data);
                    list_add(newpartition, transformed_data);
                }
            }

            pthread_mutex_lock(&rdd->lock);
            rdd->partitions[pnum] = newpartition;
            rdd->materialized_cnt += 1;
            pthread_mutex_unlock(&rdd->lock);
            break;
        }
        case FILTER:
        {
            RDD *dependancy = rdd->dependencies[0];
            int pnum = task->pnum;
            List *newpartition = list_init();

            // Start iterating
            List *oldpartition = dependancy->partitions[pnum];
            ListIter iter = list_get_iter(oldpartition);
            void *data;
            while ((data = iter_next(&iter)) != NULL)
            {
                if (((Filter)rdd->fn)(data, rdd->ctx))
                    list_add(newpartition, data);
            }

            pthread_mutex_lock(&rdd->lock);
            rdd->partitions[pnum] = newpartition;
            rdd->materialized_cnt += 1;
            pthread_mutex_unlock(&rdd->lock);
            break;
        }
        case PARTITIONBY:
        {
            RDD *dependency = rdd->dependencies[0];
            int pnum = task->pnum;
            List *newpartition = list_init();

            // Iterate through entire dependency
            for (int i = 0; i < dependency->partitions_cnt; i++)
            {
              List *oldpartition = dependency->partitions[i];
              ListIter iter = list_get_iter(oldpartition);
              void *data;
              while ((data = iter_next(&iter)) != NULL)
              {
                if (((Partitioner)rdd->fn)(data, rdd->partitions_cnt, rdd->ctx) == pnum)
                  list_add(newpartition, data);
              }
            }

            // Assign new partition to RDD
            pthread_mutex_lock(&rdd->lock);
            rdd->partitions[pnum] = newpartition;
            rdd->materialized_cnt += 1;
            pthread_mutex_unlock(&rdd->lock);
            break;
        }
        case JOIN:
        {
            // Be careful by creating partition and once it finishes assign it to RDD
            RDD *dependancy1 = rdd->dependencies[0];
            RDD *dependancy2 = rdd->dependencies[1];
            int pnum = task->pnum;
            List *newpartition = list_init();

            // Start iterating
            List *oldpartition1 = dependancy1->partitions[pnum];
            List *oldpartition2 = dependancy2->partitions[pnum];
            
            ListIter iter1 = list_get_iter(oldpartition1);
            void *data1;
            while ((data1 = iter_next(&iter1)) != NULL)
            {
              ListIter iter2 = list_get_iter(oldpartition2);
              void *data2;
              while ((data2 = iter_next(&iter2)) != NULL)
              {
                void *newelem;
                if ((newelem = ((Joiner)rdd->fn)(data1, data2, rdd->ctx)) != NULL)
                  list_add(newpartition, newelem);
              }
            }

            // Assign new partition to RDD
            pthread_mutex_lock(&rdd->lock);
            rdd->partitions[pnum] = newpartition;
            rdd->materialized_cnt += 1;
            pthread_mutex_unlock(&rdd->lock);
            break;
        }
        case FILE_BACKED:
        {
          return;
        }
    }
}

List* list_init()
{
    List *result = malloc(sizeof(List));
    result->num_items = 0;
    result->head = NULL;
    result->tail = NULL;
    return result;
}

bool list_add(List *list, void *data)
{
    if (list == NULL || data == NULL)
        return false;
    
    ListNode *new_node = malloc(sizeof(ListNode));
    new_node->data = data;

    if (list->head == NULL)
    {
        list->head = new_node;
        list->tail = new_node;
        new_node->next = NULL;
    }
    else
    {
        list->tail->next = new_node;
        list->tail = new_node;
        new_node->next = NULL;
    }
    list->num_items += 1;

    return true;
}

void* list_get(List *list, int indx)
{
    if (list == NULL)
        return NULL;
    
    if (indx < 0 || indx >= list->num_items)
        return NULL;
    
    ListNode *temp = list->head;
    for (int i = 0; i < indx; i++)
    {
        temp = temp->next;
    }
    return temp->data;
}

void list_free(List *list)
{
  if (list == NULL)
    return;

  ListNode *current = list->head;
  while (current)
  {
    ListNode *temp = current;
    current = current->next;
    free(temp->data);
    free(temp);
  } 
  free(list);
}

void list_node_free(List *list)
{
  if (list == NULL)
    return;

  ListNode *current = list->head;
  while (current)
  {
    ListNode *temp = current;
    current = current->next;
    free(temp);
  } 
  free(list);
}

ListIter list_get_iter(List *list)
{
    ListIter iter;
    iter.current = list->head;
    return iter;
}


void* iter_next(ListIter *iter)
{
    if (iter->current == NULL)
        return NULL;

    void *result = iter->current->data;
    iter->current = iter->current->next;
    return result;
}

TaskQueue* queue_init()
{
  TaskQueue *queue = malloc(sizeof(TaskQueue));
  queue->head = NULL;
  queue->tail = NULL;
  queue->num_tasks = 0;
  return queue;
}

bool queue_push(TaskQueue *queue, Task *task)
{
  if (queue == NULL || task == NULL)
    return false;
  
  ListNode *node = malloc(sizeof(ListNode));
  node->data = task;

  if (queue->head == NULL)
  {
    queue->head = node;
    queue->tail = node;
    node->next = NULL;
  }
  else
  {
    queue->tail->next = node;
    queue->tail = node;
    node->next = NULL;
  }
  queue->num_tasks += 1;

  return true;
}

Task* queue_pop(TaskQueue *queue)
{
  if (queue == NULL || queue->head == NULL)
    return NULL;

  // Update head node and free listnode associated metadata
  ListNode *node = queue->head;
  queue->head = queue->head->next;
  if (queue->head == NULL)
    queue->tail = NULL;

  queue->num_tasks -= 1;
  Task *result = (Task *)node->data;
  free(node); // TODO: destroy data
  return result;
}
