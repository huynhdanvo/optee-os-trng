/******************************************************************************
* Copyright (c) 2010 - 2021 Xilinx, Inc.  All rights reserved.
* Copyright (c) 2022 - 2025 Advanced Micro Devices, Inc. All Rights Reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/

/*****************************************************************************/
/**
*
* @file xil_types.h
*
* @addtogroup common_types Basic Data types for Xilinx&reg; Software IP
*
* The xil_types.h file contains basic types for Xilinx software IP. These data types
* are applicable for all processors supported by Xilinx.
* @{
* <pre>
* MODIFICATION HISTORY:
*
* Ver   Who    Date   Changes
* ----- ---- -------- -------------------------------------------------------
* 1.00a hbm  07/14/09 First release
* 3.03a sdm  05/30/11 Added Xuint64 typedef and XUINT64_MSW/XUINT64_LSW macros
* 5.00 	pkp  05/29/14 Made changes for 64 bit architecture
*	srt  07/14/14 Use standard definitions from stdint.h and stddef.h
*		      Define LONG and ULONG datatypes and mask values
* 7.00  mus  01/07/19 Add cpp extern macro
* 7.1   aru  08/19/19 Shift the value in UPPER_32_BITS only if it
*                     is 64-bit processor
* 8.1   dp   12/23/22 Updated UINTPTR and INTPTR to point to 64bit data types
*                     incase of microblaze 32-bit with extended address enabled
* 9.0   ml   04/14/23 Add parenthesis on sub-expression to fix misra-c violation.
* 9.2   ml   09/26/24 Removed checks to fix below compilation warning
*                     XPAR_MICROBLAZE_ADDR_SIZE is not defined, evaluates to 0
* 9.3   ml   12/20/24 Fixed GCC warnings
* </pre>
*
******************************************************************************/

/**
 *@cond nocomments
 */
#ifndef XIL_TYPES_H	/* prevent circular inclusions */
#define XIL_TYPES_H	/* by using protection macros */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/************************** Constant Definitions *****************************/

#ifndef TRUE
#  define TRUE		1U
#endif

#ifndef FALSE
#  define FALSE		0U
#endif

#ifndef NULL
#define NULL		0U
#endif

#define XIL_COMPONENT_IS_READY     0x11111111U  /**< In device drivers, This macro will be
                                                 assigend to "IsReady" member of driver
												 instance to indicate that driver
												 instance is initialized and ready to use. */
#define XIL_COMPONENT_IS_STARTED   0x22222222U  /**< In device drivers, This macro will be assigend to
                                                 "IsStarted" member of driver instance
												 to indicate that driver instance is
												 started and it can be enabled. */

/* @name New types
 * New simple types.
 * @{
 */


typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;



typedef char char8;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint64_t u64;
typedef int sint32;

#ifndef __KERNEL__
#ifndef XBASIC_TYPES_H
/*
 * guarded against xbasic_types.h.
 */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
/** @}*/
#define __XUINT64__
typedef struct
{
	u32 Upper;
	u32 Lower;
} Xuint64;


/*****************************************************************************/
/**
* @brief    Return the most significant half of the 64 bit data type.
*
* @param    x is the 64 bit word.
*
* @return   The upper 32 bits of the 64 bit word.
*
******************************************************************************/
#define XUINT64_MSW(x) ((x).Upper)

/*****************************************************************************/
/**
* @brief    Return the least significant half of the 64 bit data type.
*
* @param    x is the 64 bit word.
*
* @return   The lower 32 bits of the 64 bit word.
*
******************************************************************************/
#define XUINT64_LSW(x) ((x).Lower)

#endif /* XBASIC_TYPES_H */

/*
 * xbasic_types.h does not typedef s* or u64
 */
/** @{ */



#endif

#if defined(__MICROBLAZE__) && !defined(__arch64__) && \
    defined(XPAR_MICROBLAZE_ADDR_SIZE) && (XPAR_MICROBLAZE_ADDR_SIZE > 32)
typedef uint64_t UINTPTR;
typedef int64_t INTPTR;
#else
typedef uintptr_t UINTPTR;
typedef intptr_t INTPTR;
#endif

typedef ptrdiff_t PTRDIFF;
/** @}*/
#if !defined(LONG) || !defined(ULONG)
typedef long LONG;
typedef unsigned long ULONG;
#endif

#define ULONG64_HI_MASK	0xFFFFFFFF00000000U
#define ULONG64_LO_MASK	~ULONG64_HI_MASK

/************************** Constant Definitions *****************************/

#ifndef TRUE
#define TRUE		1U
#endif

#ifndef FALSE
#define FALSE		0U
#endif

#ifndef NULL
#define NULL		0U
#endif

#ifdef __cplusplus
}
#endif

#endif	/* end of protection macro */
/**
 *@endcond
 */
/**
* @} End of "addtogroup common_types".
*/
