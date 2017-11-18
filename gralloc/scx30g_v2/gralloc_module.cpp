/*
 * Copyright (C) 2010 ARM Limited. All rights reserved.
 *
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <pthread.h>

#include <cutils/log.h>
#include <cutils/atomic.h>
#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#include "gralloc_priv.h"
#include "alloc_device.h"
#include "framebuffer_device.h"

#if GRALLOC_ARM_DMA_BUF_MODULE
#include <linux/ion.h>
#include <ion/ion.h>
#include <sys/mman.h>
#endif

static pthread_mutex_t s_map_lock = PTHREAD_MUTEX_INITIALIZER;

static int gralloc_device_open(const hw_module_t *module, const char *name, hw_device_t **device)
{
	int status = -EINVAL;

	if (!strncmp(name, GRALLOC_HARDWARE_GPU0, MALI_GRALLOC_HARDWARE_MAX_STR_LEN))
	{
		status = alloc_device_open(module, name, device);
	}
	else if (!strncmp(name, GRALLOC_HARDWARE_FB0, MALI_GRALLOC_HARDWARE_MAX_STR_LEN))
	{
		status = framebuffer_device_open(module, name, device);
	}

	return status;
}

static int gralloc_register_buffer(gralloc_module_t const *module, buffer_handle_t handle)
{
	MALI_IGNORE(module);

	if (private_handle_t::validate(handle) < 0)
	{
		AERR("Registering invalid buffer 0x%p, returning error", handle);
		return -EINVAL;
	}

	// if this handle was created in this process, then we keep it as is.
	private_handle_t *hnd = (private_handle_t *)handle;

	ALOGD_IF(mDebug,"register buffer  handle:%p ion_hnd:0x%x",handle,hnd->ion_hnd);

	int retval = -EINVAL;

	pthread_mutex_lock(&s_map_lock);

	hnd->pid = getpid();

	if (hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER)
	{
		AERR("Can't register buffer 0x%p as it is a framebuffer", handle);
	}
	else if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)
	{
#if GRALLOC_ARM_DMA_BUF_MODULE
		int ret;
		unsigned char *mappedAddress;
		size_t size = hnd->size;
		hw_module_t *pmodule = NULL;
		private_module_t *m = (private_module_t*)module;

		/* the test condition is set to m->ion_client <= 0 here, because:
		 * 1) module structure are initialized to 0 if no initial value is applied
		 * 2) a second user process should get a ion fd greater than 0.
		 */
		if (m->ion_client <= 0)
		{
			/* a second user process must obtain a client handle first via ion_open before it can obtain the shared ion buffer*/
			m->ion_client = ion_open();

			if (m->ion_client < 0)
			{
				AERR("Could not open ion device for handle: 0x%p", hnd);
				retval = -errno;
				goto cleanup;
			}
		}

		mappedAddress = (unsigned char *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, hnd->share_fd, 0);

		if (MAP_FAILED == mappedAddress)
		{
			AERR("mmap( share_fd:%d ) failed with %s",  hnd->share_fd, strerror(errno));
			retval = -errno;
			goto cleanup;
		}

		hnd->base = mappedAddress + hnd->offset;
		hnd->lockState &= ~(private_handle_t::LOCK_STATE_UNREGISTERED);
		
		pthread_mutex_unlock(&s_map_lock);
		return 0;
#endif
	}
	else
	{
		AERR("registering non-UMP buffer not supported. flags = %d", hnd->flags);
	}

cleanup:
	pthread_mutex_unlock(&s_map_lock);
	return retval;
}

static void unmap_buffer(private_handle_t *hnd)
{
	if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)
	{
#if GRALLOC_ARM_DMA_BUF_MODULE
		void *base = (void *)hnd->base;
		size_t size = hnd->size;

		if (munmap(base, size) < 0)
		{
			AERR("Could not munmap base:0x%p size:%lu '%s'", base, (unsigned long)size, strerror(errno));
		}

#else
		AERR("Can't unregister DMA_BUF buffer for hnd %p. Not supported", hnd);
#endif

	}
	else
	{
		AERR("Unregistering unknown buffer is not supported. Flags = %d", hnd->flags);
	}

	hnd->base = 0;
	hnd->lockState = 0;
	hnd->writeOwner = 0;
}

static int gralloc_unregister_buffer(gralloc_module_t const *module, buffer_handle_t handle)
{
	MALI_IGNORE(module);

	if (private_handle_t::validate(handle) < 0)
	{
		AERR("unregistering invalid buffer 0x%p, returning error", handle);
		return -EINVAL;
	}

	private_handle_t *hnd = (private_handle_t *)handle;

	ALOGD_IF(mDebug,"unregister buffer  handle:%p ion_hnd:0x%x",handle,hnd->ion_hnd);

	AERR_IF(hnd->lockState & private_handle_t::LOCK_STATE_READ_MASK, "[unregister] handle %p still locked (state=%08x),ion_hnd=0x%x", hnd, hnd->lockState,hnd->ion_hnd);

	if (hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER)
	{
		AERR("Can't unregister buffer 0x%p as it is a framebuffer", handle);
	}
	else if (hnd->pid == getpid()) // never unmap buffers that were not registered in this process
	{
		pthread_mutex_lock(&s_map_lock);

		hnd->lockState &= ~(private_handle_t::LOCK_STATE_MAPPED);

		/* if handle is still locked, the unmapping would not happen until unlocked*/
		if (!(hnd->lockState & private_handle_t::LOCK_STATE_WRITE))
		{
			unmap_buffer(hnd);
		}

		hnd->lockState |= private_handle_t::LOCK_STATE_UNREGISTERED;

		pthread_mutex_unlock(&s_map_lock);
	}
	else
	{
		AERR("Trying to unregister buffer 0x%p from process %d that was not created in current process: %d", hnd, hnd->pid, getpid());
	}

	return 0;
}

static int gralloc_lock(gralloc_module_t const *module, buffer_handle_t handle, int usage, int l, int t, int w, int h, void **vaddr)
{
	if (private_handle_t::validate(handle) < 0)
	{
		AERR("Locking invalid buffer 0x%p, returning error", handle);
		return -EINVAL;
	}

	private_handle_t *hnd = (private_handle_t *)handle;

	pthread_mutex_lock(&s_map_lock);
	
	if (hnd->lockState & private_handle_t::LOCK_STATE_UNREGISTERED)
	{
		AERR("Locking on an unregistered buffer 0x%p, returning error", hnd);
		pthread_mutex_unlock(&s_map_lock);
		return -EINVAL;
	}

	if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION)
	{
		hnd->writeOwner = usage & GRALLOC_USAGE_SW_WRITE_MASK;
	}

	hnd->lockState |= private_handle_t::LOCK_STATE_WRITE;

	pthread_mutex_unlock(&s_map_lock);

	if (usage & (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK))
	{
		*vaddr = (void *)hnd->base;
#if GRALLOC_ARM_DMA_BUF_MODULE
		private_module_t *m = (private_module_t*)module;

		ion_invalidate_fd(m->ion_client, hnd->share_fd);
#endif
	}

	MALI_IGNORE(l);
	MALI_IGNORE(t);
	MALI_IGNORE(w);
	MALI_IGNORE(h);
	return 0;
}



static int gralloc_lock_ycbcr(struct gralloc_module_t const* module,
		buffer_handle_t handle, int usage,
		int l, int t, int w, int h,
		struct android_ycbcr *ycbcr)
{
	MALI_IGNORE(module);

	if (private_handle_t::validate(handle) < 0)
	{
		AERR("Locking invalid buffer 0x%p, returning error", handle );
		return -EINVAL;
	}
	private_handle_t* hnd = (private_handle_t*)handle;
	int ystride;
	int err=0;

	switch (hnd->format) {
		case HAL_PIXEL_FORMAT_YCrCb_420_SP:
		case HAL_PIXEL_FORMAT_YCbCr_420_888:
		/*HAL_PIXEL_FORMAT_YCbCr_420_888 is a flexible yuv format and sprd treat
		 * it as YCrCb 420 sp(be used in DCAM HAL)*/
			ystride = GRALLOC_ALIGN(hnd->width, 16);
			ycbcr->y  = (void*)hnd->base;
			ycbcr->cr = (void*)((uintptr_t)hnd->base + ystride * hnd->height);
			ycbcr->cb = (void*)((uintptr_t)hnd->base + ystride * hnd->height + 1);
			ycbcr->ystride = ystride;
			ycbcr->cstride = ystride;
			ycbcr->chroma_step = 2;
			memset(ycbcr->reserved, 0, sizeof(ycbcr->reserved));
			break;
                case HAL_PIXEL_FORMAT_YV12:
                        ystride = GRALLOC_ALIGN(hnd->width, 128);
                        ycbcr->y  = (void*)hnd->base;
                        ycbcr->cr = (void*)((uintptr_t)hnd->base + ystride * hnd->height);
                        ycbcr->cb = (void*)((uintptr_t)hnd->base + ystride * hnd->height + GRALLOC_ALIGN(ystride/2, 16) * (hnd->height/2));
                        ycbcr->ystride = ystride;
                        ycbcr->cstride = GRALLOC_ALIGN(ystride/2, 16);
                        ycbcr->chroma_step = 1;
                        memset(ycbcr->reserved, 0, sizeof(ycbcr->reserved));
			break;
		case HAL_PIXEL_FORMAT_YCbCr_420_SP:
			ystride = GRALLOC_ALIGN(hnd->width, 16);
			ycbcr->y  = (void*)hnd->base;
			ycbcr->cb = (void*)((uintptr_t)hnd->base + ystride * hnd->height);
			ycbcr->cr = (void*)((uintptr_t)hnd->base + ystride * hnd->height + 1);
			ycbcr->ystride = ystride;
			ycbcr->cstride = ystride;
			ycbcr->chroma_step = 2;
			memset(ycbcr->reserved, 0, sizeof(ycbcr->reserved));
			break;
		default:
			ALOGD("%s: Invalid format passed: 0x%x", __FUNCTION__,
				  hnd->format);
			err = -EINVAL;
	}
	MALI_IGNORE(usage);
	MALI_IGNORE(l);
	MALI_IGNORE(t);
	MALI_IGNORE(w);
	MALI_IGNORE(h);
	return err;
}



static int gralloc_unlock(gralloc_module_t const* module, buffer_handle_t handle)
{
	MALI_IGNORE(module);

	if (private_handle_t::validate(handle) < 0)
	{
		AERR("Unlocking invalid buffer 0x%p, returning error", handle);
		return -EINVAL;
	}

	private_handle_t *hnd = (private_handle_t *)handle;
	int32_t current_value;
	int32_t new_value;
	int retry;

	if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION && hnd->writeOwner)
	{
#if GRALLOC_ARM_DMA_BUF_MODULE
		private_module_t *m = (private_module_t*)module;

		ion_sync_fd(m->ion_client, hnd->share_fd);
#endif
	}

	pthread_mutex_lock(&s_map_lock);

	hnd->lockState &= ~(private_handle_t::LOCK_STATE_WRITE);

	/* if the handle has already been unregistered, unmap it here*/
	if (hnd->lockState & private_handle_t::LOCK_STATE_UNREGISTERED)
	{
		unmap_buffer(hnd);
	}

	pthread_mutex_unlock(&s_map_lock);

	return 0;
}

// There is one global instance of the module

static struct hw_module_methods_t gralloc_module_methods =
{
open:
	gralloc_device_open
};

private_module_t::private_module_t()
{
#define INIT_ZERO(obj) (memset(&(obj),0,sizeof((obj))))

	base.common.tag = HARDWARE_MODULE_TAG;
	base.common.version_major = 1;
	base.common.version_minor = 0;
	base.common.id = GRALLOC_HARDWARE_MODULE_ID;
	base.common.name = "Graphics Memory Allocator Module";
	base.common.author = "ARM Ltd.";
	base.common.methods = &gralloc_module_methods;
	base.common.dso = NULL;
	INIT_ZERO(base.common.reserved);

	base.registerBuffer = gralloc_register_buffer;
	base.unregisterBuffer = gralloc_unregister_buffer;
	base.lock = gralloc_lock;
	base.lock_ycbcr = gralloc_lock_ycbcr;
	base.unlock = gralloc_unlock;
	base.perform = NULL;
	INIT_ZERO(base.reserved_proc);

	framebuffer = NULL;
	psCtx = NULL;
	flags = 0;
	numBuffers = 0;
	bufferMask = 0;
	pthread_mutex_init(&(lock), NULL);
	currentBuffer = NULL;
	INIT_ZERO(info);
	INIT_ZERO(finfo);
	xdpi = 0.0f;
	ydpi = 0.0f;
	fps = 0.0f;

#undef INIT_ZERO
};

/*
 * HAL_MODULE_INFO_SYM will be initialized using the default constructor
 * implemented above
 */
struct private_module_t HAL_MODULE_INFO_SYM;

