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

#ifndef __GENWQE_VPD_H__
#define __GENWQE_VPD_H__

/**
 * @file	genwqe_vpd.h
 * @brief	application library for hardware access
 * @date	03/09/2012
 *
 * The GenWQE PCIe card provides the ability to speed up tasks by
 * offloading data processing. It provides a generic work queue engine
 * (GenWQE) which is used to pass the requests to the PCIe card. The
 * requests are to be passed in form of DDCB commands (Device Driver
 * Control Blocks). The device driver is allocating the next free DDCB
 * from the hardware queue and converts the DDCB-request defined in
 * this file into a DDCB. Once the request is passed to the card, the
 * process/thread will sleep and will be awoken once the request is
 * finished with our without success or a timeout condition occurred.
 *
 * IBM Accelerator Family 'GenWQE'
 */

#ifdef __cplusplus
extern "C" {
#endif

/* VPD layout for GENWQE */
struct vpd_layout {
    const char    *label;
    const int     length;
    const char    *mode;
};

#define VPD_VERSION 0x102
/* Reference Table for VPD layout 102 */
static const struct vpd_layout vpd_ref_102[] = {
    {.label = "RV", .length =  2, .mode="X"},
    {.label = "PN", .length =  7, .mode="A"},
    {.label = "EC", .length =  7, .mode="A"},
    {.label = "FN", .length =  7, .mode="A"},
    {.label = "SN", .length = 13, .mode="A"},
    {.label = "FC", .length =  5, .mode="A"},
    {.label = "CC", .length =  4, .mode="A"},
    {.label = "M0", .length =  6, .mode="X"},
    {.label = "M1", .length =  6, .mode="X"},
    {.label = "CS", .length =  4, .mode="X"}    // Must be last one in file
};

#define LINES_IN_VPD    (sizeof(vpd_ref_102)/sizeof(struct vpd_layout))
#define VPD_SIZE        (2*LINES_IN_VPD + LINES_IN_VPD + LINES_IN_VPD + \
                        2+7+7+7+13+5+4+6+6+4)
#define	GENWQE_VPD_BUFFER_SIZE	(64*1024)

union swap_me {
    uint32_t ui32;
    struct {
        uint16_t uw16[2];
    } WORD;
    struct {
        uint8_t ub8[4];
    } BYTE;
};

/*
 * X^32+X^26+X^23+X^22+X^16+X^12+X^11+X^10+X^8+X^7+X^5+X^4+X^2+X^1+X^0
 */
#define CRC32_POLYNOMIAL    0x04c11db7
#define CRC32_INIT_SEED     0xffffffff

void genwqe_crc32_setup_lut(void);
uint32_t genwqe_crc32_gen(uint8_t *buff, size_t len, uint32_t init);

/* 2 Convert functions for VPD */
bool bin_2_csv(FILE *op, int fs, uint8_t *buffer);
bool csv_2_bin(FILE *ip, uint8_t *buffer, int *size,
                    uint32_t *crc32_result,
                    uint32_t *crc32_from_csv);

#ifdef __cplusplus
}
#endif

#endif	/* __GENWQE_VPD_H__ */
