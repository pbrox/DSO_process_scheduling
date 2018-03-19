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

/*  Low priority queue for threads ready to be executed */
struct queue *low_q;

/*  High priority queue for threads ready to be executed */
struct queue *high_q;

/* Waiting queue for threads calling NIC driver */
struct queue *wait_q;


/* Thread control block for the idle thread */
static TCB idle;
static void idle_function(){
  while(1);
}

/* Initialize the thread library */
void init_mythreadlib() {
  int i; 
  /* Initialize the queues for ready processes */
  low_q = queue_new();
  high_q = queue_new();
  wait_q = queue_new();

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
  printf("Thread %d Priority %d\n", t_state[0].tid, t_state[0].priority);
  printf("*** THREAD %i READY\n",t_state[0].tid);
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

  printf("Thread %d Priority %d\n", t_state[i].tid, priority);

  /* If it is Low priority, Insert the newly created thread into the low priority queue */
  if(priority == LOW_PRIORITY){

    disable_interrupt();
    enqueue(low_q,&t_state[i]); 
    enable_interrupt();
  } 
  /* If it is high priority and there is a low priority running */
  else if(t_state[current].priority == LOW_PRIORITY){

      int old = current;
      current = i; 
      
      /* Set the remaining ticks for the next execution to the ones of the quantum slice. */
      t_state[old].ticks = QUANTUM_TICKS;
      disable_interrupt();
      /* Equeue the current process so that it can be executed again. */
      enqueue(low_q,&t_state[old]);
      enable_interrupt(); 

      printf("*** THREAD %d PREEMPTED: SET CONTEXT OF %d\n", t_state[old].tid, t_state[i].tid);
      //Activate the new thread
      activator(&t_state[old],&t_state[current]);
  } 

  else{
    
    disable_interrupt();
    enqueue(high_q, &t_state[i]);
    enable_interrupt();

  } 
  
  return i;
} /****** End my_thread_create() ******/

/* Read network syscall */
int read_network()
{
  int tid = mythread_gettid();
  t_state[tid].state = WAITING;
  printf("*** THREAD %i READ FROM NETWORK\n",t_state[tid].tid);
  TCB * s = scheduler();
  activator(&t_state[tid], s);

  return 1;
}

/* Network interrupt  */
void network_interrupt(int sig)       
{
  if(!queue_empty(wait_q)){

    disable_interrupt();
    /* Get the next thread to be wake up. */
    TCB *s = dequeue(wait_q);
    enable_interrupt();

    s->state = INIT;

    //Now depending on priority

    if(s->priority == LOW_PRIORITY){
      //If it is low priority just insert inside its queue
      disable_interrupt();
      enqueue(low_q,s);
      enable_interrupt();
    }
    else{

      //If it is hihg priority and there is currently another hp
      if(t_state[current].priority == HIGH_PRIORITY){
        disable_interrupt();
        enqueue(high_q,s);
        enable_interrupt();
      }

      else{
        //In other case we have to make preemption
        int old = current;
        current = s->tid;
      
        /* Set the remaining ticks for the next execution to the ones of the quantum slice. */
        t_state[old].ticks = QUANTUM_TICKS;
        disable_interrupt();
        /* Equeue the current process so that it can be executed again. */
        enqueue(low_q,&t_state[old]);
        enable_interrupt();

        //printf("*** THREAD %d PREEMPTED: SET CONTEXT OF %d\n", t_state[old].tid, t_state[i].tid);
        //Activate the new thread
        activator(&t_state[old],&t_state[current]);
      }
    } 
  }
} 


/* Free terminated thread and exits */
void mythread_exit() {
  int tid = mythread_gettid();  

  printf("*** THREAD %d FINISHED\n", tid);  
  t_state[tid].state = FREE;
  free(t_state[tid].run_env.uc_stack.ss_sp); 

  TCB* next = scheduler();
  /* 
    activator arguments have changed, so we have to include the current thread on the first 
    argument for swapping between current and next process (stated by the scheduler).
  */
  //printf("*** THREAD %i FINISHED : SET CONTEXT OF %i\n", t_state[tid].tid, next)->tid;
  activator(&t_state[tid], next);
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


/* FIFO para alta prioridad, RR para baja*/
TCB* scheduler(){
  /* We first get the id of the current thread, so that we can work with it */
  int tid = mythread_gettid();

  /* 
    FIRST CASE: Thread finished execution.

    We execute this condition inside the scheduler when the thread calls
    mythread_exit(), which occurs when the thread has finished its execution.
    In those cases, mythread_exit() is responsible for changing its status
    from INIT (in execution/ready) to FREE and for freeing memory. Since FREE state
    is only adquired in this situation, we have only to check it.
  */
  if(t_state[tid].state == FREE || t_state[tid].state == IDLE ){

    if(!queue_empty(high_q)){

      disable_interrupt();
      /* Get the next thread to be executed. */
      TCB *s = dequeue(high_q);
      enable_interrupt();
      /* New current thread ID is the one we have just extracted from queue. */
      current = s->tid;
      /* Return next thread to be executed. */
      return s;

    }
    /* To avoid segementation fault error, we just check if the queue is not empty. */
    
    else if(!queue_empty(low_q)){
      disable_interrupt();
      /* Get the next thread to be executed. */
      TCB *s = dequeue(low_q);
      enable_interrupt();
      /* New current thread ID is the one we have just extracted from queue. */
      current = s->tid;
      /* Return next thread to be executed. */
      return s;
    }

    else if(!queue_empty(wait_q)){
      current = idle.tid;
      return &idle;
    } 

    /* Otherwise, there are no threads waiting to be executed. */
    else{
      printf("FINISH\n"); 
      /* Leave the program. */
      exit(1);
    }
  }

  //Este caso significa que lo vamos a meter en la cola de waiting

  else if(t_state[tid].state == WAITING){

    TCB * next;

    if(!queue_empty(high_q)){

      disable_interrupt();
      /* Get the next thread to be executed. */
      next = dequeue(high_q);
      enable_interrupt();
      /* New current thread ID is the one we have just extracted from queue. */
      current = next->tid;

    }
    /* To avoid segementation fault error, we just check if the queue is not empty. */
    
    else if(!queue_empty(low_q)){
      disable_interrupt();
      /* Get the next thread to be executed. */
      next = dequeue(low_q);
      enable_interrupt();
      /* New current thread ID is the one we have just extracted from queue. */
      current = next->tid;
      t_state[tid].ticks = QUANTUM_TICKS;

    }

    else{
      current = idle.tid;
      next = &idle;
    }

    disable_interrupt();
    enqueue(wait_q, &t_state[tid]);
    enable_interrupt();

    return next;

  }

  /*
    SECOND CASE: Clock interrupt.

    We execute this condition inside the scheduler when a clock interruption
    happens, from timer_interrupt(). That means that a tick has been 
    consumed from the previous call to the scheduler, and, therefore, the
    remaining ticks on the CPU for the current thread are reduced by one.
  */
  
  /* 
    If all the ticks were consumed, we have to preempt the current thread
    from the CPU and execute the next one in the waiting queue.
  */

  else{
    /* To avoid segementation fault error, we just check if the queue is not empty. */
    
    if(!queue_empty(low_q)){
      disable_interrupt();
      /* Get the next thread to be executed. */
      TCB *s = dequeue(low_q);
      enable_interrupt();
      /* New current thread ID is the one we have just extracted from queue. */
      current = s->tid;

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
  //printf("Thread %d Priority %d Ticks %d\n", t_state[tid].tid, t_state[tid].priority, t_state[tid].ticks);

  if(t_state[tid].state == IDLE){
    TCB* next = scheduler();

      /* 
       In case we have changed the thread to be executed in the next tick,
        we perform a context switch.
      */

      if(t_state[tid].tid != next->tid){
        /* Change current thread context to new thread context. */
        printf("*** SWAPCONTEXT FROM %i to %i\n",t_state[tid].tid,next->tid);
        activator(&t_state[tid], next);
      }

  }

  else if (t_state[tid].priority == LOW_PRIORITY){
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
        activator(&t_state[tid], next);
      }
    }
  }
} 

/* Activator */
void activator(TCB* old, TCB* next){
  /* Execute context switch. */
  swapcontext (&(old->run_env),&(next->run_env));
  //printf("mythread_free: After setcontext, should never get here!!...\n");  
} 



