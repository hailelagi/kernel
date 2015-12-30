/*
 * Copyright (c) 2010-2015, Stefan Lankes, RWTH Aachen University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the University nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <hermit/stdio.h>
#include <hermit/stdlib.h>
#include <hermit/string.h>
#include <hermit/tasks.h>
#include <hermit/errno.h>
#include <hermit/processor.h>
#include <hermit/memory.h>
#include <hermit/vma.h>
#include <hermit/rcce.h>
#include <asm/tss.h>
#include <asm/elf.h>
#include <asm/page.h>

#include <lwip/sockets.h>
#include <lwip/err.h>
#include <lwip/stats.h>

#define START_ADDRESS	0x40200000

/*
 * Note that linker symbols are not variables, they have no memory allocated for
 * maintaining a value, rather their address is their value.
 */
extern const void percore_start;
extern const void percore_end0;

extern uint64_t base;

static inline void enter_user_task(size_t ep, size_t stack)
{
	// don't interrupt the jump to user-level code
	irq_disable();

	asm volatile ("swapgs" ::: "memory");

	// the jump also enable interrupts
	jump_to_user_code(ep, stack);
}

static int thread_entry(void* arg, size_t ep)
{
	task_t* curr_task = per_core(current_task);
#if 0
	size_t addr, stack = 0;
	size_t flags;
	int64_t npages;
	size_t offset = DEFAULT_STACK_SIZE-16;

	//create user-level stack
	npages = DEFAULT_STACK_SIZE >> PAGE_BITS;
	if (DEFAULT_STACK_SIZE & (PAGE_SIZE-1))
		npages++;

	addr = get_pages(npages);
	if (BUILTIN_EXPECT(!addr, 0)) {
		kprintf("load_task: not enough memory!\n");
		return -ENOMEM;
	}

	stack = (1ULL << 34ULL) - curr_task->id*DEFAULT_STACK_SIZE-PAGE_SIZE;	// virtual address of the stack
	flags = PG_USER|PG_RW;
	if (has_nx())
		flags |= PG_XD;

	if (page_map(stack, addr, npages, flags)) {
		put_pages(addr, npages);
		kprintf("Could not map stack at 0x%x\n", stack);
		return -ENOMEM;
	}
	memset((void*) stack, 0x00, npages*PAGE_SIZE);
	//kprintf("stack located at 0x%zx (0x%zx)\n", stack, addr);

	// create vma regions for the user-level stack
	flags = VMA_CACHEABLE|VMA_USER|VMA_READ|VMA_WRITE;
	vma_add(stack, stack+npages*PAGE_SIZE-1, flags);
#endif

	//vma_dump();

	// do we have to create a TLS segement?
	if (curr_task->tls_addr && curr_task->tls_size) {
		char* tls_addr = NULL;

		tls_addr = kmalloc(curr_task->tls_size);
		// copy default TLS segment to stack
		memcpy((void*) (tls_addr), (void*) curr_task->tls_addr, curr_task->tls_size);

		// set fs register to the TLS segment
		writefs((size_t) tls_addr);
		kprintf("Task %d set fs to %p\n", curr_task->id, tls_addr);
	} else writefs(0); // no TLS => clear fs register

	// set first argument
	//asm volatile ("mov %0, %%rdi" :: "r"(arg));
	//enter_user_task(ep, stack+offset);

	entry_point_t call_ep = (entry_point_t) ep;
	call_ep(arg);

	return 0;
}

size_t* get_current_stack(void)
{
	task_t* curr_task = per_core(current_task);
	size_t stptr = (size_t) curr_task->stack + KERNEL_STACK_SIZE - 0x10;

	set_per_core(kernel_stack, stptr);
	tss_set_rsp0(stptr);

	// do we change the address space?
	if (read_cr3() != curr_task->page_map)
		write_cr3(curr_task->page_map); // use new page table

	return curr_task->last_stack_pointer;
}

int create_default_frame(task_t* task, entry_point_t ep, void* arg, uint32_t core_id)
{
	size_t *stack;
	struct state *stptr;
	size_t state_size;

	if (BUILTIN_EXPECT(!task, 0))
		return -EINVAL;

	if (BUILTIN_EXPECT(!task->stack, 0))
		return -EINVAL;

	memset(task->stack, 0xCD, KERNEL_STACK_SIZE);

	/* The difference between setting up a task for SW-task-switching
	 * and not for HW-task-switching is setting up a stack and not a TSS.
	 * This is the stack which will be activated and popped off for iret later.
	 */
	stack = (size_t*) (((size_t) task->stack + KERNEL_STACK_SIZE - 0x10) & ~0xF);	// => stack is 16byte aligned

	/* Only marker for debugging purposes, ... */
	*stack-- = 0xDEADBEEF;

	/* and the "caller" we shall return to.
	 * This procedure cleans the task after exit. */
	*stack = (size_t) leave_kernel_task;

	/* Next bunch on the stack is the initial register state.
	 * The stack must look like the stack of a task which was
	 * scheduled away previously. */
	state_size = sizeof(struct state);
	stack = (size_t*) ((size_t) stack - state_size);

	stptr = (struct state *) stack;
	memset(stptr, 0x00, state_size);
	stptr->rsp = (size_t)stack + state_size;
	/* the first-function-to-be-called's arguments, ... */
	stptr->rdi = (size_t) arg;
	stptr->int_no = 0xB16B00B5;
	stptr->error =  0xC03DB4B3;

	/* The instruction pointer shall be set on the first function to be called
	   after IRETing */
	//if ((size_t) ep < KERNEL_SPACE) {
	//	stptr->rip = (size_t)ep;
	//} else {
		stptr->rip = (size_t)thread_entry;
		stptr->rsi = (size_t)ep; // use second argument to transfer the entry point
	//}
	stptr->cs = 0x08;
	stptr->ss = 0x10;
	stptr->gs = core_id * ((size_t) &percore_end0 - (size_t) &percore_start);
	stptr->rflags = 0x1202;
	stptr->userrsp = stptr->rsp;

	/* Set the task's stack pointer entry to the stack we have crafted right now. */
	task->last_stack_pointer = (size_t*)stack;

	return 0;
}

#define MAX_ARGS        (PAGE_SIZE - 2*sizeof(int))

/** @brief Structure which keeps all
 * relevant data for a new user task to start */
typedef struct {
	/// Points to a copy of the executable
	char* executable;
	/// Socket descriptor if have no file node
	int sd;
	/// Argument count
	int argc;
	/// Environment var count
	int envc;
	/// Buffer for env and argv values
	char buffer[MAX_ARGS];
} load_args_t;

/** @brief Internally used function to load tasks with a load_args_t structure
 * keeping all the information needed to launch.
 *
 * This is where the serious loading action is done.
 */
static int load_task(load_args_t* largs)
{
	uint32_t i;
	uint64_t offset, idx;
	uint64_t addr, npages;
	size_t stack = 0, heap = 0;
	size_t flags;
	elf_header_t header;
	elf_program_header_t prog_header;
	//elf_section_header_t sec_header;
	task_t* curr_task = per_core(current_task);
	int ret = -EINVAL;
	uint8_t hermit_exec = 0;

	if (!largs)
		return -EINVAL;

	if (!largs->executable || (largs->sd < 0))
		return -EINVAL;

	//curr_task->sd = largs->sd;
	memcpy(&header, largs->executable, sizeof(elf_header_t));

	if (BUILTIN_EXPECT(header.ident.magic != ELF_MAGIC, 0))
		goto Linvalid;

	if (BUILTIN_EXPECT(header.type != ELF_ET_EXEC, 0))
		goto Linvalid;

	if (BUILTIN_EXPECT(header.machine != ELF_EM_X86_64, 0))
		goto Linvalid;

	if (BUILTIN_EXPECT(header.ident._class != ELF_CLASS_64, 0))
		goto Linvalid;

	if (BUILTIN_EXPECT(header.ident.data != ELF_DATA_2LSB, 0))
		goto Linvalid;

	if (header.entry <= KERNEL_SPACE)
		goto Linvalid;

	// interpret program header table
	for (i=0; i<header.ph_entry_count; i++) {
		memcpy(&prog_header, largs->executable + header.ph_offset+i*header.ph_entry_size, sizeof(elf_program_header_t));

		switch(prog_header.type)
		{
		case  ELF_PT_LOAD:  // load program segment
			if (!prog_header.virt_addr)
				continue;

			//kprintf("Load segment at 0x%zx (0x%zx bytes)\n", prog_header.virt_addr, prog_header.mem_size);
			npages = (prog_header.virt_addr + prog_header.mem_size) - (prog_header.virt_addr & ~(PAGE_SIZE-1));
			npages = (npages >> PAGE_BITS);
			if ((prog_header.virt_addr + prog_header.mem_size) & (PAGE_SIZE-1))
				npages++;

			addr = get_pages(npages);
			if (BUILTIN_EXPECT(!addr, 0)) {
				kprintf("load_task: not enough memory for %d pages!\n", npages);
				ret = -ENOMEM;
				goto Lerr;
			}

			flags = PG_USER;
			if (has_nx() && !(prog_header.flags & PF_X))
				flags |= PG_XD;

			// map page frames in the address space of the current task
			if (page_map(prog_header.virt_addr & ~(PAGE_SIZE-1), addr, npages, flags|PG_RW)) {
				kprintf("Could not map 0x%x at 0x%x\n", addr, prog_header.virt_addr);
				ret = -ENOMEM;
				goto Lerr;
			}

			//kprintf("Map 0x%zx - 0x%zx\n", prog_header.virt_addr & ~(PAGE_SIZE-1), (prog_header.virt_addr & ~(PAGE_SIZE-1)) + npages*PAGE_SIZE - 1);
			// clear pages
			memset((void*) (prog_header.virt_addr & ~(PAGE_SIZE-1)), 0x00, npages*PAGE_SIZE);

			// update heap location
			if (heap < prog_header.virt_addr + prog_header.mem_size)
				heap = prog_header.virt_addr + prog_header.mem_size;

			// load program
			//kprintf("read programm 0x%zx - 0x%zx\n", prog_header.virt_addr, prog_header.virt_addr + prog_header.file_size);
			memcpy((uint8_t*)prog_header.virt_addr, largs->executable + prog_header.offset, prog_header.file_size);

			if (!(prog_header.flags & PF_W))
				page_set_flags(prog_header.virt_addr, npages, flags);

			flags = VMA_CACHEABLE|VMA_USER;
			if (prog_header.flags & PF_R)
				flags |= VMA_READ;
			if (prog_header.flags & PF_W)
				flags |= VMA_WRITE;
			if (prog_header.flags & PF_X)
				flags |= VMA_EXECUTE;
			vma_add(prog_header.virt_addr, prog_header.virt_addr+npages*PAGE_SIZE-1, flags);
			break;
		case ELF_PT_GNU_STACK: // Indicates stack executability
			// create user-level stack
			npages = DEFAULT_STACK_SIZE >> PAGE_BITS;
			if (DEFAULT_STACK_SIZE & (PAGE_SIZE-1))
				npages++;

			addr = get_pages(npages);
			if (BUILTIN_EXPECT(!addr, 0)) {
				kprintf("load_task: not enough memory!\n");
				ret = -ENOMEM;
				goto Lerr;
			}

			stack = (1ULL << 34ULL); // virtual address of the stack
			flags = PG_USER|PG_RW;
			if (has_nx() && !(prog_header.flags & PF_X))
				flags |= PG_XD;

			if (page_map(stack, addr, npages, flags)) {
				kprintf("Could not map stack at 0x%x\n", stack);
				ret = -ENOMEM;
				goto Lerr;
			}
			//kprintf("Map stack at 0x%zx -- 0x%zx\n", stack, stack + npages*PAGE_SIZE - 1);
			memset((void*) stack, 0x00, npages*PAGE_SIZE);

			// create vma regions for the user-level stack
			flags = VMA_CACHEABLE|VMA_USER;
			if (prog_header.flags & PF_R)
				flags |= VMA_READ;
			if (prog_header.flags & PF_W)
				flags |= VMA_WRITE;
			if (prog_header.flags & PF_X)
				flags |= VMA_EXECUTE;
			vma_add(stack, stack+npages*PAGE_SIZE-1, flags);
			break;
		case ELF_PT_TLS:
			kprintf("Found TLS segment. addr 0x%llx, mem size 0x%llx, file size 0x%llx\n", prog_header.virt_addr, prog_header.mem_size, prog_header.file_size);
#if 0
			curr_task->tls_addr = prog_header.virt_addr;
			curr_task->tls_mem_size = prog_header.mem_size;
			curr_task->tls_file_size = prog_header.file_size;
#endif
			break;
		case ELF_PT_NOTE:
			//kprintf("Found note segment: %s\n", (char*)prog_header.virt_addr + 12);
			hermit_exec |= (strcmp((char*)prog_header.virt_addr + 12, "HermitCore") == 0);
			break;
		default:
			kprintf("Unknown type 0x%lx in program header\n", prog_header.type);
		}
	}

	// setup heap
	if (!curr_task->heap)
		curr_task->heap = (vma_t*) kmalloc(sizeof(vma_t));

	if (BUILTIN_EXPECT(!curr_task->heap || !heap, 0)) {
		kprintf("load_task: heap is missing!\n");
		ret = -ENOMEM;
		goto Lerr;
	}

	curr_task->heap->flags = VMA_HEAP|VMA_USER;
	curr_task->heap->start = PAGE_FLOOR(heap);
	curr_task->heap->end = PAGE_FLOOR(heap);

	if (BUILTIN_EXPECT(!stack, 0)) {
		kprintf("Stack is missing!\n");
		ret = -ENOMEM;
		goto Lerr;
	}

	if (BUILTIN_EXPECT(!hermit_exec, 0)) {
		kprintf("Not a valid executable!\n");
		ret = -EINVAL;
		goto Lerr;

	}

	offset = DEFAULT_STACK_SIZE-16;

#if 0
	// do we have to create a TLS segement?
	if (curr_task->tls_addr && curr_task->tls_mem_size) {
		if (curr_task->tls_mem_size >= DEFAULT_STACK_SIZE-128) {
			kprintf("TLS is too large: 0x%zx\n", curr_task->tls_mem_size);
			ret = -ENOMEM;
			goto Lerr;
		}

		// set fs register to the TLS segment
		writefs(stack+offset);
		kprintf("Task %d set fs to 0x%zx\n", curr_task->id, stack+offset);

		// copy default TLS segment to stack
		offset -= curr_task->tls_mem_size;
		if (curr_task->tls_file_size)
			memcpy((void*) (stack+offset), (void*) curr_task->tls_addr, curr_task->tls_file_size);
	}
#endif

	// push strings on the stack
	memset((void*) (stack+offset), 0, 4);
	offset -= MAX_ARGS;
	memcpy((void*) (stack+offset), largs->buffer, MAX_ARGS);
	idx = offset;

	// push argv on the stack
	offset -= largs->argc * sizeof(char*);
	for(i=0; i<largs->argc; i++) {
		((char**) (stack+offset))[i] = (char*) (stack+idx);

		while(((char*) stack)[idx] != '\0')
			idx++;
		idx++;
	}

	// push env on the stack
	offset -= (largs->envc+1) * sizeof(char*);
	for(i=0; i<largs->envc; i++) {
		((char**) (stack+offset))[i] = (char*) (stack+idx);

		while(((char*) stack)[idx] != '\0')
			idx++;
		idx++;
	}
	((char**) (stack+offset))[largs->envc] = NULL;

	// align stack to be conform to the UNIX ABI
	size_t old_offset = offset;
	offset = offset & ~0xFULL;
	offset -= sizeof(size_t);

	// push pointer to env
	offset -= sizeof(char**);
	if (!(largs->envc))
		*((char***) (stack+offset)) = NULL;
	else
		*((char***) (stack+offset)) = (char**) (stack + old_offset);

	// push pointer to argv
	offset -= sizeof(char**);
	*((char***) (stack+offset)) = (char**) (stack + old_offset + (largs->envc+1) * sizeof(char*));

	// push argc on the stack
	offset -= sizeof(ssize_t);
	*((ssize_t*) (stack+offset)) = (ssize_t) largs->argc;

	if (largs->executable)
		kfree(largs->executable);
	kfree(largs);

	// clear fpu state => currently not supported
	curr_task->flags &= ~(TASK_FPU_USED|TASK_FPU_INIT);

	// map readonly kernel info into the user-space => vsyscall
	if (has_nx())
		page_map(START_ADDRESS - PAGE_SIZE, base, 1, PG_USER|PG_XD);
	else
		page_map(START_ADDRESS - PAGE_SIZE, base, 1, PG_USER);
	//kprintf("Map kernel info: 0x%zx - 0x%zx\n", START_ADDRESS - PAGE_SIZE, START_ADDRESS - 1);
	vma_add(START_ADDRESS - PAGE_SIZE, START_ADDRESS - 1, VMA_READ|VMA_CACHEABLE|VMA_USER);

	if (has_nx())
		page_map(START_ADDRESS - 2*PAGE_SIZE, phy_rcce_internals, 1, PG_USER|PG_RW|PG_XD);
	else
		page_map(START_ADDRESS - 2*PAGE_SIZE, phy_rcce_internals, 1, PG_USER|PG_RW);
	//kprintf("Map rcce info: 0x%zx - 0x%zx\n", START_ADDRESS - 2*PAGE_SIZE, START_ADDRESS - PAGE_SIZE - 1);
	vma_add(START_ADDRESS - 2*PAGE_SIZE, START_ADDRESS - PAGE_SIZE - 11, VMA_READ|VMA_WRITE|VMA_CACHEABLE|VMA_USER);

	//vma_dump();

	enter_user_task(header.entry, stack+offset);

	return 0;

Linvalid:
	kprintf("Invalid executable!\n");
	kprintf("magic number 0x%x\n", (uint32_t) header.ident.magic);
	kprintf("header type 0x%x\n", (uint32_t) header.type);
	kprintf("machine type 0x%x\n", (uint32_t) header.machine);
	kprintf("elf ident class 0x%x\n", (uint32_t) header.ident._class);
	kprintf("elf identdata 0x%x\n", header.ident.data);
	kprintf("program entry point 0x%lx\n", (size_t) header.entry);

Lerr:
	if (largs->executable)
		kfree(largs->executable);
	kfree(largs);

	return ret;
}

/** @brief This call is used to adapt create_task calls
 * which want to have a start function and argument list */
static int user_entry(void* arg)
{
	load_args_t* largs = (load_args_t*) arg;

	int ret;

	finish_task_switch();

	if (BUILTIN_EXPECT(!arg, 0))
		return -EINVAL;

	ret = load_task(largs);
	if (ret)
		kprintf("Load task failed: %d\n", ret);

	sys_exit(ret);

	while(1) {
		HALT;
	}
}

int create_user_task_form_socket(tid_t* id, int sd, uint8_t prio)
{
	int argc;
	int ret, err = -EINVAL;
	int len, total_len, i, j;
	load_args_t* load_args = NULL;
	char *dest;
	uint32_t core_id;
	uint32_t counter = 0;

	core_id = get_next_core_id();

	ret = read(sd, &argc, sizeof(int));
	if ((ret != sizeof(int)) || (argc <= 0))
		goto out;

	load_args = kmalloc(sizeof(load_args_t));
	if (BUILTIN_EXPECT(!load_args, 0)) {
		err = -ENOMEM;
		goto out;
	}

	load_args->executable = NULL;
	load_args->sd = sd;
	load_args->argc = argc;
	load_args->envc = 0;
	dest = load_args->buffer;

	for(i=0, total_len=0; i<argc; i++)
	{
		ret = read(sd, &len, sizeof(int));
		if ((ret != sizeof(int)) || (len <= 0))
			goto out;

		total_len += len;
		if (total_len >= MAX_ARGS)
			goto out;

		j=0;
		while(j < len)
		{
			ret = read(sd, dest, len-j);
			if (ret < 0)
				goto out;
			dest += ret;
			j += ret;
		}
	}

	// terminate array with a NULL pointer
	*dest = 0;

	ret = read(sd, &len, sizeof(int));
	if ((ret != sizeof(int)) || (len <= 0))
		goto out;
	//kprintf("length of the executable: %d\n", len);

	load_args->executable = kmalloc(len);
	if (!load_args->executable) {
		err = -ENOMEM;
		goto out;
	}

	j=0;
	while(j < len)
	{
		ret = read(sd, load_args->executable+j, len-j);
		if (ret < 0)
			goto out;
		j += ret;
	}

	/* create new task */
	return create_task(id, user_entry, load_args, prio, core_id);

out:
	kprintf("Unable to load task: %d\n", err);

	if (load_args && load_args->executable)
		kfree(load_args->executable);
	if (load_args)
		kfree(load_args);

	closesocket(sd);
	counter++;

	return err;
}
