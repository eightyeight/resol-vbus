#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#endif // _WIN32

#include "../../driver/ppm_ringbuffer.h"
#include "scap.h"
#include "scap_savefile.h"
#include "scap-int.h"

//#define NDEBUG
#include <assert.h>

char* scap_getlasterr(scap_t* handle)
{
	return handle->m_lasterr;
}

scap_t* scap_open_live(char *error)
{
#ifdef _WIN32
	snprintf(error, SCAP_LASTERR_SIZE, "live capture not supported on windows");
	return NULL;
#elif defined(__APPLE__)
	snprintf(error, SCAP_LASTERR_SIZE, "live capture not supported on OSX");
	return NULL;
#else
	uint32_t j;
	char dev[255];
	scap_t* handle = NULL;
	int len;
	uint32_t ndevs;
	uint32_t res;

	//
	// Allocate the handle
	//
	handle = (scap_t*)malloc(sizeof(scap_t));
	if(!handle)
	{
		snprintf(error, SCAP_LASTERR_SIZE, "error allocating the scap_t structure");
		return NULL;
	}

	//
	// Preliminary initializations
	//
	handle->m_ndevs = 0;
	handle->m_proclist = NULL;
	handle->m_file = NULL;
	handle->m_file_evt_buf = NULL;
	handle->m_evtcnt = 0;
	handle->m_addrlist = NULL;
	handle->m_userlist = NULL;
	handle->m_emptybuf_timeout_ms = BUFFER_EMPTY_WAIT_TIME_MS;

	//
	// Find out how many devices we have to open, which equals to the number of CPUs
	//
	ndevs = sysconf(_SC_NPROCESSORS_ONLN);

	//
	// Allocate the device descriptors.
	//
	len = RING_BUF_SIZE * 2;

	handle->m_devs = (scap_device*)malloc(ndevs * sizeof(scap_device));
	if(!handle->m_devs)
	{
		scap_close(handle);
		snprintf(error, SCAP_LASTERR_SIZE, "error allocating the device handles");
		return NULL;
	}

	//
	// Allocate the array of poll fds.
	//
	handle->m_pollfds = (struct pollfd*)malloc(ndevs * sizeof(struct pollfd));
	if(!handle->m_pollfds)
	{
		scap_close(handle);
		snprintf(error, SCAP_LASTERR_SIZE, "error allocating the device handles");
		return NULL;
	}

	for(j = 0; j < ndevs; j++)
	{
		handle->m_devs[j].m_buffer = (char*)MAP_FAILED;
		handle->m_devs[j].m_bufinfo = (struct ppm_ring_buffer_info*)MAP_FAILED;
	}

	handle->m_ndevs = ndevs;

	//
	// Extract machine information
	//
	handle->m_machine_info.num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	handle->m_machine_info.memory_size_bytes = (uint64_t)sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE);
	gethostname(handle->m_machine_info.hostname, sizeof(handle->m_machine_info.hostname) / sizeof(handle->m_machine_info.hostname[0]));
	handle->m_machine_info.reserved1 = 0;
	handle->m_machine_info.reserved2 = 0;
	handle->m_machine_info.reserved3 = 0;
	handle->m_machine_info.reserved4 = 0;

	//
	// Create the interface list
	//
	if(scap_create_iflist(handle) != SCAP_SUCCESS)
	{
		scap_close(handle);
		snprintf(error, SCAP_LASTERR_SIZE, "error creating the interface list");
		return NULL;
	}

	//
	// Create the user list
	//
	if(scap_create_userlist(handle) != SCAP_SUCCESS)
	{
		scap_close(handle);
		snprintf(error, SCAP_LASTERR_SIZE, "error creating the interface list");
		return NULL;
	}

	//
	// Create the process list
	//
	error[0] = '\0';
	if((res = scap_proc_scan_proc_dir(handle, "/proc", -1, -1, NULL, error)) != SCAP_SUCCESS)
	{
		scap_close(handle);
		snprintf(error, SCAP_LASTERR_SIZE, "error creating the process list");
		return NULL;
	}

	handle->m_fake_kernel_proc.tid = -1;
	handle->m_fake_kernel_proc.pid = -1;
	handle->m_fake_kernel_proc.flags = 0;
	snprintf(handle->m_fake_kernel_proc.comm, SCAP_MAX_PATH_SIZE, "kernel");
	snprintf(handle->m_fake_kernel_proc.exe, SCAP_MAX_PATH_SIZE, "kernel");
	handle->m_fake_kernel_proc.args[0] = 0;

	//
	// Open and initialize all the devices
	//
	for(j = 0; j < handle->m_ndevs; j++)
	{
		//
		// Open the device
		//
		sprintf(dev, "/dev/sysdig%d", j);

		if((handle->m_devs[j].m_fd = open(dev, O_RDWR | O_SYNC)) < 0)
		{
			scap_close(handle);
			snprintf(error, SCAP_LASTERR_SIZE, "error opening device %s", dev);
			return NULL;
		}

		//
		// Init the polling fd for the device
		//
		handle->m_pollfds[j].fd = handle->m_devs[j].m_fd;
		handle->m_pollfds[j].events = POLLIN;

		//
		// Map the ring buffer
		//
		handle->m_devs[j].m_buffer = (char*)mmap(0,
		                             len,
		                             PROT_READ,
		                             MAP_SHARED,
		                             handle->m_devs[j].m_fd,
		                             0);

		if(handle->m_devs[j].m_buffer == MAP_FAILED)
		{
			// we cleanup this fd and then we let scap_close() take care of the other ones
			close(handle->m_devs[j].m_fd);

			scap_close(handle);

			snprintf(error, SCAP_LASTERR_SIZE, "error mapping the ring buffer for device %s", dev);
			return NULL;
		}

		//
		// Map the ppm_ring_buffer_info that contains the buffer pointers
		//
		handle->m_devs[j].m_bufinfo = (struct ppm_ring_buffer_info*)mmap(0,
		                              sizeof(struct ppm_ring_buffer_info),
		                              PROT_READ | PROT_WRITE,
		                              MAP_SHARED,
		                              handle->m_devs[j].m_fd,
		                              0);

		if(handle->m_devs[j].m_bufinfo == MAP_FAILED)
		{
			// we cleanup this fd and then we let scap_close() take care of the other ones
			munmap(handle->m_devs[j].m_buffer, len);
			close(handle->m_devs[j].m_fd);

			scap_close(handle);

			snprintf(error, SCAP_LASTERR_SIZE, "error mapping the ring buffer info for device %s", dev);
			return NULL;
		}

		//
		// Additional initializations
		//
		handle->m_devs[j].m_lastreadsize = 0;
		handle->m_devs[j].m_sn_len = 0;
		scap_stop_dropping_mode(handle);
	}

	return handle;
#endif // _WIN32
}

scap_t* scap_open_offline(char* fname, char *error)
{
	scap_t* handle = NULL;

	//
	// Allocate the handle
	//
	handle = (scap_t*)malloc(sizeof(scap_t));
	if(!handle)
	{
		snprintf(error, SCAP_LASTERR_SIZE, "error allocating the scap_t structure");
		return NULL;
	}

	//
	// Preliminary initializations
	//
	handle->m_devs = NULL;
	handle->m_ndevs = 0;
	handle->m_proclist = NULL;
	handle->m_pollfds = NULL;
	handle->m_evtcnt = 0;
	handle->m_file = NULL;
	handle->m_addrlist = NULL;
	handle->m_userlist = NULL;
	handle->m_machine_info.num_cpus = (uint32_t)-1;

	handle->m_file_evt_buf = (char*)malloc(FILE_READ_BUF_SIZE);
	if(!handle->m_file_evt_buf)
	{
		snprintf(error, SCAP_LASTERR_SIZE, "error allocating the read buffer");
		scap_close(handle);
		return NULL;
	}

	//
	// Open the file
	//
	handle->m_file = fopen(fname, "rb");
	if(handle->m_file == NULL)
	{
		snprintf(error, SCAP_LASTERR_SIZE, "can't open file %s", fname);
		scap_close(handle);
		return NULL;
	}

	//
	// Validate the file and load the non-event blocks
	//
	if(scap_read_init(handle, handle->m_file) != SCAP_SUCCESS)
	{
		snprintf(error, SCAP_LASTERR_SIZE, "%s", scap_getlasterr(handle));
		scap_close(handle);
		return NULL;
	}

	//
	// Add the fake process for kernel threads
	//
	handle->m_fake_kernel_proc.tid = -1;
	handle->m_fake_kernel_proc.pid = -1;
	handle->m_fake_kernel_proc.flags = 0;
	snprintf(handle->m_fake_kernel_proc.comm, SCAP_MAX_PATH_SIZE, "kernel");
	snprintf(handle->m_fake_kernel_proc.exe, SCAP_MAX_PATH_SIZE, "kernel");
	handle->m_fake_kernel_proc.args[0] = 0;

//scap_proc_print_table(handle);

	return handle;
}

int32_t scap_set_empty_buffer_timeout_ms(scap_t* handle, uint32_t timeout_ms)
{
	handle->m_emptybuf_timeout_ms = timeout_ms;
	return SCAP_SUCCESS;
}

void scap_close(scap_t* handle)
{
	if(handle->m_file)
	{
		fclose(handle->m_file);
	}
	else
	{
#if !defined(_WIN32) && !defined(__APPLE__)
		uint32_t j;

		ASSERT(handle->m_file == NULL);

		//
		// Destroy all the device descriptors
		//
		for(j = 0; j < handle->m_ndevs; j++)
		{
			if(handle->m_devs[j].m_buffer != MAP_FAILED)
			{
				munmap(handle->m_devs[j].m_bufinfo, sizeof(struct ppm_ring_buffer_info));
				munmap(handle->m_devs[j].m_buffer, RING_BUF_SIZE * 2);
				close(handle->m_devs[j].m_fd);
			}
		}

		//
		// Free the memory
		//
		if(handle->m_devs != NULL)
		{
			free(handle->m_devs);
		}

		if(handle->m_pollfds != NULL)
		{
			free(handle->m_pollfds);
		}
#endif // _WIN32
	}

	if(handle->m_file_evt_buf)
	{
		free(handle->m_file_evt_buf);
	}

	// Free the process table
	if(handle->m_proclist != NULL)
	{
		scap_proc_free_table(handle);
	}

	// Free the interface list
	if(handle->m_addrlist)
	{
		scap_free_iflist(handle->m_addrlist);
	}

	// Free the user list
	if(handle->m_userlist)
	{
		scap_free_userlist(handle->m_userlist);
	}

	//
	// Release the handle
	//
	free(handle);
}

scap_os_patform scap_get_os_platform(scap_t* handle)
{
#if defined(_M_IX86) || defined(__i386__)
#ifdef linux
	return SCAP_PFORM_LINUX_I386;
#else
	return SCAP_PFORM_WINDOWS_I386;
#endif // linux
#else
#if defined(_M_X64) || defined(__AMD64__)
#ifdef linux
	return SCAP_PFORM_LINUX_X64;
#else
	return SCAP_PFORM_WINDOWS_X64;
#endif // linux
#else
	return SCAP_PFORM_UNKNOWN;
#endif // defined(_M_X64) || defined(__AMD64__)
#endif // defined(_M_IX86) || defined(__i386__)
}

uint32_t scap_get_ndevs(scap_t* handle)
{
	return handle->m_ndevs;
}

inline void get_buf_pointers(struct ppm_ring_buffer_info* bufinfo, uint32_t* phead, uint32_t* ptail, uint32_t* pread_size)
{
	*phead = bufinfo->head;
	*ptail = bufinfo->tail;

	if(*ptail > *phead)
	{
		*pread_size = RING_BUF_SIZE - *ptail + *phead;
	}
	else
	{
		*pread_size = *phead - *ptail;
	}
}

#if !defined(_WIN32) && !defined(__APPLE__)
int32_t scap_readbuf(scap_t* handle, uint32_t cpuid, bool blocking, OUT char** buf, OUT uint32_t* len)
{
	uint32_t thead;
	uint32_t ttail;
	uint32_t read_size;

	//
	// Update the tail based on the amount of data read in the *previous* call.
	// Tail is never updated when we serve the data, because we assume that the caller is using
	// the buffer we give to her until she calls us again.
	//
	ttail = handle->m_devs[cpuid].m_bufinfo->tail + handle->m_devs[cpuid].m_lastreadsize;

	//
	// Make sure every read of the old buffer is completed before we move the tail and the
	// producer (on another CPU) can start overwriting it.
	// I use this instead of asm(mfence) because it should be portable even on the weirdest
	// CPUs
	//
	__sync_synchronize();

	if(ttail < RING_BUF_SIZE)
	{
		handle->m_devs[cpuid].m_bufinfo->tail = ttail;
	}
	else
	{
		handle->m_devs[cpuid].m_bufinfo->tail = ttail - RING_BUF_SIZE;
	}

	//
	// Does the user want to block?
	//
	if(blocking)
	{
		//
		// If we are asked to operate in blocking mode, keep waiting until at least
		// MIN_USERSPACE_READ_SIZE bytes are in the buffer.
		//
		while(true)
		{
			get_buf_pointers(handle->m_devs[cpuid].m_bufinfo,
			                 &thead,
			                 &ttail,
			                 &read_size);

			if(read_size >= MIN_USERSPACE_READ_SIZE)
			{
				break;
			}

			usleep(BUFFER_EMPTY_WAIT_TIME_MS * 1000);
		}
	}
	else
	{
		//
		// If we are not asked to block, read the pointers and keep going.
		//
		get_buf_pointers(handle->m_devs[cpuid].m_bufinfo,
		                 &thead,
		                 &ttail,
		                 &read_size);
	}

	//
	// logic check
	// XXX should probably be an assertion, but for the moment we want to print some meaningful info and
	// stop the processing.
	//
	if((handle->m_devs[cpuid].m_bufinfo->tail + read_size) % RING_BUF_SIZE != thead)
	{
		snprintf(handle->m_lasterr,
		         SCAP_LASTERR_SIZE,
		         "buffer corruption. H=%u, T=%u, R=%u, S=%u (%u)",
		         thead,
		         handle->m_devs[cpuid].m_bufinfo->tail,
		         read_size,
		         RING_BUF_SIZE,
		         (handle->m_devs[cpuid].m_bufinfo->tail + read_size) % RING_BUF_SIZE);
		ASSERT(false);
		return SCAP_FAILURE;
	}

#if 0
	printf("%u)H:%u T:%u Used:%u Free:%u Size=%u\n",
	       cpuid,
	       thead,
	       ttail,
	       read_size,
	       (uint32_t)(RING_BUF_SIZE - read_size - 1),
	       (uint32_t)RING_BUF_SIZE);
#endif

	//
	// Remember read_size so we can update the tail at the next call
	//
	handle->m_devs[cpuid].m_lastreadsize = read_size;

	//
	// Return the results
	//
	*len = read_size;
	*buf = handle->m_devs[cpuid].m_buffer + ttail;

	return SCAP_SUCCESS;
}

bool check_scap_next_wait(scap_t* handle)
{
	uint32_t j;

	for(j = 0; j < handle->m_ndevs; j++)
	{
		uint32_t thead;
		uint32_t ttail;
		uint32_t read_size;

		get_buf_pointers(handle->m_devs[j].m_bufinfo, &thead, &ttail, &read_size);

		if(read_size > 100000)
		{
			return false;
		}
	}

	return true;
}

#endif // _WIN32

static inline int32_t scap_next_live(scap_t* handle, OUT scap_evt** pevent, OUT uint16_t* pcpuid)
{
#if defined(_WIN32) || defined(__APPLE__)
	//
	// this should be prevented at open time
	//
	ASSERT(false);
	return SCAP_FAILURE;
#else
	uint32_t j;
	uint64_t max_ts = 0xffffffffffffffff;
	uint64_t max_buf_size = 0;
	scap_evt* pe = NULL;
	bool waited = false;

	*pcpuid = 65535;

	for(j = 0; j < handle->m_ndevs; j++)
	{
		if(handle->m_devs[j].m_sn_len == 0)
		{
			//
			// The buffer for this CPU is fully consumed.
			// This is a good time to check if we should wait
			//
			if(handle->m_emptybuf_timeout_ms != 0)
			{
				if(check_scap_next_wait(handle) && !waited)
				{
					usleep(BUFFER_EMPTY_WAIT_TIME_MS * 1000);
					waited = true;
				}
			}

			//
			// read another buffer
			//
			int32_t res = scap_readbuf(handle,
			                           j,
			                           false,
			                           &handle->m_devs[j].m_sn_next_event,
			                           &handle->m_devs[j].m_sn_len);

			if(res != SCAP_SUCCESS)
			{
				return res;
			}
		}

		//
		// Make sure that we have data
		//
		if(handle->m_devs[j].m_sn_len != 0)
		{
			if(handle->m_devs[j].m_sn_len > max_buf_size)
			{
				max_buf_size = handle->m_devs[j].m_sn_len;
			}

			//
			// We want to consume the event with the lowest timestamp
			//
			pe = (scap_evt*)handle->m_devs[j].m_sn_next_event;

#ifdef _DEBUG
			ASSERT(pe->len == scap_event_compute_len(pe));
#endif

			if(pe->ts < max_ts)
			{
				if(pe->len > handle->m_devs[j].m_sn_len)
				{
					snprintf(handle->m_lasterr,	SCAP_LASTERR_SIZE, "scap_next buffer corruption");

					//
					// if you get the following assertion, first recompile the driver and libscap
					//
					ASSERT(false);
					return SCAP_FAILURE;
				}

				*pevent = pe;
				*pcpuid = j;
				max_ts = pe->ts;
			}
		}
	}

	//
	// Check which buffer has been picked
	//
	if(*pcpuid != 65535)
	{
		//
		// Update the pointers.
		//
		ASSERT(handle->m_devs[*pcpuid].m_sn_len >= (*pevent)->len);
		handle->m_devs[*pcpuid].m_sn_len -= (*pevent)->len;
		handle->m_devs[*pcpuid].m_sn_next_event += (*pevent)->len;

		return SCAP_SUCCESS;
	}
	else
	{
		//
		// This happens only when all the buffers are empty, which should be very rare.
		// The caller want't receive an event, but shouldn't treat this as an error and should just retry.
		//
		return SCAP_TIMEOUT;
	}
#endif
}

int32_t scap_next(scap_t* handle, OUT scap_evt** pevent, OUT uint16_t* pcpuid)
{
	int32_t res;

	if(handle->m_file)
	{
		res = scap_next_offline(handle, pevent, pcpuid);
	}
	else
	{
		res = scap_next_live(handle, pevent, pcpuid);
	}

	if(res == SCAP_SUCCESS)
	{
		handle->m_evtcnt++;
	}

	return res;
}

//
// Return the process list for the given handle
//
scap_threadinfo* scap_get_proc_table(scap_t* handle)
{
	return handle->m_proclist;
}

//
// Return the number of dropped events for the given handle
//
int32_t scap_get_stats(scap_t* handle, OUT scap_stats* stats)
{
	uint32_t j;

	stats->n_evts = 0;
	stats->n_drops = 0;
	stats->n_preemptions = 0;

	for(j = 0; j < handle->m_ndevs; j++)
	{
		stats->n_evts += handle->m_devs[j].m_bufinfo->n_evts;
		stats->n_drops += handle->m_devs[j].m_bufinfo->n_drops_buffer + 
			handle->m_devs[j].m_bufinfo->n_drops_pf;
		stats->n_preemptions += handle->m_devs[j].m_bufinfo->n_preemptions;
	}

	return SCAP_SUCCESS;
}

//
// Stop capturing the events
//
int32_t scap_stop_capture(scap_t* handle)
{
#ifdef _WIN32
	snprintf(handle->m_lasterr, SCAP_LASTERR_SIZE, "live capture not supported on windows");
	return SCAP_FAILURE;
#elif defined(__APPLE__)
	snprintf(handle->m_lasterr, SCAP_LASTERR_SIZE, "live capture not supported on OSX");
	return SCAP_FAILURE;
#else
	uint32_t j;

	//
	// Not supported for files
	//
	if(handle->m_file)
	{
		snprintf(handle->m_lasterr,	SCAP_LASTERR_SIZE, "cannot stop offline captures");
		ASSERT(false);
		return SCAP_FAILURE;
	}

	//
	// Disable capture on all the rings
	//
	for(j = 0; j < handle->m_ndevs; j++)
	{
		if(ioctl(handle->m_devs[j].m_fd, PPM_IOCTL_DISABLE_CAPTURE))
		{
			snprintf(handle->m_lasterr,	SCAP_LASTERR_SIZE, "scap_stop_capture failed for device %" PRIu32, j);
			ASSERT(false);
			return SCAP_FAILURE;
		}
	}

	//
	// Since no new data is going to be produced, we disable read waits so that the remaining data
	// can be consumd without slowdowns
	//
	handle->m_emptybuf_timeout_ms = 0;

	return SCAP_SUCCESS;
#endif // _WIN32
}

//
// Start capturing the events
//
int32_t scap_start_capture(scap_t* handle)
{
#ifdef _WIN32
	snprintf(handle->m_lasterr, SCAP_LASTERR_SIZE, "live capture non supported on windows");
	return SCAP_FAILURE;
#elif defined(__APPLE__)
	snprintf(handle->m_lasterr, SCAP_LASTERR_SIZE, "live capture non supported on OSX");
	return SCAP_FAILURE;
#else
	uint32_t j;

	//
	// Not supported for files
	//
	if(handle->m_file)
	{
		snprintf(handle->m_lasterr,	SCAP_LASTERR_SIZE, "cannot start offline captures");
		ASSERT(false);
		return SCAP_FAILURE;
	}

	//
	// Enable capture on all the rings
	//
	for(j = 0; j < handle->m_ndevs; j++)
	{
		if(ioctl(handle->m_devs[j].m_fd, PPM_IOCTL_ENABLE_CAPTURE))
		{
			snprintf(handle->m_lasterr,	SCAP_LASTERR_SIZE, "scap_start_capture failed for device %" PRIu32, j);
			ASSERT(false);
			return SCAP_FAILURE;
		}
	}

	return SCAP_SUCCESS;
#endif // _WIN32
}

#if !defined (_WIN32) && !defined(__APPLE__)
static int32_t scap_set_dropping_mode(scap_t* handle, int request, uint32_t sampling_ratio)
{
	//	
	// Not supported for files
	//
	if(handle->m_file)
	{
		snprintf(handle->m_lasterr,	SCAP_LASTERR_SIZE, "dropping mode not supported on offline captures");
		ASSERT(false);
		return SCAP_FAILURE;
	}

	if(handle->m_ndevs)
	{
		if(ioctl(handle->m_devs[0].m_fd, request, sampling_ratio))
		{
			snprintf(handle->m_lasterr,	SCAP_LASTERR_SIZE, "%s failed", __FUNCTION__);
			ASSERT(false);
			return SCAP_FAILURE;
		}		
	}

	return SCAP_SUCCESS;
}
#endif

int32_t scap_stop_dropping_mode(scap_t* handle)
{
#ifdef _WIN32
	snprintf(handle->m_lasterr, SCAP_LASTERR_SIZE, "live capture not supported on windows");
	return SCAP_FAILURE;
#elif defined(__APPLE__)
	snprintf(handle->m_lasterr, SCAP_LASTERR_SIZE, "live capture not supported on OSX");
	return SCAP_FAILURE;
#else
	return scap_set_dropping_mode(handle, PPM_IOCTL_DISABLE_DROPPING_MODE, 0);
#endif
}

int32_t scap_start_dropping_mode(scap_t* handle, uint32_t sampling_ratio)
{
#ifdef _WIN32
	snprintf(handle->m_lasterr, SCAP_LASTERR_SIZE, "live capture not supported on windows");
	return SCAP_FAILURE;
#elif defined(__APPLE__)
	snprintf(handle->m_lasterr, SCAP_LASTERR_SIZE, "live capture not supported on OSX");
	return SCAP_FAILURE;
#else
	return scap_set_dropping_mode(handle, PPM_IOCTL_ENABLE_DROPPING_MODE, sampling_ratio);
#endif
}

//
// Return the list of device addresses
//
scap_addrlist* scap_get_ifaddr_list(scap_t* handle)
{
	return handle->m_addrlist;
}

//
// Return the list of machine users
//
scap_userlist* scap_get_user_list(scap_t* handle)
{
	return handle->m_userlist;
}

//
// Get the machine information
//
const scap_machine_info* scap_get_machine_info(scap_t* handle)
{
	if(handle->m_machine_info.num_cpus != (uint32_t)-1)
	{
		return (const scap_machine_info*)&handle->m_machine_info;
	}
	else
	{
		//
		// Reading from a file with no process info block
		//
		return NULL;
	}
}

int32_t scap_set_snaplen(scap_t* handle, uint32_t snaplen)
{
	//
	// Not supported on files
	//
	if(handle->m_file)
	{
		snprintf(handle->m_lasterr,	SCAP_LASTERR_SIZE, "dropping mode not supported on offline captures");
		ASSERT(false);
		return SCAP_FAILURE;
	}

#ifdef _WIN32
	snprintf(handle->m_lasterr, SCAP_LASTERR_SIZE, "live capture not supported on windows");
	return SCAP_FAILURE;
#elif defined(__APPLE__)
	snprintf(handle->m_lasterr, SCAP_LASTERR_SIZE, "live capture not supported on OSX");
	return SCAP_FAILURE;
#else
	//
	// Tell the driver to change the snaplen
	//
	if(ioctl(handle->m_devs[0].m_fd, PPM_IOCTL_SET_SNAPLEN, snaplen))
	{
		snprintf(handle->m_lasterr,	SCAP_LASTERR_SIZE, "scap_set_snaplen failed");
		ASSERT(false);
		return SCAP_FAILURE;
	}

	return SCAP_SUCCESS;
#endif
}
