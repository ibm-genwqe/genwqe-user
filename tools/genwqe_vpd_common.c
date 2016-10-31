/*
 * Copyright 2015, International Business Machines
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

/**
 *
 * VPD utility for GENWQE Project
 *
 * VPD Tools from CSV file to binary file
 *      used for making the vpd bin file from a cvs file.
 *      The input file format is defined and fix.
 *      This tool can alos convert a binary file back to
 *      the original CSV file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <getopt.h>
#include <stdbool.h>
#include <unistd.h>
#include <ctype.h>
#include <asm/byteorder.h>

#include "genwqe_tools.h"
#include "genwqe_vpd.h"

#define MAX_LINE 512
#define GENWQE_VPD_BUFFER_SIZE (64*1024)

extern int _dbg_flag;
static uint32_t genwqe_crc32_lut[256];

// Search for this in collum 6 in order to add crc32
static char crc_token[]={"CS"};

void genwqe_crc32_setup_lut(void)
{
	int i, j;
	uint32_t crc;

	for (i = 0;  i < 256;  i++) {
		crc = i << 24;
		for ( j = 0;  j < 8;  j++ ) {
			if (crc & 0x80000000)
				crc = (crc << 1) ^ CRC32_POLYNOMIAL;
			else
				crc = (crc << 1);
		}
		genwqe_crc32_lut[i] = crc;
	}
}

uint32_t genwqe_crc32_gen(uint8_t *buff, size_t len, uint32_t init)
{
	int i;
	uint32_t crc;

	crc = init;
	while(len--) {
		i = ((crc >> 24) ^ *buff++) & 0xFF;
		crc = (crc << 8) ^ genwqe_crc32_lut[i];
	}
	return crc;
}

static uint8_t a2h(char c)
{
	if (c >= 'A')
		c -= 7;
	c = c & 0x0f;
	return (uint8_t)c;
}

/**
 * Converts BIN to CSV file
 *
 *  op:     Output File Pointer
 *  fs:     Size of input data in buffer
 *  buffer: a buffer to read the data
 */
bool bin_2_csv(FILE *op, int fs, uint8_t *buffer)
{
	char mode;
	char label[4];
	int length;
	int n = 0;
	int vpd_check_line = 0;
	union swap_me swap;

	swap.ui32 = 0;
	while (fs) {
		label[0] = *buffer++;
		label[1] = *buffer++;
		label[2] = 0;
		length = *buffer++;
		mode = *buffer++;
		if (0 != strcmp(label, vpd_ref_102[vpd_check_line].label)) {
			pr_err("Binary File Got: %s Expect: %s\n",
			       label, vpd_ref_102[vpd_check_line].label);
			return false;
		}
		if (length != vpd_ref_102[vpd_check_line].length) {
			pr_err("Binary File Got: %d Expect: %d\n",
			       length, vpd_ref_102[vpd_check_line].length);
			return false;
		}
		if (mode != *vpd_ref_102[vpd_check_line].mode) {
			pr_err("Binary File Got: %c Expect: %c\n",
			       mode, *vpd_ref_102[vpd_check_line].mode);
			return false;
		}
		fprintf(op, "\"%s\",%d,\"%c\",", label, length, mode);
		if ('A' == mode) {
			fprintf(op, "\"");
			for(n = 0; n < length; n++) {
				if (isprint(*buffer)) // Check if valid, ignore 0
					fprintf(op, "%c", *buffer);
				buffer++;
			}
			fprintf(op, "\"");
		}
		if ('X' == mode) {
			if(0 == vpd_check_line) {
				/* the first Line must have the correct Version */
				swap.BYTE.ub8[2] = *buffer;
				swap.BYTE.ub8[3] = *(buffer+1);
				swap.ui32 = __be32_to_cpu(swap.ui32);
				if (swap.ui32 != VPD_VERSION) {
					pr_err("Wrong Version: %x Expect: %x\n",
					       swap.ui32, VPD_VERSION);
					return false;
				}
			}
			for(n = 0; n < length; n++)
				fprintf(op, "%2.2x", *buffer++);
		}
		fprintf(op, ",\n"); // Terminate line with ,
		fs -= (4 + length);
		vpd_check_line++;
	}
	/* Check if i did match all tokens */
	pr_info("Check for %d of %d tokens in bin file.\n",
		vpd_check_line, (int)LINES_IN_VPD);
	if (LINES_IN_VPD != vpd_check_line) {
		pr_err("%d of %d tokens fond in input.\n",
		       vpd_check_line, (int)LINES_IN_VPD);
		return false;
	}
	return true;
}

/**
 *
 * Converts CSV file to binary file
 *
 * Returns true if no errors
 *
 *  ip:             Binary Input file
 *  buffer:         Ptr. to Buffer for input data
 *  size:           Ptr. to size of data in buffer
 *  crc32_result:   Ptr. to CRC of data from input stream (is 0 if match)
 *  crc32_from_csv: Ptr. to the CRC value i found in the CVS file
 */
bool csv_2_bin(FILE *ip,
	       uint8_t *buffer,
	       int *size,
	       uint32_t *crc32_result,
	       uint32_t *crc32_from_csv)
{
	char line[MAX_LINE];
	char token[MAX_LINE];
	uint8_t data[MAX_LINE];
	unsigned line_nr = 0;
	uint32_t crc32;
	uint8_t mode = 0;
	int i, j;
	bool parse_error;
	bool get_crc_value = false;
	uint8_t ln, hn; // Low nibble and high nibble for converter
	int data_len, write_size, seek_offset, vpd_check_line;
	union swap_me csv_crc;
	union swap_me swap;
	int good_lines = 0;

	write_size = 0;
	seek_offset = 0;
	crc32 = CRC32_INIT_SEED;
	vpd_check_line = 0;
	swap.ui32 = 0;

	while (NULL != fgets(line, MAX_LINE, ip)) {
		int field_num = 0;
		int num_fields;
		int line_len;
		int n;

		++line_nr;
		num_fields = 0;
		memset(token, 0, MAX_LINE);
		memset(data, 0, MAX_LINE);
		j = 0;
		line[strlen(line) - 2] = '\0'; /* remove newline character */

		line_len = (int)strlen(line);
		pr_dbg("Line (#%d) %d: <%s>\n", line_nr, line_len, line);
		field_num = 0;
		parse_error = false;

		for (i = 0; i <= line_len; i++) {
			switch (line[i]) {
			case ',':
			case '\0':  // End of Line
				switch (field_num) {
				case 0: /* 2 Bytes KEY */
					n = strlen(&token[0]);
					if (2 == n) {
						data[0] = token[0];
						data[1] = token[1];
						if (0 == strncmp(crc_token,
								 token, 2))
							get_crc_value = true;   // Set flag to read in crc value
						if (0 != strcmp(&token[0], vpd_ref_102[vpd_check_line].label))
							parse_error = true;
					} else {
						i = line_len;
						// Exit this line only, no Error, just ignore it
						break;
					}
					break;
				case 1: /* 1 Byte LEN */
					parse_error = true;
					n = sscanf(&token[0], "%d", &data_len);
					if (1 == n) {
						if (data_len == vpd_ref_102[vpd_check_line].length) {
							data[2] = data_len;
							write_size = 2 + 1 + 1 + data_len; // how many bytes to write
							parse_error = false;
						}
					}
					break;
				case 2: /* 1 Byte Mode can be A or X */
					parse_error = true;
					if (1 == strlen(&token[0])) {
						mode = token[0];
						if (0 == strcmp(&token[0], vpd_ref_102[vpd_check_line].mode))
							parse_error = false;
					}
					break;
				case 3: /* Data */
					parse_error = true;
					data[3] = mode; // Save mode in Output data
					n = strlen(&token[0]);
					if ('A' == mode) {  // ASCII mode
						if (n <= data_len) {
							memcpy(&data[4], token, n);
							parse_error = false;
						}
					} else if ('X' == mode) {   // HEX Mode
						if (n <= (2* data_len)) {
							/* start to convert from the end of the line */
							while (n) {
								n--;
								ln = a2h(token[n]);
								if (n) {
									n--;
									hn = a2h(token[n]);
									ln |= hn << 4;  // combine
								}
								data[3+data_len] = ln;
								data_len--;
							}
							parse_error = false;
							if (0 == vpd_check_line) {
								/* the first Line must have the correct Version */
								swap.BYTE.ub8[2] = data[4];
								swap.BYTE.ub8[3] = data[5];
								swap.ui32 = __be32_to_cpu(swap.ui32);
								if (swap.ui32 != VPD_VERSION) {
									pr_err("Wrong VPD Version found %x\n", swap.ui32);
									parse_error = true;
								}
							}
							if (get_crc_value) {
								/* Get CRC from source data */
								get_crc_value = false;
								csv_crc.BYTE.ub8[0] = data[4];
								csv_crc.BYTE.ub8[1] = data[5];
								csv_crc.BYTE.ub8[2] = data[6];
								csv_crc.BYTE.ub8[3] = data[7];
								csv_crc.ui32 = __be32_to_cpu(csv_crc.ui32);
							}
						}
					}
					if (!parse_error)
						good_lines++;
					break;
				default:
					break;
				}
				if (!parse_error) {
					j = 0;
					token[j] = '\0';
					field_num++;
					num_fields++;
				}
				break;
			default:
				if (0x22 == line[i])    // Skip "
					continue;
				token[j++] = line[i];
				token[j] = '\0';
				break;
			}
			if (parse_error)
				break;
		}

		if (parse_error) {
			pr_err("Line# %d Field: %d Syndrom: <%s>\n",
			       line_nr, field_num, token);
			return (false);
		}
		if (num_fields < 4) {
			pr_dbg("Skip Line# %d\n", line_nr);
			continue;
		} else {
			/* Add Data to Output buffer */
			pr_dbg("Line# %d OK Num Fields %d Offset: %d Size: %d\n",
			       line_nr, num_fields, seek_offset, write_size);
			memcpy(&buffer[seek_offset], data, write_size);
			seek_offset += write_size;
			crc32 = genwqe_crc32_gen(data, write_size, crc32);
			if (seek_offset > GENWQE_VPD_BUFFER_SIZE) {
				pr_err("Exit due to out of buffer size %d\n",
				       seek_offset);
				parse_error = true;
				break;
			}
		}
		vpd_check_line++;
	}
	*size = seek_offset;
	*crc32_result = crc32;
	*crc32_from_csv = csv_crc.ui32;
	// Check if all is ok (FIXME: do i need to check the size also ??
	// if (VPD_SIZE != seek_offset)
	//     return (false);
	if ((parse_error) || (LINES_IN_VPD != good_lines))
		return (false);
	return (true);
}
