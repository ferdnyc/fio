/*
 * Memory helpers
 */
#include <unistd.h>
#include <sys/shm.h>
#include <sys/mman.h>

#include "fio.h"

static void *pinned_mem;

void fio_unpin_memory(void)
{
	if (pinned_mem) {
		if (munlock(pinned_mem, mlock_size) < 0)
			perror("munlock");
		munmap(pinned_mem, mlock_size);
		pinned_mem = NULL;
	}
}

int fio_pin_memory(void)
{
	unsigned long long phys_mem;

	if (!mlock_size)
		return 0;

	/*
	 * Don't allow mlock of more than real_mem-128MB
	 */
	phys_mem = os_phys_mem();
	if (phys_mem) {
		if ((mlock_size + 128 * 1024 * 1024) > phys_mem) {
			mlock_size = phys_mem - 128 * 1024 * 1024;
			log_info("fio: limiting mlocked memory to %lluMiB\n", mlock_size >> 20);
		}
	}

	pinned_mem = mmap(NULL, mlock_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | OS_MAP_ANON, 0, 0);
	if (pinned_mem == MAP_FAILED) {
		perror("malloc locked mem");
		pinned_mem = NULL;
		return 1;
	}
	if (mlock(pinned_mem, mlock_size) < 0) {
		perror("mlock");
		munmap(pinned_mem, mlock_size);
		pinned_mem = NULL;
		return 1;
	}

	return 0;
}

static int alloc_mem_shm(struct thread_data *td)
{
	int flags = IPC_CREAT | SHM_R | SHM_W;

	if (td->o.mem_type == MEM_SHMHUGE)
		flags |= SHM_HUGETLB;

	td->shm_id = shmget(IPC_PRIVATE, td->orig_buffer_size, flags);
	if (td->shm_id < 0) {
		td_verror(td, errno, "shmget");
		if (geteuid() != 0 && errno == ENOMEM)
			log_err("fio: you may need to run this job as root\n");
		if (errno == EINVAL && td->o.mem_type == MEM_SHMHUGE)
			log_err("fio: check that you have free huge pages and that hugepage-size is correct.\n");
		
		return 1;
	}

	td->orig_buffer = shmat(td->shm_id, NULL, 0);
	if (td->orig_buffer == (void *) -1) {
		td_verror(td, errno, "shmat");
		td->orig_buffer = NULL;
		return 1;
	}

	return 0;
}

static int alloc_mem_mmap(struct thread_data *td)
{
	int flags = MAP_PRIVATE;

	td->mmapfd = 0;

	if (td->mmapfile) {
		td->mmapfd = open(td->mmapfile, O_RDWR|O_CREAT, 0644);

		if (td->mmapfd < 0) {
			td_verror(td, errno, "open mmap file");
			td->orig_buffer = NULL;
			return 1;
		}
		if (ftruncate(td->mmapfd, td->orig_buffer_size) < 0) {
			td_verror(td, errno, "truncate mmap file");
			td->orig_buffer = NULL;
			return 1;
		}
	} else
		flags |= OS_MAP_ANON;

	td->orig_buffer = mmap(NULL, td->orig_buffer_size, PROT_READ | PROT_WRITE, flags, td->mmapfd, 0);
	if (td->orig_buffer == MAP_FAILED) {
		td_verror(td, errno, "mmap");
		td->orig_buffer = NULL;
		if (td->mmapfd) {
			close(td->mmapfd);
			unlink(td->mmapfile);
		}
			
		return 1;
	}

	return 0;
}

static int alloc_mem_malloc(struct thread_data *td)
{
	td->orig_buffer = malloc(td->orig_buffer_size);
	if (td->orig_buffer)
		return 0;

	return 1;
}

/*
 * Setup the buffer area we need for io.
 */
int allocate_io_mem(struct thread_data *td)
{
	int ret = 0;

	if (td->o.mem_type == MEM_MALLOC)
		ret = alloc_mem_malloc(td);
	else if (td->o.mem_type == MEM_SHM || td->o.mem_type == MEM_SHMHUGE)
		ret = alloc_mem_shm(td);
	else if (td->o.mem_type == MEM_MMAP || td->o.mem_type == MEM_MMAPHUGE)
		ret = alloc_mem_mmap(td);
	else {
		log_err("fio: bad mem type: %d\n", td->o.mem_type);
		ret = 1;
	}

	if (ret)
		td_verror(td, ENOMEM, "iomem allocation");

	return ret;
}

void free_io_mem(struct thread_data *td)
{
	if (td->o.mem_type == MEM_MALLOC)
		free(td->orig_buffer);
	else if (td->o.mem_type == MEM_SHM || td->o.mem_type == MEM_SHMHUGE) {
		struct shmid_ds sbuf;

		shmdt(td->orig_buffer);
		shmctl(td->shm_id, IPC_RMID, &sbuf);
	} else if (td->o.mem_type == MEM_MMAP ||
		   td->o.mem_type == MEM_MMAPHUGE) {
		munmap(td->orig_buffer, td->orig_buffer_size);
		if (td->mmapfile) {
			close(td->mmapfd);
			unlink(td->mmapfile);
			free(td->mmapfile);
		}
	} else
		log_err("Bad memory type %u\n", td->o.mem_type);

	td->orig_buffer = NULL;
}
