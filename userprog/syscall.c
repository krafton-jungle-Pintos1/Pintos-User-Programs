#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

/** #Project 2: System Call **/
#include <string.h>

#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "userprog/process.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

/** #Project 2: System Call  **/
// 멀티스레드 환경에서 파일 시스템의 동시 접근을 제어 하는 lock
struct lock filesys_lock;

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081					/* Segment selector msr */
#define MSR_LSTAR 0xc0000082				/* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void)
{
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 |
													((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
						FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	/** #Project 2: System Call  **/
	// filesys_lock 초기화
	lock_init(&filesys_lock);
}

/** #Project 2: System Call **/
void syscall_handler(struct intr_frame *f UNUSED)
{
	// TODO: Your implementation goes here.
	int sys_number = f->R.rax;

	// Argument 순서: %rdi %rsi %rdx %r10 %r8 %r9

	switch (sys_number)
	{
	case SYS_HALT:
		halt();
		break;
	case SYS_EXIT:
		exit(f->R.rdi);
		break;
	case SYS_FORK:
		f->R.rax = fork(f->R.rdi);
		break;
	case SYS_EXEC:
		f->R.rax = exec(f->R.rdi);
		break;
	case SYS_WAIT:
		f->R.rax = process_wait(f->R.rdi);
		break;
	case SYS_CREATE:
		f->R.rax = create(f->R.rdi, f->R.rsi);
		break;
	case SYS_REMOVE:
		f->R.rax = remove(f->R.rdi);
		break;
	case SYS_OPEN:
		f->R.rax = open(f->R.rdi);
		break;
	case SYS_FILESIZE:
		f->R.rax = filesize(f->R.rdi);
		break;
	case SYS_READ:
		f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_WRITE:
		f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_SEEK:
		seek(f->R.rdi, f->R.rsi);
		break;
	case SYS_TELL:
		f->R.rax = tell(f->R.rdi);
		break;
	case SYS_CLOSE:
		close(f->R.rdi);
		break;
	default:
		exit(-1);
	}
}

/** #Project 2: System Call - check_address **/
// 주소가 유효한지 확인하는 함수
void check_address(void *addr)
{
	if (is_kernel_vaddr(addr) || addr == NULL || pml4_get_page(thread_current()->pml4, addr) == NULL)
		exit(-1);
}

/** #Project 2: System Call - halt **/
// Pintos를 강제 종료하는 시스템콜
void halt(void)
{
	power_off();
}

/** #Project 2: System Call - exit **/
// 호출된 프로세스를 강제 종료 시키는 시스템콜
void exit(int status)
{
	struct thread *curr = thread_current();
	curr->exit_status = status;

	/** #Project 2: Process Termination Messages */
	printf("%s: exit(%d)\n", curr->name, curr->exit_status);

	thread_exit();
}

/** #Project 2: System Call - exec **/
// 현재 프로세스를 cmd_line에서 호출한 새로운 실행 파일로 대체하는 시스템콜
int exec(const char *cmd_line)
{
	// 사용자 메모리 주소 검증
	check_address(cmd_line);

	// 명령어 문자열 크기 계산
	off_t size = strlen(cmd_line) + 1;

	// 명령어 문자열을 저장할 페이지 할당
	char *cmd_copy = palloc_get_page(PAL_ZERO);

	// 페이지 할당 실패 시 -1 반환
	if (cmd_copy == NULL)
		return -1;

	// 명령어 문자열 복사
	memcpy(cmd_copy, cmd_line, size);

	// 새 프로세스를 실행, 실패 시 -1 반환
	if (process_exec(cmd_copy) == -1)
		return -1;

	return 0;
}

/** #Project 2: System Call - wait **/
// tid로 지정된 자식 프로세스를 종료될 때까지 기다리고, 해당 자식의 종료 상태를 가져오는 시스템콜
int wait(pid_t tid)
{
	return process_wait(tid);
}

/** #Project 2: System Call - fork **/
// 부모 프로세스를 그대로 복제하여 새로운 자식 프로세스를 생성하는 시스템콜
pid_t fork(const char *thread_name)
{
	check_address(thread_name);

	return process_fork(thread_name, NULL);
}

/** #Project 2: System Call - create **/
// 새로운 파일을 생성하는 시스템콜
bool create(const char *file, unsigned initial_size)
{
	check_address(file);

	return filesys_create(file, initial_size);
}

/** #Project 2: System Call - remove **/
// 파일 시스템에서 지정된 이름의 파일을 삭제
bool remove(const char *file)
{
	check_address(file);

	return filesys_remove(file);
}

/** #Project 2: System Call - open **/
// 지정된 이름의 파일을 여는 시스템콜
int open(const char *file)
{
	check_address(file);
	struct file *newfile = filesys_open(file);

	if (newfile == NULL)
		return -1;

	int fd = process_add_file(newfile);

	if (fd == -1)
		file_close(newfile);

	return fd;
}

/** #Project 2: System Call - filesize **/
// 파일의 크기를 확인하는 시스템콜
int filesize(int fd)
{
	struct file *file = process_get_file(fd);

	if (file == NULL)
		return -1;

	return file_length(file);
}

/** #Project 2: System Call - read **/
// fd라는 파일 디스크립터를 가진 파일에서 length만큼 데이터를 읽어 buffer에 저장하는 함수
int read(int fd, void *buffer, unsigned length)
{
	check_address(buffer);

	// fd가 0인 경우(STDIN) keyboard로 직접 입력 받도록 함
	if (fd == 0)
	{
		int i = 0;
		char c;
		unsigned char *buf = buffer;

		for (; i < length; i++)
		{
			c = input_getc();
			*buf++ = c;
			if (c == '\0')
				break;
		}

		return i;
	}

	// fd가 1(STDOUT), 2(STDERR)인 경우 종료
	if (fd < 3)
		return -1;

	// fd가 3 이상인 경우
	struct file *file = process_get_file(fd);
	off_t bytes = -1;

	if (file == NULL)
		return -1;

	// 동시 접근을 제한하기 위해 Lock 설정
	lock_acquire(&filesys_lock);
	// 파일 내용 읽기
	bytes = file_read(file, buffer, length);
	// 읽기가 완료되면 Lock 해제
	lock_release(&filesys_lock);

	return bytes;
}

/** #Project 2: System Call - write **/
// fd라는 파일 디스크립터를 가진 파일에 buffer의 내용을 length만큼 파일에 작성
int write(int fd, const void *buffer, unsigned length)
{
	check_address(buffer);

	off_t bytes = -1;

	// fd가 0인 경우(STDIN) 종료
	if (fd <= 0)
		return -1;

	// fd가 1(STDOUT), 2(STDERR)인 경우 콘솔에 내용 출력
	if (fd < 3)
	{
		putbuf(buffer, length);
		return length;
	}

	// fd가 3 이상인 경우
	struct file *file = process_get_file(fd);

	if (file == NULL)
		return -1;

	// 동시 접근을 제한하기 위해 Lock 설정
	lock_acquire(&filesys_lock);
	// 파일에 내용 작성
	bytes = file_write(file, buffer, length);
	// 쓰기가 완료되면 Lock 해제
	lock_release(&filesys_lock);

	return bytes;
}

/** #Project 2: System Call - seek **/
// fd에서 다음 바이트 위치를 position(바이트 단위)으로 변경 하는 시스템콜
void seek(int fd, unsigned position)
{
	struct file *file = process_get_file(fd);

	if (fd < 3 || file == NULL)
		return;

	file_seek(file, position);
}

/** #Project 2: System Call - tell **/
// fd에서 다음 바이트 위치를 반환하는 시스템콜
int tell(int fd)
{
	struct file *file = process_get_file(fd);

	if (fd < 3 || file == NULL)
		return -1;

	return file_tell(file);
}

/** #Project 2: System Call - close **/
// 파일 디스크립터 fd를 닫는 함수
void close(int fd)
{
	struct file *file = process_get_file(fd);

	if (fd < 3 || file == NULL)
		return;

	process_close_file(fd);

	file_close(file);
}