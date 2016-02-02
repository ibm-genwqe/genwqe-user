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

#pragma once
__BEGIN_DECLS

#define CGZIP_CR_DEVICE		0x00000602 /* 0x0000044c */
#define CGZIP_CR_VENDOR		0x00001014
#define CGZIP_CR_CLASS		0x00120000

#define MMIO_IMP_VERSION_REG    0x00000000ull
/*
	-Bits-
	63..48 RO: Reserved
	47..32 RO: AFU logic frequency, divided by 10000.
		Examples:
		0x61A8 (25000): 250.00MHz
		0x4E20 (20000): 200.00MHz
		0x4650 (18000): 180.00MHz
		0x411B (16667): 166.67MHz
		Note: The PSL interface and job control interfaces are
		always clocked with 250MHz.
		31..24 RO: Build Year (decade, BCD coded)
		0x15: 2015
	23..16 RO: Build Month (BCD coded)
		Example: 0x10: October
	15..8  RO: Build Day (BCD coded)
		0x24: 24th
	 7..4  RO: Reserved
	 3..0  RO: Build Count (binary, count from 0)
		0xE: 15th build on that day

	POR value depends on build date.
	Example for 180MHz, 7th build on October 31st,
	2015: 0x00004650_15103106
 */

#define MMIO_APP_VERSION_REG    0x00000008ull
/*
	-Bits-
	63..40 RO: Release ID (optional): Release Identifier or mkrel
	        release name or otherwise designator that uniquely
		identifies how to retrieve the VHDL source code that
		produced this AFU. Higher numbers are later versions.
	39..32 RO: Application Layer Architecture
		0x02: GZIP DDCB with dynamic Huffman support
		0x03: GZIP DDCB with dynamic Huffman support and MMIO
		      driven queue
	31..0  RO: Application Identifier
		0x475A4950: GZIP

	POR value: 0x00000003_475A4950
*/

/* X * 1/200Mhz (set to 512 = 2048 nsec) */
#define MMIO_AFU_CONFIG_REG     0x00000010ull
/*
	-Bits-
	63..0  RW: Minimum available time slice per context

	POR value: 0x00000000_00000200 corresponds to 2048ns
*/

#define MMIO_AFU_STATUS_REG     0x00000018ull
/*
	-Bits-
	63..4  RC: TBD
	    3  RO: DEBUG REGISTER was written (to be removed!)
	    2  RO: A Huffman encoding register was written
	    1  RO: An Aggravator Register was written
	    0  RO: An Error Injection register was written
*/

#define MMIO_AFU_COMMAND_REG    0x00000020ull
/*
	-Bits-
	63..4  RO: Reserved
	3...0  RW: Command
		Legal commands are:
		0x4 Abort: Abort current DDCB and set accelerator to finished
			immediately (asserting aXh_jdone)
		0x2 Stop:  Finish current DDCB, then set accelerator to
			finished
			(asserting aXh_jdone)
		0x0 NOP
*/

/* Context is active if bit is set */
#define MMIO_CASV_REG		0x00000040ull
/*
	Address: 0x000040 + m * 0x000008 (m = 0,...,7)
	63..0  RO: Context m*8+k is attached if (and only if) bit k is set.
*/
#define MMIO_CASV_REG_NUM	8	/* ATTACH Status REG: 40 ... 78 */

#define MMIO_FRT_REG		0x00000080ull /* Free run timer X * 1/200Mhz */
/*
	63..0  RO: Counter counting the number of clock cycles since reset
		   (afu open)
	This counter increments with the 250MHz PSL clock.
*/

#define MMIO_DDCB_TIMEOUT_REG	0x00000088ull	/* X * 1/180Mhz (set to */
/*
	-Bits-
	63     RW: Enable DDCB Timeout checking
	62..32 RO: Reserved
	31..0  RW: DDCB Timeout value (this value decrements with 180MHz clock)

	POR value: 0x80000000_0ABA9500 timeout enabled to 1s
*/

#define MMIO_FIR_REGS_BASE      0x00001000ull    /* FIR: 1000...1028 */
/*
	Address: 0x0001000
	63..32 RO: Reserved
	31..0  RC: FIR bits TBD

	MMIO FIRs
	=========
	Address: 0x0001008
	63..32 RO: Reserved
	31..1  RC: Reserved (FIR bits TBD)
	    0  RC: Parity errror

	DMA FIRs
	========
	Address: 0x0001010
	63..32 RO: Reserved
	31..0  RC: Reserved (FIR bits TBD)

	DDCB-Manager FIRs
	=================
	Address: 0x0001018
	63..32 RO: Reserved
	31..0  RC: Reserved (FIR bits TBD)

	Compression FIRs
	=================
	Address: 0x0001020
	63..32 RO: Reserved
	31..0  RC: Reserved (FIR bits TBD)

	Decompression FIRs
	=================
	Address: 0x0001028
	63..32 RO: Reserved
	31..0  RC: Reserved (FIR bits TBD)
 */
#define MMIO_FIR_REGS_NUM       6

#define MMIO_ERRINJ_MMIO_REG    0x00001800ull
/*
	Error Injection MMIO
	====================
	Address: 0x0001800
	63..1  RO: Reserved
	    0  RS: Inject Parity error
*/

#define MMIO_ERRINJ_GZIP_REG    0x00001808ull
/*
	Error Injection GZIP
	====================
	Address: 0x0001808
	63..0  RS: Reserved (bits TBD)
*/

#define MMIO_AGRV_REGS_BASE     0x00002000ull
/*
	Aggravator Register
	===================
	Note: The value that is written into this register will be
              rotated left every cycle.
	Throttling is active in cycles where bit 63 equals '1'.
	Address: 0x0002000 63..0  RW: GZIP DATA READ  Throttle Register
	Address: 0x0002008 63..0  RW: GZIP DATA WRITE Throttle Register
	Address: 0x0002010 63..0  RW: DMA  DATA READ  Throttle Register
	Address: 0x0002018 63..0  RW: DMA  DATA WRITE Throttle Register
	Address: 0x0002020 63..0  RW: DMA  FSM  READ  Throttle Register
	Address: 0x0002028 63..0  RW: DMA  FSM  WRITE Throttle Register
	Address: 0x0002030 63..0  RW: DMA  FSM  CMD   Throttle Register
*/
#define MMIO_AGRV_REGS_NUM      7

#define MMIO_GZIP_REGS_BASE     0x2100ull
/*
	GZIP Huffman Literal/Length Code Register
	=========================================
	Address: 0x0002100
	63..56 RW: RAM Address
	28..24 RW: Literal/Length Code Width
	19..0  RW: Literal/Length Code

	GZIP Huffman Distance Code Register
	===================================
	Address: 0x0002108
	63..56 RW: RAM Address
	35..32 RW: Distance Extra Bit Width
	27..24 RW: Distance Code Width
	19..5  RW: Distance Code

	GZIP Huffman Decider Literal/Length Width Register
	==================================================
	Address: 0x0002110
	63..56 RW: RAM Address
	39..35 RW: Literal/Length Code Width Tree 0
	34..30 RW: Literal/Length Code Width Tree 1
	29..25 RW: Literal/Length Code Width Tree 2
	24..20 RW: Literal/Length Code Width Tree 3
	19..15 RW: Literal/Length Code Width Tree 4
	14..10 RW: Literal/Length Code Width Tree 5
	9..5  RW: Literal/Length Code Width Tree 6
	4..0  RW: Literal/Length Code Width Tree 7

	GZIP Huffman Decider Distance Width Register
	============================================
	Address: 0x0002118
	63..56 RW: RAM Address
	39..35 RW: Distance Code Width Tree 0
	34..30 RW: Distance Code Width Tree 1
	29..25 RW: Distance Code Width Tree 2
	24..20 RW: Distance Code Width Tree 3
	19..15 RW: Distance Code Width Tree 4
	14..10 RW: Distance Code Width Tree 5
	9..5  RW: Distance Code Width Tree 6
	4..0  RW: Distance Code Width Tree 7

	GZIP Huffman Tree RAM Register
	==============================
	Address: 0x0002120
	63..56 RW: RAM Address
		RAM address bits 59:56 is used to address the position of the
		40 bits Tree RAM Data inside the 160 bits.
		The list below shows the data position for each address value.
		--00b =  39:0
		--01b =  79:40
		--10b = 119:80
		--11b = 159:120
	39..0  RW: Tree RAM Data

	GZIP Huffman Decider Control Register
	=====================================
	Address: 0x0002178
	63..56 RW: RAM Address
	24     RW: Enable Predefine Values
		* 1b = bit 20 and bits 18:16 are valid
	20     RW: Use Predefine Huffman Tree
		* 1b = The GZIP Deflate Core Decider Logic is using the
			predefined Huffman tree (18:16).
			This tree will be used until the next write or the
			next power on.
		* 0b = The GZIP Deflate Core Decider Logic is no longer using
		        the predefined Huffman tree
	18..16 RW: Predefine Huffman Tree
	12     RW: Enable Decider Window
		* 1b = overrides the GZIP Deflate Core Decider Logic
		       Decider Window value.
	10..0  RW: Maximum Decider Window
		* The Value after power on = 512 (6K).
		* The value of this field multiplied by 12 is the size of the
		  Decider Window.
		* After a write, the Windows remains until the next power on.
		* A larger value could reduce the bandwidth of the GZIP
		  Deflate Core.
		* In case of bad compressible input data, the Decider Window
		  could be smaller as this value. This happens due to GZIP
		  Deflate Core internal buffer sizes.
		In such a case the Decider Window is not exactly predictable.
 */
#define MMIO_GZIP_REGS_NUM      16

#define MMIO_DEBUG_REG		0x0000FF00ull
/*
	DEBUG REGISTER (to be removed!)
	===============================
	Address: 0x000FF00 RW
	0x000FF08 RC
	0x000FF10 RS
	63..4  Reserved
	    3  Enable Parity checking
	    2..0  PSL Translation Ordering behavior
 */


#define	MMIO_CTX_OFFSET		0x00010000ull	/* Offset for each Context */
#define	MMIO_MASTER_CTX_NUMBER	0
#define	MMIO_SLAVE_CTX_NUM	512

/*****************************************
**      Slave PSA for Context n        **
*****************************************/
#define MMIO_DDCBQ_START_REG    0x00000100ull
/*
	DDCB Queue Start Pointer Register (QSPR)
	========================================
	Address: 0x0000100 + (n+1) * 0x0010000
	63..0  Pointer to start of DDCB queue in system memory
	63..8  RW
	7..0  RO: Always 0

	POR value: 0x00000000_00000000
	Value after afu_attach: WED pointer
 */

#define MMIO_DDCBQ_CONFIG_REG   0x00000108ull
/*
	DDCB Queue Configuration Register (QCfgR)
	=======================================
	** This register must not be written while the DDCB queue is active **
	** A valid write operation into this register also resets the
	   corresponding
	** DDCB Queue Work Timer **
	Address: 0x0000108 + (n+1) * 0x0010000
	63..48 RW: First expected DDCB sequence number
	47..32 RO: Reserved
	31..24 RW: First DDCB index to execute. Must be <= Max DDCB index
	23..16 RW: Max DDCB index
	15..0  RO: Reserved

	POR value: 0x00000000_00000000
 */

#define MMIO_DDCBQ_COMMAND_REG  0x00000110ull
/*
	DDCB Queue Command Register (QCmdR)
	===================================
	Address: 0x0000110 + (n+1) * 0x0010000
	63..48 RW: Argument
	47..4  RO: Reserved
	3..0  RW: Command
	Legal commands are:
	0x4 Abort: Stop all DDCB activities for this queue immediately
	           (Argument: Don't care)
	0x2 Stop:  Finish current DDCB, then stop queue (Argument: Don't care)
	0x1 Start: Execute DDCBs (Argument: <Last sequence number to be
	           executed> must be set)
	0x0 NOP

	POR value: 0x00000000_00000000
 */

#define MMIO_DDCBQ_STATUS_REG   0x00000118ull
/*
	DDCB Queue Status Register (QSR)
	================================
	Address: 0x0000118 + (n+1) * 0x0010000
	63..48 RO: Current DDCB sequence number
	47..32 RO: Last DDCB sequence number to be executed
	31..24 RO: Current DDCB index.
	23..8  Non-fatal errors:
	23..22 RO: Reserved
	21 RC: DMA Data Error (see DMA Error Address Register
	       for DMA address triggering the error)
	20 RC: DMA Address Error (see DMA Error Address Register
	       for DMA address triggering the error)
	19 RC: Received command while previous command still active
	       (i.e. DDCB Queue Status Register[3:0] <> 0x0)
	18 RC: Received illegal command in DDCB Queue Command
	       Register
	17 RC: Invalid Sequence number in DDCB (queue will be
	       stopped)
	16 RC: Write attempt to DDCB Queue Start Pointer register
	       while Queue active
	15 RC: Write attempt to DDCB Queue Configuration register
	       while Queue active
	14 RC: Write attempt to DDCB Queue Configuration register
	         with first DDCB index > max DDCB index
	13 RC: MMIO Write access to Read-Only register
	12 RC: Illegal MMIO write address
	11 RC: Illegal MMIO write alignment
	10 RC: Illegal MMIO read address
	9  RC: Illegal MMIO read alignment
	8  RC: Bad WED pointer
	7..6  RO: Reserved
	5  RO: Currently executing DDCB
	4  RO: Queue Active
	       1=fetching and executing DDCBs until last DDCB sequence
		 number is reached
	       0=stopped
	3..0 RO: Command that is currently being executed
	         (see DDCB Queue Command Register)

	Value 0x0 (NOP) means: Currently, no command is active
 */

#define MMIO_DDCBQ_CID_REG      0x00000120ull    /* Context ID REG */
/*
	Slave Context Register (SCR)
	============================
	Address: 0x0000120 + (n+1) * 0x0010000
	63..32 RO: Reserved
	31..26 RO: "000000" for Slave
	25..16 RO: Current context id (10 bits corresponding to 512 contexts)
	15..10 RO: "000000" for Slave access
	 9..0  RO: My context id (10 bits corresponding to 512 contexts)
 */

#define MMIO_DDCBQ_DMAE_REG	0x00000128ull
/*
	DDCB Queue DMA Error Address Register (QDEAR)
	=============================================
	Address: 0x0000128 + (n+1) * 0x0010000
	63..0  RO: DMA address that caused the error
 */

#define MMIO_DDCBQ_WT_REG       0x00000180ull
/*
	DDCB Queue Work Timer (QWT)
	===========================
	Address: 0x0000180 + (n+1) * 0x0010000
	63..0  RO: Counter counting the number of clock cycles during
	           DDCB execution for this context
		   (Counter gets reset with every valid DDCBQ CONFIG
		   Register write access; the value is persistent during reset)
		   This counter increments with the 250MHz PSL clock.
 */

__END_DECLS
