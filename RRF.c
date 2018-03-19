#include <stdio.h>
#include <sys/time.h>
#include <signal.h>
#include <stdlib.h>
#include <ucontext.h>
#include <unistd.h>

#include "mythread.h"
#include "interrupt.h"

#include "queue.h"

TCB* scheduler();
void activator();
void timer_interrupt(int sig);
void network_interrupt(int sig);

/* Array of state thread control blocks: the process allows a maximum of N threads */
static TCB t_state[N]; 

/* Current running thread */
static TCB* running;
static int current = 0; 

/* Variable indicating if the library is initialized (init == 1) or not (init == 0) */
static int init=0;

/*  Low priority queue for threads ready to be executed. */
struct queue *low_q;

/*  High priority queue for threads ready to be executed. */
struct queue *high_q;


/* Thread control block for the idle thread */
static TCB idle;
static void idle_function(){
  while(1);
}

/* Initialize the thread library */
void init_mythreadlib() {
  int i; 
  /* Initialize the queues for ready processes. */
  low_q = queue_new(); /* High priority. */
  high_q = queue_new(); /* Low priority. */

  /* Create context for the idle thread */
  if(getcontext(&idle.run_env) == -1){
    perror("*** ERROR: getcontext in init_thread_lib");
    exit(-1);
  }

  idle.state = IDLE;
  idle.priority = SYSTEM;
  idle.function = idle_function;
  idle.run_env.uc_stack.ss_sp = (void *)(malloc(STACKSIZE));
  idle.tid = -1;
  if(idle.run_env.uc_stack.ss_sp == NULL){
    printf("*** ERROR: thread failed to get stack space\n");
    exit(-1);
  }
  idle.run_env.uc_stack.ss_size = STACKSIZE;
  idle.run_env.uc_stack.ss_flags = 0;
  idle.ticks = QUANTUM_TICKS;
  makecontext(&idle.run_env, idle_function, 1); 

  t_state[0].state = INIT;
  t_state[0].priority = LOW_PRIORITY;
  t_state[0].ticks = QUANTUM_TICKS;
  if(getcontext(&t_state[0].run_env) == -1){
    perror("*** ERROR: getcontext in init_thread_lib");
    exit(5);
  } 

  for(i=1; i<N; i++){
    t_state[i].state = FREE;
  }
 
  t_state[0].tid = 0;
  running = &t_state[0];

  /* Initialize network and clock interrupts */
  init_network_interrupt();
  init_interrupt();
}

 
/* Create and intialize a new thread with body fun_addr and one integer argument */ 
int mythread_create (void (*fun_addr)(),int priority)
{
  int i;
  
  if (!init) { init_mythreadlib(); init=1;}
  for (i=0; i<N; i++)
    if (t_state[i].state == FREE) break;
  if (i == N) return(-1);
  if(getcontext(&t_state[i].run_env) == -1){
    perror("*** ERROR: getcontext in my_thread_create");
    exit(-1);
  }
  t_state[i].state = INIT;
  t_state[i].priority = priority;
  t_state[i].function = fun_addr;
  t_state[i].run_env.uc_stack.ss_sp = (void *)(malloc(STACKSIZE));
  if(t_state[i].run_env.uc_stack.ss_sp == NULL){
    printf("*** ERROR: thread failed to get stack space\n");
    exit(-1);
  }
  t_state[i].tid = i;
  t_state[i].run_env.uc_stack.ss_size = STACKSIZE;  
  t_state[i].run_env.uc_stack.ss_flags = 0;
  /* The initial quantum for any process is stated in the constant QUANTUM_TICKS */
  t_state[i].ticks = QUANTUM_TICKS;

  makecontext(&t_state[i].run_env, fun_addr, 1);  

  /* 
    CASE 1:
    The newly created thread is low priority.
    Therefore, we have just to insert it into the low priority queue.
  */
  if(priority == LOW_PRIORITY){

    disable_interrupt();
    /* Insert the thread in the queue */
    enqueue(low_q,&t_state[i]); 
    enable_interrupt();
  } 
  /* 
    CASE 2
    The newly created thread is high priority.
    In here we can distinguish two cases:
    - 2.1 There is a low priority thread running (and therefore, no high priority
    threads are available for execution), so you have to preempt it from the CPU 
    and put this new thread in execution.
    - 2.2 There is a high priority thread running, so you only 
    have to enqueue it into the high priority ready queue.
  */

  /* -- CASE 2.1 -- */
  else if(t_state[current].priority == LOW_PRIORITY){

      //int old = current; /* Get current thread ID. */
      //current = i; /* Set new current ID to the one of the newly created thread. */
      
      /* Set the remaining ticks for the next execution to the ones of the quantum slice. */
      t_state[current].ticks = QUANTUM_TICKS;
      disable_interrupt();
      /* Equeue the current process so that it can be executed again. */
      enqueue(low_q,&t_state[current]);
      enable_interrupt();

      printf("*** THREAD %d PREEMPTED: SET CONTEXT OF %d\n", t_state[current].tid, t_state[i].tid);
      activator(&t_state[i]);
  } 
  /* -- CASE 2.2 -- */
  else{
    
    disable_interrupt();
    /* Enqueue current thread into the ready queue. */
    enqueue(high_q, &t_state[i]);
    enable_interrupt();

  }

  return i;
} /****** End my_thread_create() ******/

/* Read network syscall */
int read_network()
{
  return 1;
}

/* Network interrupt  */
void network_interrupt(int sig) 
{
} 


/* Free terminated thread and exits */
void mythread_exit() {
  int tid = mythread_gettid();  

  printf("*** THREAD %d FINISHED\n", tid);  
  t_state[tid].state = FREE;
  free(t_state[tid].run_env.uc_stack.ss_sp); 

  TCB* next = scheduler();
  /* 
    activator() arguments have changed, so we have to include the current thread on the first 
    argument for swapping between current and next process (stated by the scheduler).
  */
  printf("*** THREAD %i FINISHED : SET CONTEXT OF %i\n", t_state[tid].tid, next->tid);
  activator(next);
}

/* Sets the priority of the calling thread */
void mythread_setpriority(int priority) {
  int tid = mythread_gettid();  
  t_state[tid].priority = priority;
}

/* Returns the priority of the calling thread */
int mythread_getpriority(int priority) {
  int tid = mythread_gettid();  
  return t_state[tid].priority;
}


/* Get the current thread id.  */
int mythread_gettid(){
  if (!init) { init_mythreadlib(); init=1;}
  return current;
}


TCB* scheduler(){
  /* We first get the id of the current thread, so that we can work with it */
  int tid = mythread_gettid();

  /* 
    CASE 1: Thread finished execution.

    We execute this condition inside the scheduler when the thread calls
    mythread_exit(), which occurs when the thread has finished its execution.
    In those cases, mythread_exit() is responsible for changing its status
    from INIT (in execution/ready) to FREE and for freeing memory. Since FREE state
    is only adquired in this situation, we have only to check it.
  */
  if(t_state[tid].state == FREE){

    /*
      Since high priority queues are executed first, we have to check first
      if the high priority queue is not empty. This also allows us to avoid
      segmentation fault error.
    */
    if(!queue_empty(high_q)){

      disable_interrupt();
      /* Get the next high priority thread to be executed. */
      TCB *s = dequeue(high_q);
      enable_interrupt();
      /* Return next thread to be executed. */
      return s;

    }
    /*
      This case will only be reached if there was no high priority threads available
      for execution. Only in that case low priority threads can be passed to CPU. Thus,
      we have to check first if there are low priority queues available. This also allows 
      us to avoid segmentation fault error.
    */
      else if(!queue_empty(low_q)){
      disable_interrupt();
      /* Get the next low priority thread to be executed. */
      TCB *s = dequeue(low_q);
      enable_interrupt();
      /* Return next thread to be executed. */
      return s;
    }
    /* Otherwise, there are not high nor low threads ready to be executed. */
    else{
      printf("FINISH\n"); 
      /* Leave the program. */
      exit(1);
    }
  }

  /*
    CASE 2: Clock interrupt.

    We execute this condition inside the scheduler when a clock interruption
    happens, from timer_interrupt(); only if the current thread in execution is
    low priority, since high priority queues are executed using FIFO policy.

    Thus, we change the current low priority thread to the first thread from
    the low priority queue.
  */
  else{

    /* Check if there are low priority threads available. */
    if(!queue_empty(low_q)){
      disable_interrupt();
      /* Get the next thread to be executed. */
      TCB *s = dequeue(low_q);
      enable_interrupt();

      /* Set the remaining ticks for the next execution to the ones of the quantum slice. */
      t_state[tid].ticks = QUANTUM_TICKS;
      disable_interrupt();
      /* Equeue the current process so that it can be executed again. */
      enqueue(low_q,&t_state[tid]);
      enable_interrupt();
      /* Return next thread to be executed. */
      return s;
    }
    else{
      /* 
        If the queue is empty, but the scheduler has been called due to quantum
        slide to have been finished, just reset the quantum slices and return the 
        current thread.
      */
      t_state[tid].ticks = QUANTUM_TICKS;
      return &t_state[tid];
    }
  }
  
}


/* Timer interrupt  */
void timer_interrupt(int sig)
{

  /* Get current thread ID. */
  int tid = mythread_gettid();  

  /* 
    Since only low-level threads use Round Robin, we can only call the
    scheduler if the priority of the current running thread is low.
  */
  if (t_state[tid].priority == LOW_PRIORITY){
    /* Update the ticks. */
     t_state[tid].ticks -= 1;

    /* In case the quantum was consumed. */
    if(t_state[tid].ticks == 0){
      /* Get next thread to be executed. */
      TCB* next = scheduler();

      /* 
        In case we have changed the thread to be executed in the next tick,
        we perform a context switch.
      */
      if(t_state[tid].tid != next->tid){
        /* Change current thread context to new thread context. */
        printf("*** SWAPCONTEXT FROM %i to %i\n",t_state[tid].tid,next->tid);
        activator(next);
      }
    }
  }
} 

/* Activator */
void activator(TCB* next){
  /* Execute context switch. */
  int old_id = current;
  current = next->tid;
  /* New current thread ID is the one we have just extracted from queue. */
  if(t_state[old_id].state == FREE){ 

    setcontext (&(next->run_env));  
    printf("mythread_free: After setcontext, should never get here!!...\n");  
  }
  else  swapcontext (&(t_state[old_id].run_env),&(next->run_env));
  
 
}




