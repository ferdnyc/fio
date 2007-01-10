/*
 * This file contains job initialization and setup functions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <assert.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "fio.h"
#include "parse.h"

#define FIO_RANDSEED		(0xb1899bedUL)

#define td_var_offset(var)	((size_t) &((struct thread_data *)0)->var)

static int str_rw_cb(void *, const char *);
static int str_ioengine_cb(void *, const char *);
static int str_mem_cb(void *, const char *);
static int str_verify_cb(void *, const char *);
static int str_lockmem_cb(void *, unsigned long *);
#ifdef FIO_HAVE_IOPRIO
static int str_prio_cb(void *, unsigned int *);
static int str_prioclass_cb(void *, unsigned int *);
#endif
static int str_exitall_cb(void);
static int str_cpumask_cb(void *, unsigned int *);

#define __stringify_1(x)	#x
#define __stringify(x)		__stringify_1(x)

/*
 * Map of job/command line options
 */
static struct fio_option options[] = {
	{
		.name	= "name",
		.type	= FIO_OPT_STR_STORE,
		.off1	= td_var_offset(name),
		.help	= "Name of this job",
	},
	{
		.name	= "directory",
		.type	= FIO_OPT_STR_STORE,
		.off1	= td_var_offset(directory),
		.help	= "Directory to store files in",
	},
	{
		.name	= "filename",
		.type	= FIO_OPT_STR_STORE,
		.off1	= td_var_offset(filename),
		.help	= "Force the use of a specific file",
	},
	{
		.name	= "rw",
		.type	= FIO_OPT_STR,
		.cb	= str_rw_cb,
		.help	= "IO direction",
		.def	= "read",
		.posval	= { "read", "write", "randwrite", "randread", "rw",
				"randrw", },
	},
	{
		.name	= "ioengine",
		.type	= FIO_OPT_STR,
		.cb	= str_ioengine_cb,
		.help	= "IO engine to use",
		.def	= "sync",
		.posval	= { "sync", "libaio", "posixaio", "mmap", "splice",
				"sg", "null", },
	},
	{
		.name	= "mem",
		.type	= FIO_OPT_STR,
		.cb	= str_mem_cb,
		.help	= "Backing type for IO buffers",
		.def	= "malloc",
		.posval	=  { "malloc", "shm", "shmhuge", "mmap", "mmaphuge", },
	},
	{
		.name	= "verify",
		.type	= FIO_OPT_STR,
		.cb	= str_verify_cb,
		.help	= "Verify sum function",
		.def	= "0",
		.posval	= { "crc32", "md5", },
	},
	{
		.name	= "write_iolog",
		.type	= FIO_OPT_STR_STORE,
		.off1	= td_var_offset(write_iolog_file),
		.help	= "Store IO pattern to file",
	},
	{
		.name	= "read_iolog",
		.type	= FIO_OPT_STR_STORE,
		.off1	= td_var_offset(read_iolog_file),
		.help	= "Playback IO pattern from file",
	},
	{
		.name	= "exec_prerun",
		.type	= FIO_OPT_STR_STORE,
		.off1	= td_var_offset(exec_prerun),
		.help	= "Execute this file prior to running job",
	},
	{
		.name	= "exec_postrun",
		.type	= FIO_OPT_STR_STORE,
		.off1	= td_var_offset(exec_postrun),
		.help	= "Execute this file after running job",
	},
#ifdef FIO_HAVE_IOSCHED_SWITCH
	{
		.name	= "ioscheduler",
		.type	= FIO_OPT_STR_STORE,
		.off1	= td_var_offset(ioscheduler),
		.help	= "Use this IO scheduler on the backing device",
	},
#endif
	{
		.name	= "size",
		.type	= FIO_OPT_STR_VAL,
		.off1	= td_var_offset(total_file_size),
		.help	= "Size of device or file",
	},
	{
		.name	= "bs",
		.type	= FIO_OPT_STR_VAL_INT,
		.off1	= td_var_offset(bs[DDIR_READ]),
		.off2	= td_var_offset(bs[DDIR_WRITE]),
		.help	= "Block size unit",
		.def	= "4k",
	},
	{
		.name	= "offset",
		.type	= FIO_OPT_STR_VAL,
		.off1	= td_var_offset(start_offset),
		.help	= "Start IO from this offset",
		.def	= "0",
	},
	{
		.name	= "zonesize",
		.type	= FIO_OPT_STR_VAL,
		.off1	= td_var_offset(zone_size),
		.help	= "Give size of an IO zone",
		.def	= "0",
	},
	{
		.name	= "zoneskip",
		.type	= FIO_OPT_STR_VAL,
		.off1	= td_var_offset(zone_skip),
		.help	= "Space between IO zones",
		.def	= "0",
	},
	{
		.name	= "lockmem",
		.type	= FIO_OPT_STR_VAL,
		.cb	= str_lockmem_cb,
		.help	= "Lock down this amount of memory",
		.def	= "0",
	},
	{
		.name	= "bsrange",
		.type	= FIO_OPT_RANGE,
		.off1	= td_var_offset(min_bs[DDIR_READ]),
		.off2	= td_var_offset(max_bs[DDIR_READ]),
		.off3	= td_var_offset(min_bs[DDIR_WRITE]),
		.off4	= td_var_offset(max_bs[DDIR_WRITE]),
		.help	= "Set block size range",
	},
	{
		.name	= "randrepeat",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(rand_repeatable),
		.help	= "Use repeatable random IO pattern",
		.def	= "1",
	},
	{
		.name	= "nrfiles",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(nr_files),
		.help	= "Split job workload between this number of files",
		.def	= "1",
	},
	{
		.name	= "iodepth",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(iodepth),
		.help	= "Amount of IO buffers to keep in flight",
		.def	= "1",
	},
	{
		.name	= "fsync",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(fsync_blocks),
		.help	= "Issue fsync for writes every given number of blocks",
		.def	= "0",
	},
	{
		.name	= "rwmixcycle",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(rwmixcycle),
		.help	= "Cycle period for mixed read/write workloads (msec)",
		.def	= "500",
	},
	{
		.name	= "rwmixread",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(rwmixread),
		.maxval	= 100,
		.help	= "Percentage of mixed workload that is reads",
		.def	= "50",
	},
	{
		.name	= "rwmixwrite",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(rwmixwrite),
		.maxval	= 100,
		.help	= "Percentage of mixed workload that is writes",
		.def	= "50",
	},
	{
		.name	= "nice",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(nice),
		.help	= "Set job CPU nice value",
		.minval	= -19,
		.maxval	= 20,
		.def	= "0",
	},
#ifdef FIO_HAVE_IOPRIO
	{
		.name	= "prio",
		.type	= FIO_OPT_INT,
		.cb	= str_prio_cb,
		.help	= "Set job IO priority value",
		.minval	= 0,
		.maxval	= 7,
	},
	{
		.name	= "prioclass",
		.type	= FIO_OPT_INT,
		.cb	= str_prioclass_cb,
		.help	= "Set job IO priority class",
		.minval	= 0,
		.maxval	= 3,
	},
#endif
	{
		.name	= "thinktime",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(thinktime),
		.help	= "Idle time between IO buffers",
		.def	= "0",
	},
	{
		.name	= "thinktime_blocks",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(thinktime_blocks),
		.help	= "IO buffer period between 'thinktime'",
		.def	= "1",
	},
	{
		.name	= "rate",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(rate),
		.help	= "Set bandwidth rate",
	},
	{
		.name	= "ratemin",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(ratemin),
		.help	= "The bottom limit accepted",
	},
	{
		.name	= "ratecycle",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(ratecycle),
		.help	= "Window average for rate limits (msec)",
		.def	= "1000",
	},
	{
		.name	= "startdelay",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(start_delay),
		.help	= "Only start job when this period has passed",
		.def	= "0",
	},
	{
		.name	= "timeout",
		.type	= FIO_OPT_STR_VAL_TIME,
		.off1	= td_var_offset(timeout),
		.help	= "Stop workload when this amount of time has passed",
		.def	= "0",
	},
	{
		.name	= "invalidate",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(invalidate_cache),
		.help	= "Invalidate buffer/page cache prior to running job",
		.def	= "1",
	},
	{
		.name	= "sync",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(sync_io),
		.help	= "Use O_SYNC for buffered writes",
		.def	= "0",
	},
	{
		.name	= "bwavgtime",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(bw_avg_time),
		.help	= "Time window over which to calculate bandwidth (msec)",
		.def	= "500",
	},
	{
		.name	= "create_serialize",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(create_serialize),
		.help	= "Serialize creating of job files",
		.def	= "1",
	},
	{
		.name	= "create_fsync",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(create_fsync),
		.help	= "Fsync file after creation",
		.def	= "1",
	},
	{
		.name	= "loops",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(loops),
		.help	= "Number of times to run the job",
		.def	= "1",
	},
	{
		.name	= "numjobs",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(numjobs),
		.help	= "Duplicate this job this many times",
		.def	= "1",
	},
	{
		.name	= "cpuload",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(cpuload),
		.help	= "Use this percentage of CPU",
	},
	{
		.name	= "cpuchunks",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(cpucycle),
		.help	= "Length of the CPU burn cycles",
	},
	{
		.name	= "direct",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(odirect),
		.help	= "Use O_DIRECT IO",
		.def	= "1",
	},
	{
		.name	= "overwrite",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(overwrite),
		.help	= "When writing, set whether to overwrite current data",
		.def	= "0",
	},
#ifdef FIO_HAVE_CPU_AFFINITY
	{
		.name	= "cpumask",
		.type	= FIO_OPT_INT,
		.cb	= str_cpumask_cb,
		.help	= "CPU affinity mask",
	},
#endif
	{
		.name	= "end_fsync",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(end_fsync),
		.help	= "Include fsync at the end of job",
		.def	= "0",
	},
	{
		.name	= "unlink",
		.type	= FIO_OPT_INT,
		.off1	= td_var_offset(unlink),
		.help	= "Unlink created files after job has completed",
		.def	= "1",
	},
	{
		.name	= "exitall",
		.type	= FIO_OPT_STR_SET,
		.cb	= str_exitall_cb,
		.help	= "Terminate all jobs when one exits",
	},
	{
		.name	= "stonewall",
		.type	= FIO_OPT_STR_SET,
		.off1	= td_var_offset(stonewall),
		.help	= "Insert a hard barrier between this job and previous",
	},
	{
		.name	= "thread",
		.type	= FIO_OPT_STR_SET,
		.off1	= td_var_offset(thread),
		.help	= "Use threads instead of forks",
	},
	{
		.name	= "write_bw_log",
		.type	= FIO_OPT_STR_SET,
		.off1	= td_var_offset(write_bw_log),
		.help	= "Write log of bandwidth during run",
	},
	{
		.name	= "write_lat_log",
		.type	= FIO_OPT_STR_SET,
		.off1	= td_var_offset(write_lat_log),
		.help	= "Write log of latency during run",
	},
	{
		.name	= "norandommap",
		.type	= FIO_OPT_STR_SET,
		.off1	= td_var_offset(norandommap),
		.help	= "Accept potential duplicate random blocks",
	},
	{
		.name	= "bs_unaligned",
		.type	= FIO_OPT_STR_SET,
		.off1	= td_var_offset(bs_unaligned),
		.help	= "Don't sector align IO buffer sizes",
	},
	{
		.name	= "hugepage-size",
		.type	= FIO_OPT_STR_VAL,
		.off1	= td_var_offset(hugepage_size),
		.help	= "When using hugepages, specify size of each page",
		.def	= __stringify(FIO_HUGE_PAGE),
	},
	{
		.name = NULL,
	},
};

#define FIO_JOB_OPTS	(sizeof(options) / sizeof(struct fio_option))
#define FIO_CMD_OPTS	(16)
#define FIO_GETOPT_JOB	(0x89988998)

/*
 * Command line options. These will contain the above, plus a few
 * extra that only pertain to fio itself and not jobs.
 */
static struct option long_options[FIO_JOB_OPTS + FIO_CMD_OPTS] = {
	{
		.name		= "output",
		.has_arg	= required_argument,
		.val		= 'o',
	},
	{
		.name		= "timeout",
		.has_arg	= required_argument,
		.val		= 't',
	},
	{
		.name		= "latency-log",
		.has_arg	= required_argument,
		.val		= 'l',
	},
	{
		.name		= "bandwidth-log",
		.has_arg	= required_argument,
		.val		= 'b',
	},
	{
		.name		= "minimal",
		.has_arg	= optional_argument,
		.val		= 'm',
	},
	{
		.name		= "version",
		.has_arg	= no_argument,
		.val		= 'v',
	},
	{
		.name		= "help",
		.has_arg	= no_argument,
		.val		= 'h',
	},
	{
		.name		= "cmdhelp",
		.has_arg	= required_argument,
		.val		= 'c',
	},
	{
		.name		= NULL,
	},
};

static int def_timeout = 0;

static char fio_version_string[] = "fio 1.11";

static char **ini_file;
static int max_jobs = MAX_JOBS;

struct thread_data def_thread;
struct thread_data *threads = NULL;

int exitall_on_terminate = 0;
int terse_output = 0;
unsigned long long mlock_size = 0;
FILE *f_out = NULL;
FILE *f_err = NULL;

static int write_lat_log = 0;
static int write_bw_log = 0;

/*
 * Return a free job structure.
 */
static struct thread_data *get_new_job(int global, struct thread_data *parent)
{
	struct thread_data *td;

	if (global)
		return &def_thread;
	if (thread_number >= max_jobs)
		return NULL;

	td = &threads[thread_number++];
	*td = *parent;

	td->thread_number = thread_number;
	return td;
}

static void put_job(struct thread_data *td)
{
	if (td == &def_thread)
		return;

	memset(&threads[td->thread_number - 1], 0, sizeof(*td));
	thread_number--;
}

/*
 * Lazy way of fixing up options that depend on each other. We could also
 * define option callback handlers, but this is easier.
 */
static void fixup_options(struct thread_data *td)
{
	if (!td->rwmixread && td->rwmixwrite)
		td->rwmixread = 100 - td->rwmixwrite;

	if (td->write_iolog_file && td->read_iolog_file) {
		log_err("fio: read iolog overrides write_iolog\n");
		free(td->write_iolog_file);
		td->write_iolog_file = NULL;
	}

	if (td->io_ops->flags & FIO_SYNCIO)
		td->iodepth = 1;
	else {
		if (!td->iodepth)
			td->iodepth = td->nr_files;
	}

	/*
	 * only really works for sequential io for now, and with 1 file
	 */
	if (td->zone_size && !td->sequential && td->nr_files == 1)
		td->zone_size = 0;

	/*
	 * Reads can do overwrites, we always need to pre-create the file
	 */
	if (td_read(td) || td_rw(td))
		td->overwrite = 1;

	if (!td->min_bs[DDIR_READ])
		td->min_bs[DDIR_READ]= td->bs[DDIR_READ];
	if (!td->max_bs[DDIR_READ])
		td->max_bs[DDIR_READ] = td->bs[DDIR_READ];
	if (!td->min_bs[DDIR_WRITE])
		td->min_bs[DDIR_WRITE]= td->bs[DDIR_WRITE];
	if (!td->max_bs[DDIR_WRITE])
		td->max_bs[DDIR_WRITE] = td->bs[DDIR_WRITE];

	td->rw_min_bs = min(td->min_bs[DDIR_READ], td->min_bs[DDIR_WRITE]);

	if (td_read(td) && !td_rw(td))
		td->verify = 0;

	if (td->norandommap && td->verify != VERIFY_NONE) {
		log_err("fio: norandommap given, verify disabled\n");
		td->verify = VERIFY_NONE;
	}
	if (td->bs_unaligned && (td->odirect || td->io_ops->flags & FIO_RAWIO))
		log_err("fio: bs_unaligned may not work with raw io\n");

	/*
	 * O_DIRECT and char doesn't mix, clear that flag if necessary.
	 */
	if (td->filetype == FIO_TYPE_CHAR && td->odirect)
		td->odirect = 0;
}

/*
 * This function leaks the buffer
 */
static char *to_kmg(unsigned int val)
{
	char *buf = malloc(32);
	char post[] = { 0, 'K', 'M', 'G', 'P', 0 };
	char *p = post;

	do {
		if (val & 1023)
			break;

		val >>= 10;
		p++;
	} while (*p);

	snprintf(buf, 31, "%u%c", val, *p);
	return buf;
}

/*
 * Adds a job to the list of things todo. Sanitizes the various options
 * to make sure we don't have conflicts, and initializes various
 * members of td.
 */
static int add_job(struct thread_data *td, const char *jobname, int job_add_num)
{
	const char *ddir_str[] = { "read", "write", "randread", "randwrite",
				   "rw", NULL, "randrw" };
	struct stat sb;
	int numjobs, ddir, i;
	struct fio_file *f;

	/*
	 * the def_thread is just for options, it's not a real job
	 */
	if (td == &def_thread)
		return 0;

	assert(td->io_ops);

	if (td->odirect)
		td->io_ops->flags |= FIO_RAWIO;

	td->filetype = FIO_TYPE_FILE;
	if (!stat(jobname, &sb)) {
		if (S_ISBLK(sb.st_mode))
			td->filetype = FIO_TYPE_BD;
		else if (S_ISCHR(sb.st_mode))
			td->filetype = FIO_TYPE_CHAR;
	}

	fixup_options(td);

	if (td->filename)
		td->nr_uniq_files = 1;
	else
		td->nr_uniq_files = td->nr_files;

	if (td->filetype == FIO_TYPE_FILE || td->filename) {
		char tmp[PATH_MAX];
		int len = 0;

		if (td->directory && td->directory[0] != '\0')
			len = sprintf(tmp, "%s/", td->directory);

		td->files = malloc(sizeof(struct fio_file) * td->nr_files);

		for_each_file(td, f, i) {
			memset(f, 0, sizeof(*f));
			f->fd = -1;

			if (td->filename)
				sprintf(tmp + len, "%s", td->filename);
			else
				sprintf(tmp + len, "%s.%d.%d", jobname, td->thread_number, i);
			f->file_name = strdup(tmp);
		}
	} else {
		td->nr_files = 1;
		td->files = malloc(sizeof(struct fio_file));
		f = &td->files[0];

		memset(f, 0, sizeof(*f));
		f->fd = -1;
		f->file_name = strdup(jobname);
	}

	for_each_file(td, f, i) {
		f->file_size = td->total_file_size / td->nr_files;
		f->file_offset = td->start_offset;
	}
		
	fio_sem_init(&td->mutex, 0);

	td->clat_stat[0].min_val = td->clat_stat[1].min_val = ULONG_MAX;
	td->slat_stat[0].min_val = td->slat_stat[1].min_val = ULONG_MAX;
	td->bw_stat[0].min_val = td->bw_stat[1].min_val = ULONG_MAX;

	if (td->stonewall && td->thread_number > 1)
		groupid++;

	td->groupid = groupid;

	if (setup_rate(td))
		goto err;

	if (td->write_lat_log) {
		setup_log(&td->slat_log);
		setup_log(&td->clat_log);
	}
	if (td->write_bw_log)
		setup_log(&td->bw_log);

	if (!td->name)
		td->name = strdup(jobname);

	ddir = td->ddir + (!td->sequential << 1) + (td->iomix << 2);

	if (!terse_output) {
		if (!job_add_num) {
			if (td->io_ops->flags & FIO_CPUIO)
				fprintf(f_out, "%s: ioengine=cpu, cpuload=%u, cpucycle=%u\n", td->name, td->cpuload, td->cpucycle);
			else {
				char *c1, *c2, *c3, *c4;

				c1 = to_kmg(td->min_bs[DDIR_READ]);
				c2 = to_kmg(td->max_bs[DDIR_READ]);
				c3 = to_kmg(td->min_bs[DDIR_WRITE]);
				c4 = to_kmg(td->max_bs[DDIR_WRITE]);

				fprintf(f_out, "%s: (g=%d): rw=%s, odir=%u, bs=%s-%s/%s-%s, rate=%u, ioengine=%s, iodepth=%u\n", td->name, td->groupid, ddir_str[ddir], td->odirect, c1, c2, c3, c4, td->rate, td->io_ops->name, td->iodepth);

				free(c1);
				free(c2);
				free(c3);
				free(c4);
			}
		} else if (job_add_num == 1)
			fprintf(f_out, "...\n");
	}

	/*
	 * recurse add identical jobs, clear numjobs and stonewall options
	 * as they don't apply to sub-jobs
	 */
	numjobs = td->numjobs;
	while (--numjobs) {
		struct thread_data *td_new = get_new_job(0, td);

		if (!td_new)
			goto err;

		td_new->numjobs = 1;
		td_new->stonewall = 0;
		job_add_num = numjobs - 1;

		if (add_job(td_new, jobname, job_add_num))
			goto err;
	}
	return 0;
err:
	put_job(td);
	return -1;
}

/*
 * Initialize the various random states we need (random io, block size ranges,
 * read/write mix, etc).
 */
int init_random_state(struct thread_data *td)
{
	unsigned long seeds[4];
	int fd, num_maps, blocks, i;
	struct fio_file *f;

	if (td->io_ops->flags & FIO_CPUIO)
		return 0;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd == -1) {
		td_verror(td, errno);
		return 1;
	}

	if (read(fd, seeds, sizeof(seeds)) < (int) sizeof(seeds)) {
		td_verror(td, EIO);
		close(fd);
		return 1;
	}

	close(fd);

	os_random_seed(seeds[0], &td->bsrange_state);
	os_random_seed(seeds[1], &td->verify_state);
	os_random_seed(seeds[2], &td->rwmix_state);

	if (td->sequential)
		return 0;

	if (td->rand_repeatable)
		seeds[3] = FIO_RANDSEED;

	if (!td->norandommap) {
		for_each_file(td, f, i) {
			blocks = (f->file_size + td->rw_min_bs - 1) / td->rw_min_bs;
			num_maps = (blocks + BLOCKS_PER_MAP-1)/ BLOCKS_PER_MAP;
			f->file_map = malloc(num_maps * sizeof(long));
			f->num_maps = num_maps;
			memset(f->file_map, 0, num_maps * sizeof(long));
		}
	}

	os_random_seed(seeds[3], &td->random_state);
	return 0;
}

static void fill_cpu_mask(os_cpu_mask_t cpumask, int cpu)
{
#ifdef FIO_HAVE_CPU_AFFINITY
	unsigned int i;

	CPU_ZERO(&cpumask);

	for (i = 0; i < sizeof(int) * 8; i++) {
		if ((1 << i) & cpu)
			CPU_SET(i, &cpumask);
	}
#endif
}

static int is_empty_or_comment(char *line)
{
	unsigned int i;

	for (i = 0; i < strlen(line); i++) {
		if (line[i] == ';')
			return 1;
		if (!isspace(line[i]) && !iscntrl(line[i]))
			return 0;
	}

	return 1;
}

static int str_rw_cb(void *data, const char *mem)
{
	struct thread_data *td = data;

	if (!strncmp(mem, "read", 4) || !strncmp(mem, "0", 1)) {
		td->ddir = DDIR_READ;
		td->sequential = 1;
		return 0;
	} else if (!strncmp(mem, "randread", 8)) {
		td->ddir = DDIR_READ;
		td->sequential = 0;
		return 0;
	} else if (!strncmp(mem, "write", 5) || !strncmp(mem, "1", 1)) {
		td->ddir = DDIR_WRITE;
		td->sequential = 1;
		return 0;
	} else if (!strncmp(mem, "randwrite", 9)) {
		td->ddir = DDIR_WRITE;
		td->sequential = 0;
		return 0;
	} else if (!strncmp(mem, "rw", 2)) {
		td->ddir = DDIR_READ;
		td->iomix = 1;
		td->sequential = 1;
		return 0;
	} else if (!strncmp(mem, "randrw", 6)) {
		td->ddir = DDIR_READ;
		td->iomix = 1;
		td->sequential = 0;
		return 0;
	}

	log_err("fio: data direction: read, write, randread, randwrite, rw, randrw\n");
	return 1;
}

static int str_verify_cb(void *data, const char *mem)
{
	struct thread_data *td = data;

	if (!strncmp(mem, "0", 1)) {
		td->verify = VERIFY_NONE;
		return 0;
	} else if (!strncmp(mem, "md5", 3) || !strncmp(mem, "1", 1)) {
		td->verify = VERIFY_MD5;
		return 0;
	} else if (!strncmp(mem, "crc32", 5)) {
		td->verify = VERIFY_CRC32;
		return 0;
	}

	log_err("fio: verify types: md5, crc32\n");
	return 1;
}

/*
 * Check if mmap/mmaphuge has a :/foo/bar/file at the end. If so, return that.
 */
static char *get_mmap_file(const char *str)
{
	char *p = strstr(str, ":");

	if (!p)
		return NULL;

	p++;
	strip_blank_front(&p);
	strip_blank_end(p);
	return strdup(p);
}

static int str_mem_cb(void *data, const char *mem)
{
	struct thread_data *td = data;

	if (!strncmp(mem, "malloc", 6)) {
		td->mem_type = MEM_MALLOC;
		return 0;
	} else if (!strncmp(mem, "mmaphuge", 8)) {
#ifdef FIO_HAVE_HUGETLB
		/*
		 * mmaphuge must be appended with the actual file
		 */
		td->mmapfile = get_mmap_file(mem);
		if (!td->mmapfile) {
			log_err("fio: mmaphuge:/path/to/file\n");
			return 1;
		}

		td->mem_type = MEM_MMAPHUGE;
		return 0;
#else
		log_err("fio: mmaphuge not available\n");
		return 1;
#endif
	} else if (!strncmp(mem, "mmap", 4)) {
		/*
		 * Check if the user wants file backed memory. It's ok
		 * if there's no file given, we'll just use anon mamp then.
		 */
		td->mmapfile = get_mmap_file(mem);
		td->mem_type = MEM_MMAP;
		return 0;
	} else if (!strncmp(mem, "shmhuge", 7)) {
#ifdef FIO_HAVE_HUGETLB
		td->mem_type = MEM_SHMHUGE;
		return 0;
#else
		log_err("fio: shmhuge not available\n");
		return 1;
#endif
	} else if (!strncmp(mem, "shm", 3)) {
		td->mem_type = MEM_SHM;
		return 0;
	}

	log_err("fio: mem type: malloc, shm, shmhuge, mmap, mmaphuge\n");
	return 1;
}

static int str_ioengine_cb(void *data, const char *str)
{
	struct thread_data *td = data;

	td->io_ops = load_ioengine(td, str);
	if (td->io_ops)
		return 0;

	log_err("fio: ioengine= libaio, posixaio, sync, mmap, sgio, splice, cpu, null\n");
	log_err("fio: or specify path to dynamic ioengine module\n");
	return 1;
}

static int str_lockmem_cb(void fio_unused *data, unsigned long *val)
{
	mlock_size = *val;
	return 0;
}

#ifdef FIO_HAVE_IOPRIO
static int str_prioclass_cb(void *data, unsigned int *val)
{
	struct thread_data *td = data;

	td->ioprio |= *val << IOPRIO_CLASS_SHIFT;
	return 0;
}

static int str_prio_cb(void *data, unsigned int *val)
{
	struct thread_data *td = data;

	td->ioprio |= *val;
	return 0;
}
#endif

static int str_exitall_cb(void)
{
	exitall_on_terminate = 1;
	return 0;
}

static int str_cpumask_cb(void *data, unsigned int *val)
{
	struct thread_data *td = data;

	fill_cpu_mask(td->cpumask, *val);
	return 0;
}

/*
 * This is our [ini] type file parser.
 */
static int parse_jobs_ini(char *file, int stonewall_flag)
{
	unsigned int global;
	struct thread_data *td;
	char *string, *name;
	fpos_t off;
	FILE *f;
	char *p;
	int ret = 0, stonewall;

	f = fopen(file, "r");
	if (!f) {
		perror("fopen job file");
		return 1;
	}

	string = malloc(4096);
	name = malloc(256);
	memset(name, 0, 256);

	stonewall = stonewall_flag;
	do {
		p = fgets(string, 4095, f);
		if (!p)
			break;
		if (is_empty_or_comment(p))
			continue;
		if (sscanf(p, "[%255s]", name) != 1)
			continue;

		global = !strncmp(name, "global", 6);

		name[strlen(name) - 1] = '\0';

		td = get_new_job(global, &def_thread);
		if (!td) {
			ret = 1;
			break;
		}

		/*
		 * Seperate multiple job files by a stonewall
		 */
		if (!global && stonewall) {
			td->stonewall = stonewall;
			stonewall = 0;
		}

		fgetpos(f, &off);
		while ((p = fgets(string, 4096, f)) != NULL) {
			if (is_empty_or_comment(p))
				continue;

			strip_blank_front(&p);

			if (p[0] == '[')
				break;

			strip_blank_end(p);

			fgetpos(f, &off);

			/*
			 * Don't break here, continue parsing options so we
			 * dump all the bad ones. Makes trial/error fixups
			 * easier on the user.
			 */
			ret |= parse_option(p, options, td);
		}

		if (!ret) {
			fsetpos(f, &off);
			ret = add_job(td, name, 0);
		} else {
			log_err("fio: job %s dropped\n", name);
			put_job(td);
		}
	} while (!ret);

	free(string);
	free(name);
	fclose(f);
	return ret;
}

static int fill_def_thread(void)
{
	memset(&def_thread, 0, sizeof(def_thread));

	if (fio_getaffinity(getpid(), &def_thread.cpumask) == -1) {
		perror("sched_getaffinity");
		return 1;
	}

	/*
	 * fill default options
	 */
	fill_default_options(&def_thread, options);

	def_thread.timeout = def_timeout;
	def_thread.write_bw_log = write_bw_log;
	def_thread.write_lat_log = write_lat_log;

#ifdef FIO_HAVE_DISK_UTIL
	def_thread.do_disk_util = 1;
#endif

	return 0;
}

static void usage(void)
{
	printf("%s\n", fio_version_string);
	printf("\t--output\tWrite output to file\n");
	printf("\t--timeout\tRuntime in seconds\n");
	printf("\t--latency-log\tGenerate per-job latency logs\n");
	printf("\t--bandwidth-log\tGenerate per-job bandwidth logs\n");
	printf("\t--minimal\tMinimal (terse) output\n");
	printf("\t--version\tPrint version info and exit\n");
	printf("\t--help\t\tPrint this page\n");
	printf("\t--cmdhelp=cmd\tPrint command help, \"all\" for all of them\n");
}

static int parse_cmd_line(int argc, char *argv[])
{
	struct thread_data *td = NULL;
	int c, ini_idx = 0, lidx, ret;

	while ((c = getopt_long(argc, argv, "", long_options, &lidx)) != -1) {
		switch (c) {
		case 't':
			def_timeout = atoi(optarg);
			break;
		case 'l':
			write_lat_log = 1;
			break;
		case 'w':
			write_bw_log = 1;
			break;
		case 'o':
			f_out = fopen(optarg, "w+");
			if (!f_out) {
				perror("fopen output");
				exit(1);
			}
			f_err = f_out;
			break;
		case 'm':
			terse_output = 1;
			break;
		case 'h':
			usage();
			exit(0);
		case 'c':
			ret = show_cmd_help(options, optarg);
			exit(ret);
		case 'v':
			printf("%s\n", fio_version_string);
			exit(0);
		case FIO_GETOPT_JOB: {
			const char *opt = long_options[lidx].name;
			char *val = optarg;

			if (!strncmp(opt, "name", 4) && td) {
				ret = add_job(td, td->name ?: "fio", 0);
				if (ret) {
					put_job(td);
					return 0;
				}
				td = NULL;
			}
			if (!td) {
				int global = !strncmp(val, "global", 6);

				td = get_new_job(global, &def_thread);
				if (!td)
					return 0;
			}

			ret = parse_cmd_option(opt, val, options, td);
			if (ret) {
				log_err("fio: job dropped\n");
				put_job(td);
				td = NULL;
			}
			break;
		}
		default:
			break;
		}
	}

	if (td) {
		ret = add_job(td, td->name ?: "fio", 0);
		if (ret)
			put_job(td);
	}

	while (optind < argc) {
		ini_idx++;
		ini_file = realloc(ini_file, ini_idx * sizeof(char *));
		ini_file[ini_idx - 1] = strdup(argv[optind]);
		optind++;
	}

	return ini_idx;
}

static void free_shm(void)
{
	struct shmid_ds sbuf;

	if (threads) {
		shmdt((void *) threads);
		threads = NULL;
		shmctl(shm_id, IPC_RMID, &sbuf);
	}
}

/*
 * The thread area is shared between the main process and the job
 * threads/processes. So setup a shared memory segment that will hold
 * all the job info.
 */
static int setup_thread_area(void)
{
	/*
	 * 1024 is too much on some machines, scale max_jobs if
	 * we get a failure that looks like too large a shm segment
	 */
	do {
		size_t size = max_jobs * sizeof(struct thread_data);

		shm_id = shmget(0, size, IPC_CREAT | 0600);
		if (shm_id != -1)
			break;
		if (errno != EINVAL) {
			perror("shmget");
			break;
		}

		max_jobs >>= 1;
	} while (max_jobs);

	if (shm_id == -1)
		return 1;

	threads = shmat(shm_id, NULL, 0);
	if (threads == (void *) -1) {
		perror("shmat");
		return 1;
	}

	atexit(free_shm);
	return 0;
}

/*
 * Copy the fio options into the long options map, so we mirror
 * job and cmd line options.
 */
static void dupe_job_options(void)
{
	struct fio_option *o;
	unsigned int i;

	i = 0;
	while (long_options[i].name)
		i++;

	o = &options[0];
	while (o->name) {
		long_options[i].name = o->name;
		long_options[i].val = FIO_GETOPT_JOB;
		if (o->type == FIO_OPT_STR_SET)
			long_options[i].has_arg = no_argument;
		else
			long_options[i].has_arg = required_argument;

		i++;
		o++;
		assert(i < FIO_JOB_OPTS + FIO_CMD_OPTS);
	}
}

int parse_options(int argc, char *argv[])
{
	int job_files, i;

	f_out = stdout;
	f_err = stderr;

	dupe_job_options();

	if (setup_thread_area())
		return 1;
	if (fill_def_thread())
		return 1;

	job_files = parse_cmd_line(argc, argv);

	for (i = 0; i < job_files; i++) {
		if (fill_def_thread())
			return 1;
		if (parse_jobs_ini(ini_file[i], i))
			return 1;
		free(ini_file[i]);
	}

	free(ini_file);

	if (!thread_number) {
		log_err("No jobs defined(s)\n");
		return 1;
	}

	return 0;
}
