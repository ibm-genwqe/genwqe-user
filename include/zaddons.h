/*
 * Copyright 2014,2015 International Business Machines
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __ZADDONS_H__
#define __ZADDONS_H__

/*
 * Extensions of our hardware accelerated zlib implementation. Use
 * with care, since they are not part of the official zlib.h
 * interface.
 */

enum zlib_impl {
	ZLIB_SW_IMPL = 0x00,
	ZLIB_HW_IMPL = 0x01,
	ZLIB_MAX_IMPL = 0x02,
	ZLIB_IMPL_MASK = 0x0f,

	/* Flags which influence special optimization behavior */
	ZLIB_FLAG_USE_FLAT_BUFFERS = 0x10, /* GenWQE only */
	ZLIB_FLAG_CACHE_HANDLES = 0x20,	   /* GenWQE only, fails for CAPI */
	ZLIB_FLAG_OMIT_LAST_DICT = 0x40,   /* Useful for cases like Genomics */
	ZLIB_FLAG_USE_POLLING = 0x80,  /* Use polling mode only for CAPI */
	ZLIB_FLAG_DISABLE_CV_FOR_Z_STREAM_END = 0x100,
};

/**
 * zlib_set_inflate_impl() - Set default implementation for inflate
 *
 * @impl: Either ZLIB_SW_IMPL or ZLIB_HW_IMPL.
 *
 * We can enforce trying hardware usage by setting
 * ZLIB_HW_IMPL. Nevertheless if there is no hardware available
 * e.g. driver not installed, no card plugged, or access rights wrong,
 * the software version will be used as fallback.
 */
void zlib_set_inflate_impl(enum zlib_impl impl);

/**
 * zlib_set_deflate_impl() - Set default implementation for deflate
 *
 * @impl: Either ZLIB_SW_IMPL or ZLIB_HW_IMPL.
 *
 * We can enforce trying hardware usage by setting
 * ZLIB_HW_IMPL. Nevertheless if there is no hardware available
 * e.g. driver not installed, no card plugged, or access rights wrong,
 * the software version will be used as fallback.
 */
void zlib_set_deflate_impl(enum zlib_impl impl);

/**
 * zlib_set_accelerator() - Set accelerator type to be used
 *
 * @accel:          GENWQE or CAPI
 * @card_no:        card id or -1 for automatic card selection
 *
 * We support different types of hardware acceleration
 * devices. Examples are our PCIe based GenWQE accelerator or the CAPI
 * implementation for IBM System p.
 */
void zlib_set_accelerator(const char *accel, int card_no);

#endif	/* __ZADDONS_H__ */
