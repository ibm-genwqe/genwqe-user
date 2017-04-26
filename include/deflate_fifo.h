/*
 * Copyright 2015 International Business Machines
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

#ifndef __LIBZEDC_FIFO_H__
#define __LIBZEDC_FIFO_H__

/*
 * To store temporary data the deflate code uses the FIFO data
 * structure defined in this file. Storing data is required if the
 * output buffer in the zedc_stream struct is not sufficient to store
 * the produced data. This can happen e.g. for the ZLIB/GZIP header
 * data or the ADLER32 and CRC32/data-size trailer at the end of an
 * RFC1950, RFC1952 data stream. In case of RFC1951 data the header
 * and the end of stream symbols can be affected. If the last symbol
 * in an input stream produces more output bytes than the output
 * buffer can store, we also use this FIFO to temporarilly store the
 * data before it goes into the user provided output buffer.
 */

#include <stdint.h>
#include <string.h>
#include <assert.h>

/* Must be n^2 and large engough to keep some spare bytes. */
#define ZEDC_FIFO_SIZE		256
#define ZEDC_FIFO_MASK		(ZEDC_FIFO_SIZE - 1)

#ifndef ARRAY_SIZE
#  define ARRAY_SIZE(a)		(sizeof((a)) / sizeof((a)[0]))
#endif

struct zedc_fifo {
	unsigned int	push;	/* push into FIFO here */
	unsigned int	pop;	/* pop from FIFO here */
	uint8_t		fifo[ZEDC_FIFO_SIZE];  /* FIFO storage */
};

static inline void fifo_init(struct zedc_fifo *fifo)
{
	memset(fifo->fifo, 0x00, ZEDC_FIFO_SIZE);
	fifo->pop = fifo->push = 0;
}

static inline int fifo_empty(struct zedc_fifo *fifo)
{
	return (fifo->pop == fifo->push);
}

static inline unsigned int fifo_used(struct zedc_fifo *fifo)
{
	return ((fifo->push - fifo->pop) & ZEDC_FIFO_MASK);
}

static inline unsigned int fifo_free(struct zedc_fifo *fifo)
{
	return ZEDC_FIFO_SIZE - fifo_used(fifo) - 1; /* keep 1 more free */
}

static inline int fifo_push(struct zedc_fifo *fifo, uint8_t data)
{
	if (fifo_free(fifo) < 1)
		return 0;

	fifo->fifo[fifo->push] = data;
	fifo->push = (fifo->push + 1) & ZEDC_FIFO_MASK;
	return 1;
}

static inline int fifo_push32(struct zedc_fifo *fifo, uint32_t data)
{
	unsigned int i;
	union {
		uint32_t u32;
		uint8_t u8[4];
	} d;

	if (fifo_free(fifo) < ARRAY_SIZE(d.u8))
		return 0;

	d.u32 = data;
	for (i = 0; i < ARRAY_SIZE(d.u8); i++) {
		fifo->fifo[fifo->push] = d.u8[i];
		fifo->push = (fifo->push + 1) & ZEDC_FIFO_MASK;
	}
	return 1;
}

static inline int fifo_pop(struct zedc_fifo *fifo, uint8_t *data)
{
	if (fifo_empty(fifo))
		return 0;

	*data = fifo->fifo[fifo->pop];
	fifo->pop = (fifo->pop + 1) & ZEDC_FIFO_MASK;
	return 1;
}

static inline int fifo_pop16(struct zedc_fifo *fifo, uint16_t *data)
{
	unsigned int i;
	union {
		uint16_t u16;
		uint8_t u8[2];
	} d;

	if (fifo_used(fifo) < ARRAY_SIZE(d.u8))
		return 0;

	for (i = 0; i < ARRAY_SIZE(d.u8); i++)
		fifo_pop(fifo, &d.u8[i]);

	*data = d.u16;
	return 1;
}

#endif /* __LIBZEDC_FIFO_H__ */
