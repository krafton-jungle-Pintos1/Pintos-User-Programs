#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
	Used to detect stack overflow.  See the big comment at the top
	 of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
	 Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
	 that are ready to run but not actually running. */
static struct list ready_list;

/* sleep 상태의 쓰레드들로만 이루어진 리스트 */
static struct list sleep_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;	 /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;	 /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4					/* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
	If true, use multi-level feedback queue scheduler.
	 Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *)(pg_round_down(rrsp())))

// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff};

/* Initializes the threading system by transforming the code
	that's currently running into a thread.  This can't work in
	general and it is possible in this case only because loader.S
	was careful to put the bottom of the stack at a page boundary.

	Also initializes the run queue and the tid lock.

	After calling this function, be sure to initialize the page
	allocator before trying to create any threads with
	thread_create().

	It is not safe to call thread_current() until this function
	 finishes. */
void thread_init(void)
/*
- 쓰레드 시스템의 초기설정을 담당
- ready_list와 destruction_req 리스트 초기화
- initial_thread(최초 실행 쓰레드) 설정
- tid_lock 초기화
*/
{
	ASSERT(intr_get_level() == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
			.size = sizeof(gdt) - 1,
			.address = (uint64_t)gdt};
	lgdt(&gdt_ds);

	/* Init the globla thread context */
	lock_init(&tid_lock);
	list_init(&ready_list);
	list_init(&destruction_req);
	list_init(&sleep_list);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread();
	init_thread(initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling by enabling interrupts.
	 Also creates the idle thread. */
void thread_start(void)
/*
- idle 쓰레드 생성
- 선점형 쓰레드 스케줄링 시작을 위해 인터럽트 활성화
*/
{
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init(&idle_started, 0);
	thread_create("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
	 Thus, this function runs in an external interrupt context. */
void thread_tick(void)
/*
- 타이머 인터럽트마다 호출
- 쓰레드 통계 업데이트
- TIME_SLICE 초과 시 선점 스케줄링 수행
*/
{
	struct thread *t = thread_current();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void)
{
	printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
				 idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
	PRIORITY, which executes FUNCTION passing AUX as the argument,
	and adds it to the ready queue.  Returns the thread identifier
	for the new thread, or TID_ERROR if creation fails.

	If thread_start() has been called, then the new thread may be
	scheduled before thread_create() returns.  It could even exit
	before thread_create() returns.  Contrariwise, the original
	thread may run for any amount of time before the new thread is
	scheduled.  Use a semaphore or some other form of
	synchronization if you need to ensure ordering.

	The code provided sets the new thread's `priority' member to
	PRIORITY, but no actual priority scheduling is implemented.
	 Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(const char *name, int priority,
										thread_func *function, void *aux)
/*
- 새로운 커널 쓰레드 생성
- 쓰레드 구조체 할당 및 초기화
- 쓰레드를 ready 상태로 변경
*/
{
	struct thread *t;
	tid_t tid;

	ASSERT(function != NULL);

	/* Allocate thread. */
	t = palloc_get_page(PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread(t, name, priority);
	tid = t->tid = allocate_tid();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t)kernel_thread;
	t->tf.R.rdi = (uint64_t)function;
	t->tf.R.rsi = (uint64_t)aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock(t);
	thread_test_max_priority();
	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
	again until awoken by thread_unblock().

	This function must be called with interrupts turned off.  It
	is usually a better idea to use one of the synchronization
	 primitives in synch.h. */
void thread_block(void)
/*
- 현재 쓰레드를 BLOCKED 상태로 변경
- schedule() 호출하여 다른 쓰레드로 전환
*/
{
	ASSERT(!intr_context());
	ASSERT(intr_get_level() == INTR_OFF);
	thread_current()->status = THREAD_BLOCKED;
	schedule();
}

/* Transitions a blocked thread T to the ready-to-run state.
	This is an error if T is not blocked.  (Use thread_yield() to
	make the running thread ready.)

	This function does not preempt the running thread.  This can
	be important: if the caller had disabled interrupts itself,
	it may expect that it can atomically unblock a thread and
	 update other data. */
void thread_unblock(struct thread *t)
/*
- BLOCKED 상태의 쓰레드를 READY 상태로 전환
- ready_list에 쓰레드 추가
*/
{
	enum intr_level old_level;

	ASSERT(is_thread(t));

	old_level = intr_disable();
	ASSERT(t->status == THREAD_BLOCKED);
	list_insert_ordered(&ready_list, &t->elem, thread_compare_priority, 0);
	t->status = THREAD_READY;
	intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name(void)
{
	return thread_current()->name;
}

/* Returns the running thread.
	This is running_thread() plus a couple of sanity checks.
	 See the big comment at the top of thread.h for details. */
struct thread *
thread_current(void)
{
	struct thread *t = running_thread();

	/* Make sure T is really a thread.
		If either of these assertions fire, then your thread may
		have overflowed its stack.  Each thread has less than 4 kB
		of stack, so a few big automatic arrays or moderate
		recursion can cause stack overflow. */
	ASSERT(is_thread(t));
	ASSERT(t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void)
{
	return thread_current()->tid;
}

/* Deschedules the current thread and destroys it.  Never
	 returns to the caller. */
void thread_exit(void)
/*
- 현재 쓰레드 종료
- DYING 상태로 전환하고 schedule() 호출
*/
{
	ASSERT(!intr_context());

#ifdef USERPROG
	process_exit();
#endif

	/* Just set our status to dying and schedule another process.
		 We will be destroyed during the call to schedule_tail(). */
	intr_disable();
	do_schedule(THREAD_DYING);
	NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
	 may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void)
/*
- 현재 쓰레드가 자발적으로 CPU 양보
- 현재 쓰레드를 ready_list에 추가하고 다른 쓰레드로 전환
*/
{
	struct thread *cur = thread_current();
	enum intr_level old_level;

	ASSERT(!intr_context());

	old_level = intr_disable();
	if (cur != idle_thread)
		list_insert_ordered(&ready_list, &cur->elem, thread_compare_priority, 0);

	do_schedule(THREAD_READY);
	intr_set_level(old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority)
/*
- 현재 쓰레드의 우선순위 변경
*/
{
	struct thread *cur = thread_current();
	cur->init_priority = new_priority;
	refresh_priority();
	thread_test_max_priority();
}

/* Returns the current thread's priority. */
int thread_get_priority(void)
/*
- 현재 쓰레드의 우선순위 반환
*/
{
	return thread_current()->priority;
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED)
{
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int thread_get_nice(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

/* 일어날 시간을 저장한 다음에 재워야 할 쓰레드를 재우는 함수
쓰레드의 상태를 block state로 만들어준다
*/
void thread_sleep(int64_t ticks)
/*
- 현재 쓰레드의 깨어날 시간(wakeup) 설정
- 쓰레드를 sleep_list에 추가
- 쓰레드 상태를 BLOCKED로 변경
*/
{
	struct thread *cur;
	enum intr_level old_level;

	old_level = intr_disable(); // 인터럽트 off
	cur = thread_current();			// 현재 실행 중인 쓰레드 포인터 획득

	ASSERT(cur != idle_thread) // idle 쓰레드가 아닌지 확인 (idle 쓰레드는 sleep하면 안됨)

	cur->wakeup = ticks;										 // 일어날 시간을 저장
	list_push_back(&sleep_list, &cur->elem); // sleep_list에 추가
	thread_block();													 // block 상태로 변경

	intr_set_level(old_level); // 인터럽트 on
}

/*
깨어날 쓰레드를 찾으면 sleep_list에서 제거
쓰레드의 상태를 ready state로 만들어준다
 */
void thread_awake(int64_t ticks)
/*
- sleep_list의 모든 쓰레드를 순회하며 검사
- 깨어날 시간이 된 쓰레드를 찾으면 sleep_list에서 제거
- 해당 쓰레드를 READY 상태로 변경(thread_unblock 호출)
- 아직 깨어날 시간이 안 된 쓰레드는 계속 sleep_list에 유지
*/
{
	struct list_elem *e = list_begin(&sleep_list); // sleep_list의 첫 번째 원소를 가리키는 포인터 획득

	while (e != list_end(&sleep_list)) // 리스트의 끝에 도달할 때까지 반복
	{
		struct thread *t = list_entry(e, struct thread, elem); // elem의 주소로부터 thread 구조체의 시작 주소를 계산하여 전체 thread 구조체에 접근할 수 있게 해준다

		if (t->wakeup <= ticks) // 스레드가 일어날 시간이 되었는지 확인
		{
			e = list_remove(e); // sleep list 에서 제거
			thread_unblock(t);	// 스레드 상태 변경
		}
		else
			e = list_next(e); // 다음 원소로 이동
	}
}

/*
내림차순으로 정렬
새로운 쓰레드의 우선순위가 ready_list에 있는 리스트의 쓰레드 priority(우선순위)보다 높으면 삽입
 */
bool thread_compare_priority(struct list_elem *a, struct list_elem *b, void *aux UNUSED)
{
	struct thread *ta = list_entry(a, struct thread, elem);
	struct thread *tb = list_entry(b, struct thread, elem);

	if (list_empty(ta) || list_empty(tb))
		return false;

	return ta->priority > tb->priority;
}

/*
running thread(현재 동작하는 쓰레드)와 ready_list 의 가장 앞의 쓰레드의 priority(우선순위)를 비교
현재 동작하는 쓰레드의 우선순위가 더 낮으면 양보하기
*/
void thread_test_max_priority(void)
{
	// ready_list가 비어있으면 선점 검사할 필요 없음
	if (list_empty(&ready_list))
		return;

	// 현재 실행중인 스레드와 ready_list의 최상위 스레드의 우선순위 비교를 위한 변수
	struct thread *cur = thread_current();
	struct thread *ready_front = list_entry(list_front(&ready_list), struct thread, elem);

	if (cur->priority < ready_front->priority)
		thread_yield();
}

void donate_priority()
{
	struct thread *t = thread_current();
	int priority = t->priority;

	for (int depth = 0; depth < 8; depth++)
	{
		if (t->wait_on_lock == NULL)
			break;

		t = t->wait_on_lock->holder;

		t->priority = priority;
	}
}

void remove_with_lock(struct lock *lock)
{
	struct thread *t = thread_current();
	struct list_elem *cur = list_begin(&t->donations);
	struct thread *cur_thread = NULL;

	while (cur != list_end(&t->donations))
	{
		cur_thread = list_entry(cur, struct thread, donation_elem);

		if (cur_thread->wait_on_lock == lock)
			list_remove(&cur_thread->donation_elem);

		cur = list_next(cur);
	}
}

void refresh_priority(void)
{
	struct thread *t = thread_current();
	t->priority = t->init_priority;

	if (list_empty(&t->donations))
		return;

	list_sort(&t->donations, thread_compare_priority, NULL);

	struct list_elem *max_elem = list_front(&t->donations);
	struct thread *max_thread = list_entry(max_elem, struct thread, donation_elem);

	if (t->priority < max_thread->priority)
		t->priority = max_thread->priority;
}

/* Idle thread.  Executes when no other thread is ready to run.

	The idle thread is initially put on the ready list by
	thread_start().  It will be scheduled once initially, at which
	point it initializes idle_thread, "up"s the semaphore passed
	to it to enable thread_start() to continue, and immediately
	blocks.  After that, the idle thread never appears in the
	ready list.  It is returned by next_thread_to_run() as a
	 special case when the ready list is empty. */
static void idle(void *idle_started_ UNUSED)
{
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current();
	sema_up(idle_started);

	for (;;)
	{
		/* Let someone else run. */
		intr_disable();
		thread_block();

		/* Re-enable interrupts and wait for the next one.

			The `sti' instruction disables interrupts until the
			completion of the next instruction, so these two
			instructions are executed atomically.  This atomicity is
			important; otherwise, an interrupt could be handled
			between re-enabling interrupts and waiting for the next
			one to occur, wasting as much as one clock tick worth of
			time.

			See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
			 7.11.1 "HLT Instruction". */
		asm volatile("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
//  *function = kernel 이 실행할 함수, *aux = synchronization을 위한 세마포 등이 들어옴
static void kernel_thread(thread_func *function, void *aux)
{
	ASSERT(function != NULL);

	intr_enable(); /* The scheduler runs with interrupts off. */
	function(aux); /* Execute the thread function. */
	thread_exit(); /* If function() returns, kill the thread. */
}

/* Does basic initialization of T as a blocked thread named
	 NAME. */
static void
init_thread(struct thread *t, const char *name, int priority)
/*
- 쓰레드 기본 정보 초기화(이름, 우선순위, 스택 등)
- 초기 상태를 BLOCKED로 설정
*/
{
	ASSERT(t != NULL);
	ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT(name != NULL);

	memset(t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy(t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t)t + PGSIZE - sizeof(void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;

	t->init_priority = priority;
	t->wait_on_lock = NULL;
	list_init(&t->donations);
}

/* Chooses and returns the next thread to be scheduled.  Should
	return a thread from the run queue, unless the run queue is
	empty.  (If the running thread can continue running, then it
	will be in the run queue.)  If the run queue is empty, return
	idle_thread. */
static struct thread *
next_thread_to_run(void)
/*
- ready_list에서 다음 실행할 쓰레드 선택
- ready_list가 비어있으면 idle_thread 반환
*/
{
	if (list_empty(&ready_list))
		return idle_thread;
	else
		return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void do_iret(struct intr_frame *tf)
{
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g"((uint64_t)tf) : "memory");
}

/* Switching the thread by activating the new thread's page
	tables, and, if the previous thread is dying, destroying it.

	At this function's invocation, we just switched from thread
	PREV, the new thread is already running, and interrupts are
	still disabled.

	It's not safe to call printf() until the thread switch is
	complete.  In practice that means that printf()s should be
	 added at the end of the function. */
static void
thread_launch(struct thread *th)
/*
- 실제 쓰레드 컨텍스트 스위칭 수행
- 새 쓰레드의 실행 컨텍스트 복원
*/
{
	uint64_t tf_cur = (uint64_t)&running_thread()->tf;
	uint64_t tf = (uint64_t)&th->tf;
	ASSERT(intr_get_level() == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile(
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n" // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n" // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n" // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n" // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"	 // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g"(tf) : "memory");
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status)
{
	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(thread_current()->status == THREAD_RUNNING);
	while (!list_empty(&destruction_req))
	{
		struct thread *victim =
				list_entry(list_pop_front(&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current()->status = status;
	schedule();
}

static void
schedule(void)
/*
- 다음 실행할 쓰레드 선택 및 컨텍스트 스위칭
- 현재 실행 중인 쓰레드의 상태 변경
*/
{
	struct thread *curr = running_thread();			// 현재 실행 중인 스레드 정보
	struct thread *next = next_thread_to_run(); // ready_list에서 다음에 실행할 스레드 선택

	ASSERT(intr_get_level() == INTR_OFF);		// scheduling 도중에는 인터럽트가 발생하면 안 되기 때문에 비활성화 상태인지 확인한다.
	ASSERT(curr->status != THREAD_RUNNING); // CPU 소유권을 넘겨주기 전에 running 쓰레드는 그 상태를 running 외의 다른 상태로 바꾸어 주는 작업이 되어 있어야 하고 이를 학인하는 부분이다.
	ASSERT(is_thread(next));								// 다음 실행할 스레드가 유효한 스레드인지 확인ㅍ

	/* Mark us as running. */
	next->status = THREAD_RUNNING; // 다음 스레드의 상태를 RUNNING으로 변경

	/* Start new time slice. */
	thread_ticks = 0; // 새로운 타임 슬라이스 시작을 위해 틱 카운트 초기화

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate(next);
#endif

	if (curr != next) // 현재 스레드와 다음 스레드가 다른 경우
	{
		/* If the thread we switched from is dying, destroy its struct
			thread. This must happen late so that thread_exit() doesn't
			pull out the rug under itself.
			We just queuing the page free reqeust here because the page is
			currently used by the stack.
			The real destruction logic will be called at the beginning of the
			 schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread)
		{
			ASSERT(curr != next);
			list_push_back(&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch(next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid(void)
{
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire(&tid_lock);
	tid = next_tid++;
	lock_release(&tid_lock);

	return tid;
}
