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
 * Convert from CSV file to binary file used for making the vpd bin
 * file from a cvs file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define MAX_LINE 512

#define DEBUG_PRINTF(...) printf(__VA_ARGS__)

/* Command line arguments */
static int arg_index;
static int arg_count;
static char *arg_values[100];

/*
 * Standard CRC-32 Polynomial of:
 *  x^32 + x^26 + x^23 + x^22 + x^16 + x^12 + x^11 + x^10 + x^8 + x^7 +
 *  x^5 + x^4 + x^2 + x^1+1
 */
typedef enum { FALSE = 0, TRUE = 1 } BOOL;

/**
 * Global parameters used by the utilities
 *
 */
static char input_fn[MAX_LINE];
static char output_fn[MAX_LINE];
static BOOL verbose_flag = FALSE; /** TRUE if verbose messages output */
static BOOL add_crc = FALSE;

typedef enum _SOS_ENDIANNESS {
	SOS_ENDIANNESS_BIG = 10,
	SOS_ENDIANNESS_LITTLE
} SOS_ENDIANNESS;

/**
 * Return endianness of the system we are running on
 *
 */
static SOS_ENDIANNESS sos0_endianness(void)
{
	int16_t x;
	int16_t y;

	/* Set up an integer */
	x = 1;
	/* Get the first byte of the integer address range */
	y = *((char *) &x);
	if (y == 1)
		return (SOS_ENDIANNESS_LITTLE);
	else
		return (SOS_ENDIANNESS_BIG);
}

/**
 * Forces a 32 bit value to be big endian
 *
 */
static uint32_t endian_big_uint32(SOS_ENDIANNESS endi, uint32_t input)
{
	uint32_t temp = 0x0;

	/* If we are running on a little endian system, convert to big */
	if (endi == SOS_ENDIANNESS_LITTLE) {
		temp |= ((input >> 24) & 0x000000ff);
		temp |= ((input >> 8) & 0x0000ff00);
		temp |= ((input << 8) & 0x00ff0000);
		temp |= ((input << 24) & 0xff000000);
	} else {
		temp = input;
	}
	return (temp);
}

/*
 * Standard CRC-32 Polynomial of: x^32 + x^26 + x^23 + x^22 + x^16 +
 * x^12 + x^11 + x^10 + x^8 + x^7 + x^5 + x^4 + x^2 + x^1+1
 */
static const uint32_t crc32_lut[] = {
	0x0, 0x04c11db7, 0x09823b6e, 0x0d4326d9, 0x130476dc,
	0x17c56b6b, 0x1a864db2, 0x1e475005, 0x2608edb8, 0x22c9f00f, 0x2f8ad6d6,
	0x2b4bcb61, 0x350c9b64, 0x31cd86d3, 0x3c8ea00a, 0x384fbdbd, 0x4c11db70,
	0x48d0c6c7, 0x4593e01e, 0x4152fda9, 0x5f15adac, 0x5bd4b01b, 0x569796c2,
	0x52568b75, 0x6a1936c8, 0x6ed82b7f, 0x639b0da6, 0x675a1011, 0x791d4014,
	0x7ddc5da3, 0x709f7b7a, 0x745e66cd, 0x9823b6e0, 0x9ce2ab57, 0x91a18d8e,
	0x95609039, 0x8b27c03c, 0x8fe6dd8b, 0x82a5fb52, 0x8664e6e5, 0xbe2b5b58,
	0xbaea46ef, 0xb7a96036, 0xb3687d81, 0xad2f2d84, 0xa9ee3033, 0xa4ad16ea,
	0xa06c0b5d, 0xd4326d90, 0xd0f37027, 0xddb056fe, 0xd9714b49, 0xc7361b4c,
	0xc3f706fb, 0xceb42022, 0xca753d95, 0xf23a8028, 0xf6fb9d9f, 0xfbb8bb46,
	0xff79a6f1, 0xe13ef6f4, 0xe5ffeb43, 0xe8bccd9a, 0xec7dd02d, 0x34867077,
	0x30476dc0, 0x3d044b19, 0x39c556ae, 0x278206ab, 0x23431b1c, 0x2e003dc5,
	0x2ac12072, 0x128e9dcf, 0x164f8078, 0x1b0ca6a1, 0x1fcdbb16, 0x018aeb13,
	0x054bf6a4, 0x0808d07d, 0x0cc9cdca, 0x7897ab07, 0x7c56b6b0, 0x71159069,
	0x75d48dde, 0x6b93dddb, 0x6f52c06c, 0x6211e6b5, 0x66d0fb02, 0x5e9f46bf,
	0x5a5e5b08, 0x571d7dd1, 0x53dc6066, 0x4d9b3063, 0x495a2dd4, 0x44190b0d,
	0x40d816ba, 0xaca5c697, 0xa864db20, 0xa527fdf9, 0xa1e6e04e, 0xbfa1b04b,
	0xbb60adfc, 0xb6238b25, 0xb2e29692, 0x8aad2b2f, 0x8e6c3698, 0x832f1041,
	0x87ee0df6, 0x99a95df3, 0x9d684044, 0x902b669d, 0x94ea7b2a, 0xe0b41de7,
	0xe4750050, 0xe9362689, 0xedf73b3e, 0xf3b06b3b, 0xf771768c, 0xfa325055,
	0xfef34de2, 0xc6bcf05f, 0xc27dede8, 0xcf3ecb31, 0xcbffd686, 0xd5b88683,
	0xd1799b34, 0xdc3abded, 0xd8fba05a, 0x690ce0ee, 0x6dcdfd59, 0x608edb80,
	0x644fc637, 0x7a089632, 0x7ec98b85, 0x738aad5c, 0x774bb0eb, 0x4f040d56,
	0x4bc510e1, 0x46863638, 0x42472b8f, 0x5c007b8a, 0x58c1663d, 0x558240e4,
	0x51435d53, 0x251d3b9e, 0x21dc2629, 0x2c9f00f0, 0x285e1d47, 0x36194d42,
	0x32d850f5, 0x3f9b762c, 0x3b5a6b9b, 0x0315d626, 0x07d4cb91, 0x0a97ed48,
	0x0e56f0ff, 0x1011a0fa, 0x14d0bd4d, 0x19939b94, 0x1d528623, 0xf12f560e,
	0xf5ee4bb9, 0xf8ad6d60, 0xfc6c70d7, 0xe22b20d2, 0xe6ea3d65, 0xeba91bbc,
	0xef68060b, 0xd727bbb6, 0xd3e6a601, 0xdea580d8, 0xda649d6f, 0xc423cd6a,
	0xc0e2d0dd, 0xcda1f604, 0xc960ebb3, 0xbd3e8d7e, 0xb9ff90c9, 0xb4bcb610,
	0xb07daba7, 0xae3afba2, 0xaafbe615, 0xa7b8c0cc, 0xa379dd7b, 0x9b3660c6,
	0x9ff77d71, 0x92b45ba8, 0x9675461f, 0x8832161a, 0x8cf30bad, 0x81b02d74,
	0x857130c3, 0x5d8a9099, 0x594b8d2e, 0x5408abf7, 0x50c9b640, 0x4e8ee645,
	0x4a4ffbf2, 0x470cdd2b, 0x43cdc09c, 0x7b827d21, 0x7f436096, 0x7200464f,
	0x76c15bf8, 0x68860bfd, 0x6c47164a, 0x61043093, 0x65c52d24, 0x119b4be9,
	0x155a565e, 0x18197087, 0x1cd86d30, 0x029f3d35, 0x065e2082, 0xb1d065b,
	0x0fdc1bec, 0x3793a651, 0x3352bbe6, 0x3e119d3f, 0x3ad08088, 0x2497d08d,
	0x2056cd3a, 0x2d15ebe3, 0x29d4f654, 0xc5a92679, 0xc1683bce, 0xcc2b1d17,
	0xc8ea00a0, 0xd6ad50a5, 0xd26c4d12, 0xdf2f6bcb, 0xdbee767c, 0xe3a1cbc1,
	0xe760d676, 0xea23f0af, 0xeee2ed18, 0xf0a5bd1d, 0xf464a0aa, 0xf9278673,
	0xfde69bc4, 0x89b8fd09, 0x8d79e0be, 0x803ac667, 0x84fbdbd0, 0x9abc8bd5,
	0x9e7d9662, 0x933eb0bb, 0x97ffad0c, 0xafb010b1, 0xab710d06, 0xa6322bdf,
	0xa2f33668, 0xbcb4666d, 0xb8757bda, 0xb5365d03, 0xb1f740b4 };

static unsigned long memcrc(const unsigned char *b, size_t n)
{
	/*  Input arguments:
	 *  const char*	  b == byte sequence to checksum
	 *  size_t	  n == length of sequence
	 */
	register unsigned	i, c, s = 0;

	for (i = n; i > 0; --i) {
		c = (unsigned)(*b++);
		s = (s << 8) ^ crc32_lut[(s >> 24) ^ c];
	}
	/* Extend with the length of the string. */
	while (n != 0) {
		c = n & 0xff;
		n >>= 8;
		s = (s << 8) ^ crc32_lut[(s >> 24) ^ c];
	}
	return ~s;
}

// Search for this in column 6 in order to add crc32
static char crc_token[] = {"crc32"};


/**
 *
 * Converts CSV file to binary fiule
 *
 * Returns TRUE if no errors
 *
 */
static BOOL convert_csv(void)
{
	FILE *ip, *op;
	char line[MAX_LINE];
	char token[MAX_LINE];
	unsigned line_nr = 0;
	int offset;
	unsigned int crc32_seek = 0;
	unsigned int value;
	char desc[MAX_LINE];
	uint32_t crc32 = 0;
	uint8_t data;
	int i, j;
	BOOL parse_error;
	int last_byte = 0;
	int nrw = 0;
	SOS_ENDIANNESS endianness;
	uint8_t *buffer = NULL;

	ip = fopen(input_fn, "r");
	if (ip == NULL ) {
		printf("Cannot open input file '%s'\n", input_fn);
		return (FALSE);
	}

	op = fopen(output_fn, "w");
	if (op == NULL ) {
		printf("Cannot open output file '%s'\n", output_fn);
		fclose(ip);
		return (FALSE);
	}

	while (fgets(line, MAX_LINE, ip) != NULL ) {
		int field_num = 0;
		int num_fields;
		int line_len;
		parse_error = FALSE;

		++line_nr;
		num_fields = 0;
		j = 0;
		token[j] = '\0';
		field_num = 0;
		line[strlen(line) - 2] = '\0'; /* remove newline character */

		line_len = (int)strlen(line);
		/* only use lines that start with "0x" */
		if (line[0] != '0' || tolower(line[1]) != 'x') {
			if (verbose_flag)
				DEBUG_PRINTF("\nSkipping Line (#%d) "
					     "len: %d <%s>",
					     line_nr, line_len, line);
			continue;
		}

		if (verbose_flag)
			DEBUG_PRINTF("\nLine (#%d): <%s>\n", line_nr, line);

		for (i = 0; i <= line_len; i++) {
			switch (line[i]) {
			case ',':
			case '\0':
				if (strlen(token) != 0) {
					switch (field_num) {
					case 0: /* Offset */
						if (verbose_flag)
							DEBUG_PRINTF("Token "
								     "(Offset): <%s>\n",
								     token);
						if (token[0] != '0' ||
						    tolower(token[1]) != 'x' ||
						    sscanf(&token[2], "%x",
							   &offset) != 1)
							{
								parse_error = TRUE;
								printf("ERROR while "
								       "reading Offset-Token "
								       "on line %d! "
								       "skipping line\n",
								       line_nr);
								continue;
							}
						num_fields++;
						break;
					case 1: /* Desc */
						strcpy(desc, token);
						num_fields++;
						break;
					case 6: /* CRC */
						if (0 == strncmp(crc_token,
								 token, 5)) {
							if (0 == crc32_seek) {
								if (verbose_flag)
									DEBUG_PRINTF("Token (crc32): "
										     "at offset %d <%s>\n",
										     offset, token);
								crc32_seek = offset; /* Save */
							}
						}
						break;
					case 9: /* Value */
						if (verbose_flag)
							DEBUG_PRINTF("Token "
								     "(Value): <%s>\n",
								     token);
						if (token[0] != '0' ||
						    tolower(token[1]) != 'x' ||
						    sscanf(&token[2], "%x",
							      &value) != 1) {
							parse_error = TRUE;
							printf("ERROR while reading "
							       "Value-Token on line %d! "
							       "skipping line\n",
							       line_nr);
							continue;
						}
						num_fields++;
						break;
					default:
						if (verbose_flag)
							DEBUG_PRINTF("Token %d: "
								     "<%s>\n",
								     field_num, token);
						break;
					}

				}
				j = 0;
				token[j] = '\0';
				field_num++;
				break;
			default:
				token[j++] = line[i];
				token[j] = '\0';
				break;
			}
		}

		if (!parse_error && ((num_fields == 3) || (num_fields == 2))) {
			if (verbose_flag)
				DEBUG_PRINTF("Offset: <0x%04X>, Desc: <%s>, "
					     "Value: <0x%02X>\n",
					     offset, desc, value);
			fseek(op, offset, SEEK_SET);
			data = (uint8_t)value;;
			nrw = fwrite(&data, 1, 1, op);
			if (1 != nrw)
				printf("Error: fwrite %d != 1\n", nrw);
			if (offset > last_byte)
				last_byte = offset + 1;
		}
	}
	if (verbose_flag)
		DEBUG_PRINTF("Close In <%s> Out <%s> Size %d\n", input_fn,
			     output_fn, last_byte);
	fclose(op);
	fclose(ip);
	// Add some code to add crc32 to output file
	if ((TRUE == add_crc) && (0 != crc32_seek)) {
		op = fopen(output_fn, "r+");
		if (op == NULL ) {
			printf("\nCannot open '%s'", output_fn);
			return (FALSE);
		}
		buffer = malloc(last_byte);
		if (NULL == buffer) {
			printf("\nCannot allocate %d Bytes\n", last_byte);
			fclose(op);
			return (FALSE);
		}
		nrw = fread(buffer, 1, last_byte, op);
		if (nrw == last_byte) {
			crc32 = memcrc(buffer, last_byte);
			DEBUG_PRINTF("%ld %d %s\n", (unsigned long)crc32,
				     last_byte, output_fn);
			/* Work out endianness */
			endianness = sos0_endianness();
			crc32 = endian_big_uint32(endianness, crc32);
			// Seek to crc32 offset
			fseek(op, crc32_seek, SEEK_SET);
			nrw = fwrite(&crc32, 1, 4, op);
			if (4 != nrw)
				printf("Error: fwrite %d of 4 Bytes\n", nrw);
		} else {
			printf("Error: fread %d of %d Bytes\n",
			       nrw, last_byte);
		}
		free(buffer);
		fclose(op);
		if (verbose_flag)
			DEBUG_PRINTF("CRC32 Added to <%s>\n", output_fn);
	}
	return (TRUE);
}

static void set_args(int argc, char *argv[])
{
	int i;

	arg_count = argc;
	for (i = 0; i < argc; i++)
		arg_values[i] = argv[i];
	arg_index = 1;
}

static char *next_arg(void)
{
	if (arg_index < arg_count)
		return (arg_values[arg_index]);
	return (NULL);
}

static char *get_next_arg(void)
{
	if (arg_index < arg_count)
		return (arg_values[arg_index++]);
	return (NULL);
}

static void help(void)
{
	printf("csv2bin -i <Input CSV File> -o <Output Bin File>\n"
	       "\t-crc Add crc32 to bin file (same as from chksum).\n"
	       "\t-v Verbose mode.\n");
	return;
}

/**
 * Get command line parameters and create the output file.
 */
int main(int argc, char *argv[])
{
	BOOL input_fn_set = FALSE;
	BOOL output_fn_set = FALSE;

	set_args(argc, argv);
	/* Process command line args */
	while (arg_index < argc) {
		if (strcmp(next_arg(), "-h") == 0) {
			help();
			exit(0);
		}
		if (strcmp(next_arg(), "-o") == 0) {
			get_next_arg();
			if (sscanf(next_arg(), "%s",
				   (char *) &(output_fn)) == 1)
				output_fn_set = TRUE;
		}
		else if (strcmp(next_arg(), "-i") == 0) {
			get_next_arg();
			if (sscanf(next_arg(), "%s",
				   (char *) &(input_fn)) == 1)
				input_fn_set = TRUE;
		}
		else if (strcmp(next_arg(), "-v") == 0)
			verbose_flag = TRUE;
		else if (strcmp(next_arg(), "-crc") == 0)
			add_crc = TRUE;
		get_next_arg();
	}

	if (verbose_flag) {
		printf("\nInput Filename:   '%s'", input_fn);
		printf("\nOutput Filename:  '%s'", output_fn);
		printf("\n");
	}

	if ((TRUE == input_fn_set) && (TRUE == output_fn_set))
		convert_csv();
	else
		help();

	if (verbose_flag)
		printf("\n");
	exit(0);
}
