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

/* Waiting queue for threads calling NIC driver. */
struct queue *wait_q;


/* Thread control block for the idle thread */
static TCB idle;
static void idle_function(){
  while(1);
}

/* Initialize the thread library */
void init_mythreadlib() {
  int i; 
  /* Initialize all the queues. */
  low_q = queue_new(); /* Low priority queue for ready processes. */
  high_q = queue_new(); /* High priority queue for ready processes. */
  wait_q = queue_new(); /* Wait queue for waiting processes. */

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
    if(enqueue(low_q,&t_state[i])==NULL){
      perror("*** ERROR: enqueue in my_thread_create");
      exit(-1);
    }
    enable_interrupt();
  } 
  /* 
    CASE 2
    The newly created thread is high priority.
    In here we can distinguish two cases:
    - 2.1 There is a low priority thread running (and therefore, no high priority
    threads are available for execution), so you have to preempt it from the CPU 
    and put this new thread in execution.
    - 2.2 There is a high priority thread running, so we only 
    have to enqueue it into the high priority ready queue.
  */

  /* -- CASE 2.1 -- */
  else if(t_state[current].priority == LOW_PRIORITY){
      
      /* Set the remaining ticks for the next execution to the ones of the quantum slice. */
      t_state[current].ticks = QUANTUM_TICKS;
      disable_interrupt();
      /* Equeue the current process so that it can be executed again. */
      if(enqueue(low_q,&t_state[current])==NULL){
        perror("*** ERROR: enqueue in my_thread_create");
        exit(-1);
      }
      enable_interrupt();

      printf("*** THREAD %d PREEMPTED: SET CONTEXT OF %d\n", t_state[current].tid, t_state[i].tid);
      activator(&t_state[i]);
  } 
  /* -- CASE 2.2 -- */
  else{
    
    disable_interrupt();
    /* Enqueue current thread into the ready queue. */
    if(enqueue(high_q, &t_state[i])==NULL){
      perror("*** ERROR: enqueue in my_thread_create");
      exit(-1);
    }
    enable_interrupt();


  } 
  
  return i;
} /****** End my_thread_create() ******/

/* Read network syscall */
int read_network()
{
  /*
	If the current thread reads from network, it must be preempted, so that
	it is enqueued in the waiting queue, waiting for a network interrupt to
	be ready.
  */
  int tid = mythread_gettid(); /* Get current thread ID. */
  t_state[tid].state = WAITING; /* Change state from INIT to WAITING. */
  printf("*** THREAD %i READ FROM NETWORK\n",t_state[tid].tid);

  /* Get next thread to be executed. */
  TCB * s = scheduler();
  activator(s);

  return 1;
}

/* Network interrupt  */
void network_interrupt(int sig)       
{
  /* 
    When a network interrupt comes, it is only effective
    if there are threads on the waiting queue.
  */
  if(!queue_empty(wait_q)){

    disable_interrupt();
    /* Get the next thread to be woken up. */
    TCB *s = dequeue(wait_q);
    if(s == NULL){
      perror("*** ERROR: dequeue in network_interrupt");
      exit(-1);
    }
    enable_interrupt();
    s->state = INIT;

    printf("*** THREAD %d READY\n",s->tid);

    /* CASE LOW PRIORITY  */
    if(s->priority == LOW_PRIORITY){
      /* Enqueue into low priority queue. */
      disable_interrupt();
      if(enqueue(low_q,s)==NULL){
        perror("*** ERROR: enqueue in network_interrupt");
        exit(-1);
      }
      enable_interrupt();
    }
    /* CASE HIGH PRIORITY */
    else{

      /* Case another high priority thread is executing. */
      if(t_state[current].priority == HIGH_PRIORITY){
        /* Enqueue into high priority queue. */
        disable_interrupt();
        if(enqueue(high_q,s)==NULL){
          perror("*** ERROR: enqueue in network_interrupt");
          exit(-1);
        }
        enable_interrupt();
      }
      /* Case a low priority queu is executing: preemption. */
      else{
        
        	if(current != -1){ /* Not idle executing. */
            printf("*** THREAD %d PREEMPTED: SET CONTEXT OF %d\n", current,s->tid);
        	  t_state[current].ticks = QUANTUM_TICKS;
        	  /* Set the remaining ticks for the next execution to the ones of the quantum slice. */
         	  disable_interrupt();
            /* Equeue the current process so that it can be executed again. */
            if(enqueue(low_q,&t_state[current])==NULL){
              perror("*** ERROR: enqueue in network_interrupt");
              exit(-1);
            }
            enable_interrupt();
          }
      	
        activator(s);
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


/* FIFO para alta prioridad, RR para baja*/
TCB* scheduler(){
  /* We first get the id of the current thread, so that we can work with it */
  int tid = mythread_gettid();

  /* 
    FIRST CASE: Thread finished execution / idle.

    We execute this condition inside the scheduler when the thread calls
    mythread_exit(), which occurs when the thread has finished its execution.
    In those cases, mythread_exit() is responsible for changing its status
    from INIT (in execution/ready) to FREE and for freeing memory. Since FREE state
    is only adquired in this situation, we have only to check it.
  */
  if(t_state[tid].state == FREE || tid == -1){

    /* Check if there are high priority threads available. */
    if(!queue_empty(high_q)){

      disable_interrupt();
      /* Get the next thread to be executed. */
      TCB *s = dequeue(high_q);
      if(s == NULL){
        perror("*** ERROR: dequeue in scheduler");
        exit(-1);
      }
      enable_interrupt();
      /* Return next thread to be executed. */
      return s;

    }

    /* Check if there are low priority threads available. */
    else if(!queue_empty(low_q)){
      disable_interrupt();
      /* Get the next thread to be executed. */
      TCB *s = dequeue(low_q);
      if(s == NULL){
        perror("*** ERROR: dequeue in scheduler");
        exit(-1);
      }
      enable_interrupt();
      /* Return next thread to be executed. */
      return s;
    }
    /* No high nor low available, but some threds waiting -> idle */
    else if(!queue_empty(wait_q)) return &idle;

    /* Otherwise, there are no threads waiting to be executed. */
    else{
      printf("FINISH\n"); 
      /* Leave the program. */
      exit(1);
    }
  }

  /* 
    SECOND CASE: Current process has changed to waiting state
    (network call)
  */
  else if(t_state[tid].state == WAITING){

    TCB * next;

    /* Check if there are high priority threads available. */
    if(!queue_empty(high_q)){

      disable_interrupt();
      /* Get the next thread to be executed. */
      next = dequeue(high_q);
      if(next == NULL){
        perror("*** ERROR: dequeue in scheduler");
        exit(-1);
      }
      enable_interrupt();

    }

    /* Check if there are low priority threads available. */
    else if(!queue_empty(low_q)){
      disable_interrupt();
      /* Get the next thread to be executed. */
      next = dequeue(low_q);
      if(next == NULL){
        perror("*** ERROR: dequeue in scheduler");
        exit(-1);
      }
      enable_interrupt();
      /* New current thread ID is the one we have just extracted from queue. */
      t_state[tid].ticks = QUANTUM_TICKS;

    }

    /* No more processes ready (all waiting) -> idle */
    else next = &idle; 

    disable_interrupt();
    if(enqueue(wait_q, &t_state[tid])==NULL){
      perror("*** ERROR: enqueue in scheduler");
      exit(-1);
    }
    enable_interrupt();

    return next;

  }

  /*
    THIRD CASE: Clock interrupt.

    We execute this condition inside the scheduler when a clock interruption
    happens, in case all the ticks were consumed; only if the current thread 
    in execution is low priority, since high priority queues are executed using 
    FIFO policy.

    Thus, we change the current low priority thread to the first thread from
    the low priority queue.
  */

  else{
    
    /* Check if there are low priority threads available. */
    if(!queue_empty(low_q)){
      disable_interrupt();
      /* Get the next thread to be executed. */
      TCB *s = dequeue(low_q);
      if(s == NULL){
        perror("*** ERROR: dequeue in scheduler");
        exit(-1);
      }
      enable_interrupt();

      /* Set the remaining ticks for the next execution to the ones of the quantum slice. */
      t_state[tid].ticks = QUANTUM_TICKS;
      disable_interrupt();
      /* Equeue the current process so that it can be executed again. */
      if(enqueue(low_q,&t_state[tid])==NULL){
        perror("*** ERROR: enqueue in scheduler");
        exit(-1);
      }
      enable_interrupt();
      /* Return next thread to be executed. */
      return s;
    }
    /* Otherwise, only the current thread is available. */
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

  if(tid == -1){ /* idle process */
    TCB* next = scheduler(); /* Get next thread*/

      /* 
       In case we have changed the thread to be executed in the next tick,
        we perform a context switch.
      */
      if(next->tid != -1){
        /* Change current thread context to new thread context. */
        printf("*** SWAPCONTEXT FROM %i to %i\n",tid,next->tid);
        activator(next);
      }

  }
  /* Case low priority, since they are the only ones using Round Robin. */
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
  if(old_id == -1){
    /* Change current thread context to new thread context. */
    printf("*** SWAPCONTEXT FROM %i to %i\n",old_id,current);
    if(swapcontext (&(idle.run_env),&(next->run_env))){
      perror("*** ERROR: swapcontext in activator");
      exit(-1);
    }
  }
  else if(t_state[old_id].state == FREE){ 
    /* Set context to new thread given by scheduler. */
    printf("*** THREAD %d FINISHED : SET CONTEXT OF %d\n",old_id, current);
    if(setcontext (&(next->run_env))){
      perror("*** ERROR: setcontext in activator");
      exit(-1);
    }  
    printf("mythread_free: After setcontext, should never get here!!...\n");  
  }
  else  swapcontext (&(t_state[old_id].run_env),&(next->run_env));
 
}








