// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2022 Xilinx, Inc.  All rights reserved.
 * Copyright (C) 2022 Foundries Ltd.
 *
 * Driver port from Xilinx's FSBL implementation, jorge@foundries.io
 *
 * The Xilinx True Random Number Generator(TRNG) module in Versal - PMC TRNG
 * consists of an entropy source, a deterministic random bit generator (DRBG)
 * and health test logic, which tests the randomness of the generated data.
 * The entropy source for the unit is an array of Ring Oscillators.
 *
 * The Versal PMC TRNG is envisaged to operate in three basic modes: DRNG, PTRNG
 * and HRNG . Each of these can be operated with or without Derivative Function
 * (DF), resulting in a total of 6 different modes of operation.
 *
 * NIST SP-800-90A practically requires the true random generators based on
 * CTR_DRBG to include a derivation function (DF). This is expected to be
 * implemented inside the Silicon (TRNG IP). However, the version of the IP used
 * in Versal PMC does not have this capability. Hence, a software
 * implementation of the DF is done in this driver.
 *
 * DRNG mode: Deterministic Random Number Generator mode.
 *            In this mode, the DRBG portion of the TRNG is used. User provides
 *            the (external) seed.
 * PTRNG mode: Physical True Random Number Generator mode (aka Entropy mode).
 *            In this mode digitized Entropy source is output as random number.
 * HRNG mode: Hybrid Random Number Generator mode.
 *            This is combination of above two modes in which the Entropy source
 *            is used to provide the seed, which is fed to the DRBG, which in
 *            turn generates the random number.
 *
 * DRNG mode with DF: It may not be common usecase to use the DF with DRNG as
 * the general expectation would be that the seed would have sufficient entropy.
 * However, the below guideline from section 10.2.1 of NIST SP-800-90A implies
 * that need for DF for DRNG mode too: "..the DRBG mechanism is specified to
 * allow an implementation tradeoff with respect to the use of this derivation
 * function. The use of the derivation function is optional if either an
 * approved RBG or an entropy source provides full entropy output when entropy
 * input is requested by the DRBG mechanism. Otherwise, the derivation function
 * shall be used". Sufficient large entropy data from user is fed to DF to
 * generate the seed which will be loaded into the external seed registers.
 * From here, it is similar to regular DRNG mode.
 *
 * PTRNG mode with DF: This mode is similar to PTRNG mode, however, the entropy
 * data from the core output registers are accumulated and fed to the DF
 * (instead of directly consuming it). The output of the DF would be final
 * random data. In this mode, the output of DF is not seed but the random data.
 *
 * HRNG mode with DF: This mode is the combination of the above two modes.
 * The entropy data is fed to the DF to produce seed. This seed is loaded to the
 * external seed registers which provide seed to the DRBG.
 */
#include <arm.h>
#include <crypto/crypto.h>
#include <initcall.h>
#include <io.h>
#include <kernel/delay.h>
#include <kernel/panic.h>
#include <mm/core_mmu.h>
#include <mm/core_memprot.h>
#include <platform_config.h>
#include <rng_support.h>
#include <stdlib.h>
#include <string.h>
#include <tee/tee_cryp_utl.h>
#include <trace.h>
#include <drivers/versal_trng.h>
#include <drivers/versal_mbox.h>
#include <drivers/xtrngpsx.h>

#define TRNG_STATUS			0x04
#define TRNG_STATUS_QCNT_SHIFT		9
#define TRNG_STATUS_QCNT_MASK		(BIT(9) | BIT(10) | BIT(11))
#define TRNG_STATUS_CERTF_MASK		BIT(3)
#define TRNG_STATUS_DTF_MASK		BIT(1)
#define TRNG_STATUS_DONE_MASK		BIT(0)
#define TRNG_CTRL			0x08
#define TRNG_CTRL_PERSODISABLE		BIT(10)
#define TRNG_CTRL_SINGLEGENMODE		BIT(9)
#define TRNG_CTRL_EUMODE_MASK		BIT(8)
#define TRNG_CTRL_PRNGMODE_MASK		BIT(7)
#define TRNG_CTRL_TSTMODE_MASK		BIT(6)
#define TRNG_CTRL_PRNGSTART_MASK	BIT(5)
#define TRNG_CTRL_PRNGXS_MASK		BIT(3)
#define TRNG_CTRL_TRSSEN_MASK		BIT(2)
#define TRNG_CTRL_PRNGSRST_MASK		BIT(0)

#if defined(CFG_VERSAL_RNG_DRV_V2)
#define TRNG_CTRL_2			0x0C
#define TRNG_CTRL_2_RCTCUTOFF_SHIFT	8
#define TRNG_CTRL_2_RCTCUTOFF_MASK	GENMASK_32(16, 8)
#define TRNG_CTRL_2_RCTCUTOFF_DEFVAL	0x21
#define TRNG_CTRL_2_DIT_SHIFT		0
#define TRNG_CTRL_2_DIT_MASK		GENMASK_32(4, 0)
#define TRNG_CTRL_2_DIT_DEFVAL		0xC
#define TRNG_CTRL_3			0x10
#define TRNG_CTRL_3_APTCUTOFF_SHIFT	8
#define TRNG_CTRL_3_APTCUTOFF_MASK	GENMASK_32(17, 8)
#define TRNG_CTRL_3_APTCUTOFF_DEFVAL	0x264
#define TRNG_CTRL_3_DLEN_SHIFT		0
#define TRNG_CTRL_3_DLEN_MASK		GENMASK_32(7, 0)
#define TRNG_CTRL_3_DLEN_DEFVAL		0x9
#define TRNG_CTRL_4			0x14
#define TRNGPSX_DF_NUM_OF_BYTES_BEFORE_MIN_700CLKS_WAIT	8U /**< Number of bytes to be written before wait */
#define TRNGPSX_PERS_STRING_LEN_IN_WORDS		12U	/**< Personalization string length in words */
#define TRNGPSX_PERS_STRING_LEN_IN_BYTES		48U	/**< Personalization string length in bytes */
#define TRNGPSX_WORD_LEN_IN_BYTES				4U	/**< Word length in bytes */
#define TRNGPSX_BYTE_LEN_IN_BITS				8U	/**< Byte length in bits */
#define TRNG_PER_STRNG_11    					(0x000000ACU)
#define TRNGPSX_DF_2CLKS_WAIT					2U	/** < delay after 1byte */
#define TRNGPSX_BLOCK_LEN_IN_BYTES				16U	/**< TRNG block length length in bytes */
#define TRNGPSX_DF_700CLKS_WAIT					10U	/** < delay after 4bytes */
#define TRNG_CTRL_PERSODISABLE_MASK   	 		0x00000400U
#define TRNG_CTRL_PERSODISABLE_DEFVAL  			0x0U
#endif

#define TRNG_EXT_SEED_0			0x40
/*
 * Below registers are not directly referenced in driver but are accessed
 * with offset from TRNG_EXT_SEED_0
 * Register: TRNG_EXT_SEED_1		0x00000044
 * Register: TRNG_EXT_SEED_2		0x00000048
 * Register: TRNG_EXT_SEED_3		0x0000004C
 * Register: TRNG_EXT_SEED_4		0x00000050
 * Register: TRNG_EXT_SEED_5		0x00000054
 * Register: TRNG_EXT_SEED_6		0x00000058
 * Register: TRNG_EXT_SEED_7		0x0000005C
 * Register: TRNG_EXT_SEED_8		0x00000060
 * Register: TRNG_EXT_SEED_9		0x00000064
 * Register: TRNG_EXT_SEED_10		0x00000068
 * Register: TRNG_EXT_SEED_11		0x0000006C
 */
#define TRNG_PER_STRING_0		0x80
/*
 * Below registers are not directly referenced in driver but are accessed
 * with offset from TRNG_PER_STRING_0
 * Register: TRNG_PER_STRING_1		0x00000084
 * Register: TRNG_PER_STRING_2		0x00000088
 * Register: TRNG_PER_STRING_3		0x0000008C
 * Register: TRNG_PER_STRING_4		0x00000090
 * Register: TRNG_PER_STRING_5		0x00000094
 * Register: TRNG_PER_STRING_6		0x00000098
 * Register: TRNG_PER_STRING_7		0x0000009C
 * Register: TRNG_PER_STRING_8		0x000000A0
 * Register: TRNG_PER_STRING_9		0x000000A4
 * Register: TRNG_PER_STRING_10		0x000000A8
 * Register: TRNG_PER_STRING_11		0x000000AC
 */
#define TRNG_CORE_OUTPUT		0xC0
#define TRNG_RESET			0xD0
#define TRNG_RESET_VAL_MASK		BIT(0)
#define TRNG_OSC_EN			0xD4
#define TRNG_OSC_EN_VAL_MASK		BIT(0)

/* TRNG configuration  */
#define TRNG_BURST_SIZE		16
#define TRNG_BURST_SIZE_BITS	128
#define TRNG_NUM_INIT_REGS	12
#define TRNG_REG_SIZE		32
#define TRNG_BYTES_PER_REG	4
#define TRNG_MAX_QCNT		4
#define TRNG_GENERATE_TIMEOUT	8000
#define TRNG_RESEED_TIMEOUT		1500000U /**< Reseed timeout in micro-seconds */
#define TRNG_MIN_DFLENMULT	2
#define TRNG_MAX_DFLENMULT	9
#define PRNGMODE_RESEED		0
#define PRNGMODE_GEN		TRNG_CTRL_PRNGMODE_MASK
#define RESET_DELAY		10
#define TRNG_SEC_STRENGTH_LEN	32
#define TRNG_SEED_REGS		12
#define TRNG_GEN_LEN		32
#define BYTES_PER_BLOCK		16
#define ALL_A_PATTERN_32	0xAAAAAAAA
#define ALL_5_PATTERN_32	0x55555555

/* Derivative function variables */
static unsigned char sbx1[256];
static unsigned char sbx2[256];
static unsigned char sbx3[256];
static unsigned char schedule[BLK_SIZE * (MAX_ROUNDS + 1)];
static unsigned int rounds;

static void rota4(uint8_t *a, uint8_t *b, uint8_t *c, uint8_t *d)
{
	uint8_t t = *a;

	*a = sbx1[*b];
	*b = sbx1[*c];
	*c = sbx1[*d];
	*d = sbx1[t];
}

static void rota2(uint8_t *a, uint8_t *b)
{
	uint8_t t = *a;

	*a = sbx1[*b];
	*b = sbx1[t];
}

static void sbox4(uint8_t *a, uint8_t *b, uint8_t *c, uint8_t *d)
{
	*a = sbx1[*a];
	*b = sbx1[*b];
	*c = sbx1[*c];
	*d = sbx1[*d];
}

static void xorb(uint8_t *res,  const uint8_t *in)
{
	size_t i = 0;

	for (i = 0; i < BLK_SIZE; ++i)
		res[i] ^= in[i];
}

static void set_key(uint8_t *res, const uint8_t *src, unsigned int roundval)
{
	memcpy(res, src, BLK_SIZE);
	xorb(res, schedule + roundval * BLK_SIZE);
}

static void mix_column_sbox(uint8_t *dst, const uint8_t *f)
{
	size_t i = 0;
	size_t a = 0;
	size_t b = 0;
	size_t c = 0;
	size_t d = 0;

	for (i = 0; i < 4; i++) {
		a = 4 * i;
		b = (0x5 + a) % 16;
		c = (0xa + a) % 16;
		d = (0xf + a) % 16;
		dst[0 + a] = sbx2[f[a]] ^ sbx3[f[b]] ^ sbx1[f[c]] ^ sbx1[f[d]];
		dst[1 + a] = sbx1[f[a]] ^ sbx2[f[b]] ^ sbx3[f[c]] ^ sbx1[f[d]];
		dst[2 + a] = sbx1[f[a]] ^ sbx1[f[b]] ^ sbx2[f[c]] ^ sbx3[f[d]];
		dst[3 + a] = sbx3[f[a]] ^ sbx1[f[b]] ^ sbx1[f[c]] ^ sbx2[f[d]];
	}
}

static void shift_row_sbox(uint8_t *f)
{
	sbox4(&f[0], &f[4], &f[8], &f[12]);
	rota4(&f[1], &f[5], &f[9], &f[13]);
	rota2(&f[2], &f[10]);
	rota2(&f[6], &f[14]);
	rota4(&f[15], &f[11], &f[7], &f[3]);
}

static void encrypt(uint8_t *in, uint8_t *out)
{
	uint8_t fa[BLK_SIZE] = { 0 };
	uint8_t fb[BLK_SIZE] = { 0 };
	size_t roundval = 0;

	set_key(fa, in, 0);
	for (roundval = 1; roundval < rounds; ++roundval) {
		mix_column_sbox(fb, fa);
		set_key(fa, fb, roundval);
	}

	shift_row_sbox(fa);
	set_key(out, fa, roundval);
}

static void checksum(unsigned char *in, uint8_t *iv, int max_blk)
{
	while (max_blk > 0) {
		xorb(iv, in);
		encrypt(iv, iv);
		in += BLK_SIZE;
		max_blk -= 1;
	}
}

static void setup_key(const unsigned char *k, size_t klen)
{
	unsigned char rcon = 1;
	size_t sch_size = 240;
	size_t i = 0;

	rounds = MAX_ROUNDS;
	memcpy(schedule, k, klen);
	for (i = klen; i < sch_size; i += 4) {
		unsigned char t0 = 0;
		unsigned char t1 = 0;
		unsigned char t2 = 0;
		unsigned char t3 = 0;
		int ik = 0;

		t0 = schedule[i - 4];
		t1 = schedule[i - 3];
		t2 = schedule[i - 2];
		t3 = schedule[i - 1];
		if (i % klen == 0) {
			rota4(&t0, &t1, &t2, &t3);
			t0 ^= rcon;
			rcon = (rcon << 1) ^ (((rcon >> 7) & 1) * 0x1B);
		} else if (i % klen == 16) {
			sbox4(&t0, &t1, &t2, &t3);
		}
		ik = i - klen;
		schedule[i + 0] = schedule[ik + 0] ^ t0;
		schedule[i + 1] = schedule[ik + 1] ^ t1;
		schedule[i + 2] = schedule[ik + 2] ^ t2;
		schedule[i + 3] = schedule[ik + 3] ^ t3;
	}
}

static void trng_df_init(void)
{
	const uint8_t sb[] = {
		0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01,
		0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d,
		0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4,
		0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
		0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7,
		0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
		0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e,
		0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
		0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb,
		0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb,
		0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c,
		0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
		0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c,
		0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d,
		0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a,
		0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
		0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3,
		0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
		0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
		0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
		0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e,
		0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9,
		0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9,
		0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
		0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99,
		0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
	};
	size_t i = 0;

	memcpy(sbx1, sb, sizeof(sb));
	for (i = 0; i < sizeof(sb); i++) {
		sbx2[i] = (sb[i] << 1) ^ (((sb[i] >> 7) & 1) * 0x1B);
		sbx3[i] = sbx2[i] ^ sb[i];
	}
}

/*
 * This function implements the Derivative Function by distilling the entropy
 * available in its input into a smaller number of bits on the output.
 * - per NIST SP80090A.
 *
 * The Block Cipher algorithm follows sections 10.3.2 and 10.3.3 of the
 * NIST.SP.800-90Ar1 document
 */
static void trng_df_algorithm(struct versal_trng *trng, uint8_t *dfout,
			      uint32_t flag, const uint8_t *pstr)
{
	static bool df_init;
	const uint8_t df_key[DF_KEY_LEN] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
		17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
	};
	size_t dfin_len = sizeof(struct trng_dfin) + trng->len;
	uint8_t *inp_blk = NULL;
	uint8_t *out_blk = NULL;
	uintptr_t reminder = 0;
	size_t xfer_len = 0;
	uint32_t index = 0;
	uintptr_t src = 0;
	uintptr_t dst = 0;
	size_t offset = 0;

	if (!df_init) {
		trng_df_init();
		df_init = true;
	}

	if (flag == DF_SEED)
		trng->dfin.val2 = TEE_U32_TO_BIG_ENDIAN(TRNG_PERS_STR_LEN);
	else
		trng->dfin.val2 = TEE_U32_TO_BIG_ENDIAN(TRNG_GEN_LEN);

	trng->dfin.pad_data[0] = DF_PAD_VAL;

	if (!pstr) {
		if (trng->len > (MAX_PRE_DF_LEN + TRNG_PERS_STR_LEN))
			panic();

		dfin_len = dfin_len - TRNG_PERS_STR_LEN - MAX_PRE_DF_LEN;
		trng->dfin.val1 = TEE_U32_TO_BIG_ENDIAN(trng->len);

		xfer_len = DF_PAD_DATA_LEN;
		src = (uintptr_t)trng->dfin.pad_data;
		offset = MAX_PRE_DF_LEN + TRNG_PERS_STR_LEN - trng->len;
	} else {
		if (trng->len > MAX_PRE_DF_LEN)
			panic();

		memcpy(trng->dfin.pstr, pstr, TRNG_PERS_STR_LEN);
		dfin_len = dfin_len - MAX_PRE_DF_LEN;
		trng->dfin.val1 = TEE_U32_TO_BIG_ENDIAN(trng->len +
							TRNG_PERS_STR_LEN);
		xfer_len = DF_PAD_DATA_LEN + TRNG_PERS_STR_LEN;
		src = (uintptr_t)trng->dfin.pstr;
		offset = MAX_PRE_DF_LEN - trng->len;
	}

	/* Move back into the dfin structure */
	dst = src - offset;
	reminder = (uintptr_t)&trng->dfin + sizeof(trng->dfin) - offset;
	if (offset) {
		if (xfer_len > offset)
			panic("Overlapping data");

		memcpy((void *)dst, (void *)src, xfer_len);
		memset((void *)reminder, 0, offset);
	}

	/* DF algorithm - step 1 */
	setup_key(df_key, DF_KEY_LEN);
	for (index = 0; index < TRNG_SEED_LEN; index += BLK_SIZE) {
		memset((void *)(trng->dfout + index), 0, BLK_SIZE);
		trng->dfin.ivc[0] = TEE_U32_TO_BIG_ENDIAN(index / BLK_SIZE);
		checksum((unsigned char *)&trng->dfin,
			 trng->dfout + index, dfin_len / BLK_SIZE);
	}

	/* DF algorithm - step 2 */
	setup_key(trng->dfout, DF_KEY_LEN);
	for (index = 0; index < TRNG_SEED_LEN; index += BLK_SIZE) {
		if (!index)
			inp_blk = &dfout[TRNG_SEC_STRENGTH_LEN];
		else
			inp_blk = &dfout[index - BLK_SIZE];

		out_blk = &dfout[index];
		encrypt(inp_blk, out_blk);
	}
}

static uint32_t trng_read32(vaddr_t addr, size_t off)
{
	return io_read32(addr + off);
}

static void trng_write32(vaddr_t addr, size_t off, uint32_t val)
{
	io_write32(addr + off, val);
}

#if defined(CFG_VERSAL_RNG_DRV_V2)
static int trng_write32_v2(vaddr_t addr, uint32_t mask, uint32_t value)
{
	int status = 1;
	uint32_t regval;
	u32 val;

	val = io_read32(addr);
	val = (val & (~mask)) | (mask & value);
	io_write32(addr, val);

	/* verify value written to specified address */
	regval = io_read32(addr) & mask;

	if (regval == (mask & value)) {
		status = 0;
	}

	return status;
}

static int trng_write_perstr(const struct versal_trng *trng, const uint8_t *perstr)
{
	int status = 1;
	volatile u8 idx = 0;
	uint8_t cnt = 0;
	uint32_t regval =0;

	for(idx = 0; idx < TRNGPSX_PERS_STRING_LEN_IN_WORDS; idx++)
	{
		regval = 0;
		for (cnt = 0; cnt < TRNGPSX_WORD_LEN_IN_BYTES; cnt++) 
		{
			regval = (regval << TRNGPSX_BYTE_LEN_IN_BITS) | perstr[(idx * TRNGPSX_WORD_LEN_IN_BYTES) + cnt];
		}

		trng_write32(trng->cfg.addr, (TRNG_PER_STRNG_11 - (idx * TRNGPSX_WORD_LEN_IN_BYTES)), regval);
	}

	if (idx == TRNGPSX_PERS_STRING_LEN_IN_WORDS) {
		status = 0;
	}

	return status;
}

static int trng_write_seed(const struct versal_trng *trng, const uint8_t *seed, uint8_t dlen) {
	volatile int status = 1;
	uint32_t seed_len = (dlen + 1U) * TRNGPSX_BLOCK_LEN_IN_BYTES;
	volatile u32 idx = 0U;
	uint8_t cnt = 0U;
	uint32_t bit = 0U;
	uint8_t seed_construct = 0U;

	while (idx < seed_len) {
		seed_construct = 0U;
		for (cnt = 0; cnt < TRNGPSX_BYTE_LEN_IN_BITS; cnt++) {
			bit = (uint32_t)(seed[idx] >> (TRNGPSX_BYTE_LEN_IN_BITS - 1U - cnt)) & 0x01U;
			trng_write32(trng->cfg.addr, TRNG_CTRL_4, bit);
			seed_construct = (uint8_t)((seed_construct << 1U) | (uint8_t)bit);
		}
		if (seed_construct != seed[idx]) {
			goto END;
		}
		udelay(TRNGPSX_DF_2CLKS_WAIT);
		if ((idx % TRNGPSX_DF_NUM_OF_BYTES_BEFORE_MIN_700CLKS_WAIT) == 0U) {
			udelay(TRNGPSX_DF_700CLKS_WAIT);
		}
		idx++;
	}
	if (idx == seed_len) {
		status = 0;
	}

END:
	return status;
}
#endif

static void trng_clrset32(vaddr_t addr, size_t off, uint32_t mask, uint32_t val)
{
	io_clrsetbits32(addr + off, mask, mask & val);
}

static void trng_write32_range(const struct versal_trng *trng, uint32_t start,
			       uint32_t num_regs, const uint8_t *buf)
{
	size_t off = 0;
	uint32_t val = 0;
	size_t cnt = 0;
	size_t i = 0;

	for (i = 0; i < num_regs; ++i) {
		if (!buf) {
			off = start + i * TRNG_BYTES_PER_REG;
			trng_write32(trng->cfg.addr, off, 0);
			continue;
		}

		val = 0;
		for (cnt = 0; cnt < TRNG_BYTES_PER_REG; ++cnt)
			val = (val << 8) | buf[i * TRNG_BYTES_PER_REG + cnt];

		off = start + (TRNG_NUM_INIT_REGS - 1 - i) * TRNG_BYTES_PER_REG;
		trng_write32(trng->cfg.addr, off, val);
	}
}

static TEE_Result trng_wait_for_event(vaddr_t addr, size_t off, uint32_t mask,
				      uint32_t event, uint32_t time_out)
{
	uint64_t tref = timeout_init_us(time_out);

	do {
		if (timeout_elapsed(tref))
			break;
	} while ((io_read32(addr + off) & mask) != event);

	/* Normal world might have suspended the OP-TEE thread, check again  */
	if ((io_read32(addr + off) & mask) != event)
		return TEE_ERROR_GENERIC;

	return TEE_SUCCESS;
}

static void trng_soft_reset(const struct versal_trng *trng)
{
	trng_clrset32(trng->cfg.addr, TRNG_CTRL, TRNG_CTRL_PRNGSRST_MASK,
		      TRNG_CTRL_PRNGSRST_MASK);
	udelay(RESET_DELAY);
	trng_clrset32(trng->cfg.addr, TRNG_CTRL, TRNG_CTRL_PRNGSRST_MASK, 0);
}

static void trng_reset(const struct versal_trng *trng)
{
	trng_write32(trng->cfg.addr, TRNG_RESET, TRNG_RESET_VAL_MASK);
	udelay(RESET_DELAY);
	trng_write32(trng->cfg.addr, TRNG_RESET, 0);
	trng_soft_reset(trng);
}

static void trng_hold_reset(const struct versal_trng *trng)
{
	trng_clrset32(trng->cfg.addr, TRNG_CTRL,
		      TRNG_CTRL_PRNGSRST_MASK, TRNG_CTRL_PRNGSRST_MASK);
	trng_write32(trng->cfg.addr, TRNG_RESET, TRNG_RESET_VAL_MASK);
	udelay(RESET_DELAY);
}

static TEE_Result trng_check_seed(uint8_t *entropy, uint32_t len)
{
	uint32_t *p = (void *)entropy;
	size_t i = 0;

	for (i = 0; i < len / sizeof(*p); i++) {
		if (p[i] == ALL_A_PATTERN_32)
			return TEE_ERROR_GENERIC;

		if (p[i] == ALL_5_PATTERN_32)
			return TEE_ERROR_GENERIC;
	}

	return TEE_SUCCESS;
}

static TEE_Result trng_collect_random(struct versal_trng *trng, uint8_t *dst,
				      size_t len)
{
	const size_t bursts = len / TRNG_BURST_SIZE;
	const size_t words = TRNG_BURST_SIZE_BITS / TRNG_REG_SIZE;
	uint32_t *p = (void *)dst;
	size_t bcnt = 0;
	size_t wcnt = 0;
	size_t index = 0;
	uint32_t val = 0;
	bool match = false;

	trng_clrset32(trng->cfg.addr, TRNG_CTRL,
		      TRNG_CTRL_PRNGSTART_MASK, TRNG_CTRL_PRNGSTART_MASK);

	/*
	 * Loop as many times based on len requested. In each burst 128 bits
	 * are generated, which is reflected in QCNT value of 4 by hardware.
	 */
	for (bcnt = 0; bcnt < bursts; bcnt++) {
		if (trng_wait_for_event(trng->cfg.addr,
					TRNG_STATUS, TRNG_STATUS_QCNT_MASK,
					TRNG_MAX_QCNT << TRNG_STATUS_QCNT_SHIFT,
					TRNG_GENERATE_TIMEOUT)) {
			EMSG("Timeout waiting for randomness");
			return TEE_ERROR_GENERIC;
		}

		/*
		 * DTF flag set during generate indicates catastrophic
		 * condition, which needs to be checked for every time unless we
		 * are in PTRNG mode
		 */
		if (trng->usr_cfg.mode != TRNG_PTRNG) {
			val = trng_read32(trng->cfg.addr, TRNG_STATUS);
			if (val & TRNG_STATUS_DTF_MASK) {
				EMSG("Catastrophic DFT error");
				trng->status = TRNG_CATASTROPHIC;

				return TEE_ERROR_GENERIC;
			}
		}
		/*
		 * Read the core output register 4 times to consume the random
		 * data generated for every burst.
		 */
		match = true;
		for (wcnt = 0; wcnt < words; wcnt++) {
			val = trng_read32(trng->cfg.addr, TRNG_CORE_OUTPUT);

			if (bcnt > 0 && trng->buf[wcnt] != val)
				match = false;

			trng->buf[wcnt] = val;

			if (dst) {
				p[index] = TEE_U32_TO_BIG_ENDIAN(val);
				index++;
			}
		}

		if (bursts > 1 && bcnt > 0 && match) {
			EMSG("Catastrophic software error");
			trng->status = TRNG_CATASTROPHIC;
			return TEE_ERROR_GENERIC;
		}
	}

	return TEE_SUCCESS;
}

static TEE_Result trng_reseed_internal_nodf(struct versal_trng *trng,
					    uint8_t *eseed, uint8_t *str, uint32_t mul)
{
#if defined(CFG_VERSAL_RNG_DRV_V2)
	/* Configure DF Len */
	uint32_t persmask = TRNG_CTRL_PERSODISABLE_MASK;
	if (trng->cfg.version == TRNG_V2)
	{
		trng_write32_v2(trng->cfg.addr + TRNG_CTRL_3, TRNG_CTRL_3_DLEN_MASK, (mul << TRNG_CTRL_3_DLEN_SHIFT));
	}

	if (str != NULL)
	{
		trng_write_perstr(trng, str);
		persmask = TRNG_CTRL_PERSODISABLE_DEFVAL;
	}

	trng_write32_v2(trng->cfg.addr + TRNG_CTRL, TRNG_CTRL_PERSODISABLE_MASK | TRNG_CTRL_PRNGSTART_MASK, persmask);
	/* DRNG Mode */
	if (eseed != NULL) {
		/* Enable TST mode and set PRNG mode for reseed operation*/
		trng_write32_v2(trng->cfg.addr + TRNG_CTRL, TRNG_CTRL_PRNGMODE_MASK | TRNG_CTRL_TSTMODE_MASK | TRNG_CTRL_TRSSEN_MASK, TRNG_CTRL_TSTMODE_MASK | TRNG_CTRL_TRSSEN_MASK);

		/* Start reseed operation */
		trng_write32_v2(trng->cfg.addr + TRNG_CTRL, TRNG_CTRL_PRNGSTART_MASK, TRNG_CTRL_PRNGSTART_MASK);
		
		/* For writing seed as an input to DF, PRNG start needs to be set */
		trng_write_seed(trng, eseed, mul);
	} 
	else { /* HTRNG Mode */
		/* Enable ring oscillators for random seed source */
		trng_write32_v2(trng->cfg.addr + TRNG_OSC_EN, TRNG_OSC_EN_VAL_MASK, TRNG_OSC_EN_VAL_MASK);

		/* Enable TRSSEN and set PRNG mode for reseed operation */
		trng_write32_v2(trng->cfg.addr + TRNG_CTRL, TRNG_CTRL_PRNGMODE_MASK | TRNG_CTRL_TRSSEN_MASK | TRNG_CTRL_PRNGXS_MASK, TRNG_CTRL_TRSSEN_MASK);
	
		/* Start reseed operation */
		trng_write32_v2(trng->cfg.addr + TRNG_CTRL, TRNG_CTRL_PRNGSTART_MASK, TRNG_CTRL_PRNGSTART_MASK);
	}
	trng->stats.elapsed_seed_life = 0;
#else
	uint8_t entropy[TRNG_SEED_LEN] = { 0 };
	uint8_t *seed = NULL;
	switch (trng->usr_cfg.mode) {
	case TRNG_HRNG:
		trng_write32(trng->cfg.addr, TRNG_OSC_EN, TRNG_OSC_EN_VAL_MASK);
		trng_soft_reset(trng);
		trng_write32(trng->cfg.addr, TRNG_CTRL,
			     TRNG_CTRL_EUMODE_MASK | TRNG_CTRL_TRSSEN_MASK);

		if (trng_collect_random(trng, entropy, TRNG_SEED_LEN))
			return TEE_ERROR_GENERIC;

		if (trng_check_seed(entropy, TRNG_SEED_LEN))
			return TEE_ERROR_GENERIC;

		seed = entropy;
		break;
	case TRNG_DRNG:
		seed = eseed;
		break;
	default:
		seed = NULL;
		break;
	}

	trng_write32_range(trng, TRNG_EXT_SEED_0, TRNG_SEED_REGS, seed);
	if (str)
		trng_write32_range(trng, TRNG_PER_STRING_0, TRNG_PERS_STR_REGS,
				   str);
#endif
	return TEE_SUCCESS;
}

static TEE_Result trng_reseed_internal_df(struct versal_trng *trng,
					  uint8_t *eseed, uint8_t *str)
{
	memset(&trng->dfin, 0, sizeof(trng->dfin));

	switch (trng->usr_cfg.mode) {
	case TRNG_HRNG:
		trng_write32(trng->cfg.addr, TRNG_OSC_EN, TRNG_OSC_EN_VAL_MASK);
		trng_soft_reset(trng);
		trng_write32(trng->cfg.addr, TRNG_CTRL,
			     TRNG_CTRL_EUMODE_MASK | TRNG_CTRL_TRSSEN_MASK);

		if (trng_collect_random(trng, trng->dfin.entropy, trng->len))
			return TEE_ERROR_GENERIC;

		if (trng_check_seed(trng->dfin.entropy, trng->len))
			return TEE_ERROR_GENERIC;
		break;
	case TRNG_DRNG:
		memcpy(trng->dfin.entropy, eseed, trng->len);
		break;
	default:
		break;
	}

	trng_df_algorithm(trng, trng->dfout, DF_SEED, str);
	trng_write32_range(trng, TRNG_EXT_SEED_0, TRNG_SEED_REGS, trng->dfout);

	return TEE_SUCCESS;
}

static TEE_Result trng_reseed_internal(struct versal_trng *trng,
				       uint8_t *eseed, uint8_t *str,
				       uint32_t mul)
{
	uint32_t val = 0;

	trng->stats.bytes_reseed = 0;
	trng->stats.elapsed_seed_life = 0;

	if (trng->usr_cfg.df_disable)
		trng->len = TRNG_SEED_LEN;
	else
		trng->len = (mul + 1) * BYTES_PER_BLOCK;

	if (trng->usr_cfg.df_disable || trng->cfg.version == TRNG_V2) {
		if (trng_reseed_internal_nodf(trng, eseed, str, mul))
			goto error;
	} else {
		if (trng_reseed_internal_df(trng, eseed, str))
			goto error;
	}
#ifndef CFG_VERSAL_RNG_DRV_V2
	trng_write32(trng->cfg.addr, TRNG_CTRL,
		     PRNGMODE_RESEED | TRNG_CTRL_PRNGXS_MASK);

	/* Start the reseed operation */
	trng_clrset32(trng->cfg.addr, TRNG_CTRL, TRNG_CTRL_PRNGSTART_MASK,
		      TRNG_CTRL_PRNGSTART_MASK);
#endif
	/* Wait for reseed operation */
	if (trng_wait_for_event(trng->cfg.addr, TRNG_STATUS,
				TRNG_STATUS_DONE_MASK, TRNG_STATUS_DONE_MASK,
				TRNG_RESEED_TIMEOUT))
		goto error;

	/* Check SP800 - 90B (entropy health test error) */
	val = trng_read32(trng->cfg.addr, TRNG_STATUS) & TRNG_STATUS_CERTF_MASK;
	if (val == TRNG_STATUS_CERTF_MASK)
		goto error;

	trng_clrset32(trng->cfg.addr, TRNG_CTRL, TRNG_CTRL_PRNGSTART_MASK, 0);
	return TEE_SUCCESS;
error:
	trng->status = TRNG_ERROR;
	return TEE_ERROR_GENERIC;
}

static TEE_Result trng_instantiate(struct versal_trng *trng,
				   const struct trng_usr_cfg *usr_cfg)
{
	uint8_t *seed = NULL;
	uint8_t *pers = NULL;

	if (!trng)
		return TEE_ERROR_GENERIC;

	if (!usr_cfg)
		goto error;

	if (trng->status != TRNG_UNINITIALIZED)
		goto error;

	if (usr_cfg->mode != TRNG_HRNG && usr_cfg->mode != TRNG_DRNG &&
	    usr_cfg->mode != TRNG_PTRNG)
		goto error;

	if (usr_cfg->mode != TRNG_PTRNG && !usr_cfg->seed_life)
		goto error;

	if (!usr_cfg->iseed_en && usr_cfg->mode == TRNG_DRNG)
		goto error;

	if (usr_cfg->iseed_en && usr_cfg->mode == TRNG_HRNG)
		goto error;

	if (!usr_cfg->df_disable &&
	    (usr_cfg->dfmul < TRNG_MIN_DFLENMULT ||
	     usr_cfg->dfmul > TRNG_MAX_DFLENMULT))
		goto error;

	if (usr_cfg->df_disable && usr_cfg->dfmul)
		goto error;

	if (usr_cfg->mode == TRNG_PTRNG &&
	    (usr_cfg->iseed_en || usr_cfg->pstr_en ||
	     usr_cfg->predict_en || usr_cfg->seed_life))
		goto error;

	memcpy(&trng->usr_cfg, usr_cfg, sizeof(struct trng_usr_cfg));
	/* Bring TRNG and PRNG unit core out of reset */
	trng_reset(trng);

	if (trng->usr_cfg.iseed_en)
		seed = (void *)trng->usr_cfg.init_seed;

	if (trng->usr_cfg.pstr_en)
		pers = (void *)trng->usr_cfg.pstr;

	if (trng->cfg.version == TRNG_V2 &&
	    (usr_cfg->mode == TRNG_PTRNG || usr_cfg->mode == TRNG_HRNG)) {
		/* Configure cutoff test values */
		trng_clrset32(trng->cfg.addr, TRNG_CTRL_3,
			      TRNG_CTRL_3_APTCUTOFF_MASK,
			      TRNG_CTRL_3_APTCUTOFF_DEFVAL
			      << TRNG_CTRL_3_APTCUTOFF_SHIFT);
		trng_clrset32(trng->cfg.addr, TRNG_CTRL_2,
			      TRNG_CTRL_2_RCTCUTOFF_MASK,
			      TRNG_CTRL_2_RCTCUTOFF_DEFVAL
			      << TRNG_CTRL_2_RCTCUTOFF_SHIFT);
		/* Configure default DIT value */
		trng_clrset32(trng->cfg.addr, TRNG_CTRL_2,
			      TRNG_CTRL_2_DIT_MASK,
			      TRNG_CTRL_2_DIT_DEFVAL << TRNG_CTRL_2_DIT_SHIFT);
	}

	/* Do reseed operation when mode is DRNG/HRNG */
	if (trng->usr_cfg.mode != TRNG_PTRNG) {
		if (trng_reseed_internal(trng, seed, pers, trng->usr_cfg.dfmul))
			goto error;
	}

	trng->status = TRNG_HEALTHY;
	return TEE_SUCCESS;
error:
	trng->status = TRNG_ERROR;
	return TEE_ERROR_GENERIC;
}

static TEE_Result trng_reseed(struct versal_trng *trng, uint8_t *eseed,
			      uint32_t mul)
{
	if (!trng)
		return TEE_ERROR_GENERIC;

	if (trng->status != TRNG_HEALTHY)
		goto error;

	if (trng->usr_cfg.mode != TRNG_DRNG && trng->usr_cfg.mode != TRNG_HRNG)
		goto error;

	if (trng->usr_cfg.mode == TRNG_DRNG && !eseed)
		goto error;

	if (trng->usr_cfg.mode != TRNG_DRNG && eseed)
		goto error;

	if (!trng->usr_cfg.df_disable) {
		if (mul < TRNG_MIN_DFLENMULT || mul > TRNG_MAX_DFLENMULT)
			goto error;
	}

	if (trng->usr_cfg.df_disable && mul)
		goto error;

	if (eseed && !memcmp(eseed, trng->usr_cfg.init_seed, trng->len))
		goto error;


#if defined(CFG_VERSAL_RNG_DRV_V2)
	/* Wait for reseed operation */
	trng_wait_for_event(trng->cfg.addr, TRNG_STATUS, TRNG_STATUS_DONE_MASK, TRNG_STATUS_DONE_MASK, TRNG_RESEED_TIMEOUT);
#endif
	if (trng_reseed_internal(trng, eseed, NULL, mul))
		goto error;
	return TEE_SUCCESS;
error:
	trng->status = TRNG_ERROR;
	return TEE_ERROR_GENERIC;
}

static TEE_Result trng_generate(struct versal_trng *trng, uint8_t *buf,
				size_t blen, bool predict)
{
	uint32_t len = TRNG_SEC_STRENGTH_LEN;
	uint8_t *p = buf;

	if (!trng)
		return TEE_ERROR_GENERIC;

	if (!p)
		goto error;

	if (blen < TRNG_SEC_STRENGTH_LEN)
		goto error;

	if (trng->status != TRNG_HEALTHY)
		goto error;

	if (trng->usr_cfg.mode == TRNG_PTRNG && predict)
		goto error;

	if (!trng->usr_cfg.predict_en && predict)
		goto error;

	switch (trng->usr_cfg.mode) {
	case TRNG_HRNG:
		if (trng->stats.elapsed_seed_life >= trng->usr_cfg.seed_life) {
			if (trng_reseed_internal(trng, NULL, NULL, 0))
				goto error;
		}

		if (predict && trng->stats.elapsed_seed_life > 0) {
			if (trng_reseed_internal(trng, NULL, NULL, 0))
				goto error;
		}

		trng_write32(trng->cfg.addr, TRNG_CTRL, PRNGMODE_GEN);
		break;
	case TRNG_DRNG:
		if (trng->stats.elapsed_seed_life > trng->usr_cfg.seed_life)
			goto error;

		if (predict && trng->stats.elapsed_seed_life > 0)
			goto error;

		trng_write32(trng->cfg.addr, TRNG_CTRL, PRNGMODE_GEN);
		break;
	default:
		if (!trng->usr_cfg.df_disable) {
			memset(&trng->dfin, 0, sizeof(trng->dfin));
			len = (trng->usr_cfg.dfmul + 1) * BYTES_PER_BLOCK;
			trng->len = len;
			p = trng->dfin.entropy;
		}
		/* Enable the 8 ring oscillators used for entropy source */
		trng_write32(trng->cfg.addr, TRNG_OSC_EN, TRNG_OSC_EN_VAL_MASK);
		trng_soft_reset(trng);
		trng_write32(trng->cfg.addr, TRNG_CTRL,
			     TRNG_CTRL_EUMODE_MASK | TRNG_CTRL_TRSSEN_MASK);
		break;
	}

	if (trng_collect_random(trng, p, len))
		goto error;

	trng->stats.bytes_reseed += len;
	trng->stats.bytes += len;
	trng->stats.elapsed_seed_life++;

	if (!trng->usr_cfg.df_disable && trng->usr_cfg.mode == TRNG_PTRNG)
		trng_df_algorithm(trng, buf, DF_RAND, NULL);

	return TEE_SUCCESS;
error:
	if (trng->status != TRNG_CATASTROPHIC)
		trng->status = TRNG_ERROR;

	return TEE_ERROR_GENERIC;
}

static TEE_Result trng_release(struct versal_trng *trng)
{
	if (!trng)
		return TEE_ERROR_GENERIC;

	if (trng->status == TRNG_UNINITIALIZED)
		goto error;

	trng_write32_range(trng, TRNG_EXT_SEED_0, TRNG_SEED_REGS, NULL);
	trng_write32_range(trng, TRNG_PER_STRING_0, TRNG_PERS_STR_REGS, NULL);
	trng_hold_reset(trng);

	/* Clear the instance */
	memset(&trng->usr_cfg, 0, sizeof(trng->usr_cfg));
	memset(trng->buf, 0, sizeof(trng->buf));
	memset(trng->dfout, 0, sizeof(trng->dfout));
	trng->status = TRNG_UNINITIALIZED;

	return TEE_SUCCESS;
error:
	trng->status = TRNG_ERROR;

	return TEE_ERROR_GENERIC;
}

/* Health tests should be run when the configured mode is of PTRNG or HRNG */
static TEE_Result trng_health_test(struct versal_trng *trng)
{
	struct trng_usr_cfg tests = {
		.mode = TRNG_HRNG,
		.seed_life = 10,
		.dfmul = 7,
		.predict_en = false,
		.iseed_en = false,
		.pstr_en = false,
		.df_disable = false,
	};

	if (trng_instantiate(trng, &tests))
		goto error;

	if (trng_release(trng))
		goto error;

	return TEE_SUCCESS;
error:
	trng->status = TRNG_ERROR;

	return TEE_ERROR_GENERIC;
}

/*
 * The KAT test should be run when the TRNG is configured in DRNG or HRNG mode.
 * If KAT fails, the driver has to be put in error state.
 */
static TEE_Result trng_kat_test(struct versal_trng *trng)
{
	struct trng_usr_cfg tests = {
		.mode = TRNG_DRNG,
		.seed_life = 5,
		.dfmul = 2,
		.predict_en = false,
		.iseed_en = true,
		.pstr_en = true,
		.df_disable = false,
	};
	const uint8_t ext_seed[TRNG_SEED_LEN] = {
		0x3BU, 0xC3U, 0xEDU, 0x64U, 0xF4U, 0x80U, 0x1CU, 0xC7U,
		0x14U, 0xCCU, 0x35U, 0xEDU, 0x57U, 0x01U, 0x2AU, 0xE4U,
		0xBCU, 0xEFU, 0xDEU, 0xF6U, 0x7CU, 0x46U, 0xA6U, 0x34U,
		0xC6U, 0x79U, 0xE8U, 0x91U, 0x5DU, 0xB1U, 0xDBU, 0xA7U,
		0x49U, 0xA5U, 0xBBU, 0x4FU, 0xEDU, 0x30U, 0xB3U, 0x7BU,
		0xA9U, 0x8BU, 0xF5U, 0x56U, 0x4DU, 0x40U, 0x18U, 0x9FU,
	};
	const uint8_t pers_str[TRNG_PERS_STR_LEN] = {
		0xB2U, 0x80U, 0x7EU, 0x4CU, 0xD0U, 0xE4U, 0xE2U, 0xA9U,
		0x2FU, 0x1FU, 0x5DU, 0xC1U, 0xA2U, 0x1FU, 0x40U, 0xFCU,
		0x1FU, 0x24U, 0x5DU, 0x42U, 0x61U, 0x80U, 0xE6U, 0xE9U,
		0x71U, 0x05U, 0x17U, 0x5BU, 0xAFU, 0x70U, 0x30U, 0x18U,
		0xBCU, 0x23U, 0x18U, 0x15U, 0xCBU, 0xB8U, 0xA6U, 0x3EU,
		0x83U, 0xB8U, 0x4AU, 0xFEU, 0x38U, 0xFCU, 0x25U, 0x87U,
	};
	const uint8_t expected_out[TRNG_GEN_LEN] = {
		0x91U, 0x9AU, 0x6BU, 0x99U, 0xD5U, 0xBCU, 0x2CU, 0x11U,
		0x5FU, 0x3AU, 0xFCU, 0x0BU, 0x0EU, 0x7BU, 0xC7U, 0x69U,
		0x4DU, 0xE1U, 0xE5U, 0xFEU, 0x59U, 0x9EU, 0xAAU, 0x41U,
		0xD3U, 0x48U, 0xFDU, 0x3DU, 0xD2U, 0xC4U, 0x50U, 0x1EU,
	};
	uint8_t out[TRNG_GEN_LEN] = { 0 };

	if (!trng)
		return TEE_ERROR_GENERIC;

	memcpy(&tests.init_seed, ext_seed, sizeof(ext_seed));
	memcpy(tests.pstr, pers_str, sizeof(pers_str));

	if (trng_instantiate(trng, &tests))
		goto error;

	if (trng_generate(trng, out, sizeof(out), false))
		goto error;

	if (memcmp(out, expected_out, TRNG_GEN_LEN)) {
		EMSG("K.A.T mismatch");
		goto error;
	}

	if (trng_release(trng))
		goto error;

	return TEE_SUCCESS;
error:
	trng->status = TRNG_ERROR;
	return TEE_ERROR_GENERIC;
}

__maybe_unused
static TEE_Result trng_kat_test_v3(struct versal_trng *trng)
{
#define XTRNGPSX_EXAMPLE_SEEDLIFE			12U
#define XTRNGPSX_EXAMPLE_DFLENMUL			4U
#define XTRNGPSX_EXAMPLE_RESEED_DFLENMUL	3U
#define XTRNGPSX_ENTROPY_SIZE               80U
#define XTRNGPSX_RESEED_ENTROPY_SIZE        64U
#ifndef SDT
#define XTRNGPSX_PMC_DEVICE		0U /**< Device Id for PMC*/
#else
#define XTRNGPSX_PMC_DEVICE		XPAR_XTRNGPSX_0_BASEADDR /**< Device Id for PMC*/
#endif

	XTrngpsx_Instance Trngpsx; /* Instance of TRNGPSX */
	int Status = XST_SUCCESS;
	XTrngpsx_Config *Config;

	IMSG("trng_kat_test_v3");

	/*
	 * Initialize the TRNGPSX driver so that it's ready to use look up
	 * configuration in the config table, then initialize it.
	 */

	Config = XTrngpsx_LookupConfig(XTRNGPSX_PMC_DEVICE);
	if (NULL == Config) {
		IMSG("LookupConfig Failed \n\r");
		goto END;
	}

	//Force BaseAddress
	Config->BaseAddress = trng->cfg.addr;

	/* Initialize the TRNGPSX driver so that it is ready to use. */
	Status = XTrngpsx_CfgInitialize(&Trngpsx, Config, Config->BaseAddress);
	if (Status != XST_SUCCESS) {
		IMSG("CfgInitialize Failed, Status: 0x%08x\n\r", Status);
		goto END;
	}
	Status = XTrngpsx_PreOperationalSelfTests(&Trngpsx);
	if (Status != XST_SUCCESS) {
		IMSG("KAT Failed, Status: 0x%08x\n\r", Status);
		goto END;
	}

	Status = XST_SUCCESS;
END:
	return Status;
}

static TEE_Result trng_kat_test_v2(struct versal_trng *trng)
{
	struct trng_usr_cfg tests = {
		.mode = TRNG_DRNG,
		.seed_life = 2,
		.dfmul = 7,
		.predict_en = false,
		.iseed_en = true,
		.pstr_en = true,
		.df_disable = false,
	};
	const uint8_t ext_seed[TRNG_V2_SEED_LEN] = {
		0x3BU, 0xC3U, 0xEDU, 0x64U, 0xF4U, 0x80U, 0x1CU, 0xC7U,
		0x14U, 0xCCU, 0x35U, 0xEDU, 0x57U, 0x01U, 0x2AU, 0xE4U,
		0xBCU, 0xEFU, 0xDEU, 0xF6U, 0x7CU, 0x46U, 0xA6U, 0x34U,
		0xC6U, 0x79U, 0xE8U, 0x91U, 0x5DU, 0xB1U, 0xDBU, 0xA7U,
		0x49U, 0xA5U, 0xBBU, 0x4FU, 0xEDU, 0x30U, 0xB3U, 0x7BU,
		0xA9U, 0x8BU, 0xF5U, 0x56U, 0x4DU, 0x40U, 0x18U, 0x9FU,
		0x66U, 0x4EU, 0x39U, 0xC0U, 0x60U, 0xC8U, 0x8EU, 0xF4U,
		0x1CU, 0xB9U, 0x9DU, 0x7BU, 0x97U, 0x8BU, 0x69U, 0x62U,
		0x45U, 0x0CU, 0xD4U, 0x85U, 0xFCU, 0xDCU, 0x5AU, 0x2BU,
		0xFDU, 0xABU, 0x92U, 0x4AU, 0x12U, 0x52U, 0x7DU, 0x45U,
		0xD2U, 0x61U, 0x0AU, 0x06U, 0x74U, 0xA7U, 0x88U, 0x36U,
		0x4BU, 0xA2U, 0x65U, 0xEEU, 0x71U, 0x0BU, 0x5AU, 0x4EU,
		0x33U, 0xB2U, 0x7AU, 0x2EU, 0xC0U, 0xA6U, 0xF2U, 0x7DU,
		0xBDU, 0x7DU, 0xDFU, 0x07U, 0xBBU, 0xE2U, 0x86U, 0xFFU,
		0xF0U, 0x8EU, 0xA4U, 0xB1U, 0x46U, 0xDBU, 0xF7U, 0x8CU,
		0x3CU, 0x62U, 0x4DU, 0xF0U, 0x51U, 0x50U, 0xE7U, 0x85U
	};
	uint8_t reseed_entropy[TRNG_V2_SEED_LEN] = {
		0xDFU, 0x5EU, 0x4DU, 0x4FU, 0x38U, 0x9EU, 0x2AU, 0x3EU,
		0xF2U, 0xABU, 0x46U, 0xE3U, 0xA0U, 0x26U, 0x77U, 0x84U,
		0x0BU, 0x9DU, 0x29U, 0xB0U, 0x5DU, 0xCEU, 0xC8U, 0xC3U,
		0xF9U, 0x4DU, 0x32U, 0xF7U, 0xBAU, 0x6FU, 0xA3U, 0xB5U,
		0x35U, 0xCBU, 0xC7U, 0x5CU, 0x62U, 0x48U, 0x01U, 0x65U,
		0x3AU, 0xAAU, 0x34U, 0x2DU, 0x89U, 0x6EU, 0xEFU, 0x6FU,
		0x69U, 0x96U, 0xE7U, 0x84U, 0xDAU, 0xEFU, 0x4EU, 0xBEU,
		0x27U, 0x4EU, 0x9FU, 0x88U, 0xB1U, 0xA0U, 0x7FU, 0x83U,
		0xDBU, 0x4AU, 0xA9U, 0x42U, 0x01U, 0xF1U, 0x84U, 0x71U,
		0xA9U, 0xEFU, 0xB9U, 0xE8U, 0x7FU, 0x81U, 0xC7U, 0xC1U,
		0x6CU, 0x5EU, 0xACU, 0x00U, 0x47U, 0x34U, 0xA1U, 0x75U,
		0xC0U, 0xE8U, 0x7FU, 0x48U, 0x00U, 0x45U, 0xC9U, 0xE9U,
		0x41U, 0xE3U, 0x8DU, 0xD8U, 0x4AU, 0x63U, 0xC4U, 0x94U,
		0x77U, 0x59U, 0xD9U, 0x50U, 0x2AU, 0x1DU, 0x4CU, 0x47U,
		0x64U, 0xA6U, 0x66U, 0x60U, 0x16U, 0xE7U, 0x29U, 0xC0U,
		0xB1U, 0xCFU, 0x3BU, 0x3FU, 0x54U, 0x49U, 0x31U, 0xD4U
	};
	const uint8_t pers_str[TRNG_PERS_STR_LEN] = {
		0xB2U, 0x80U, 0x7EU, 0x4CU, 0xD0U, 0xE4U, 0xE2U, 0xA9U,
		0x2FU, 0x1FU, 0x5DU, 0xC1U, 0xA2U, 0x1FU, 0x40U, 0xFCU,
		0x1FU, 0x24U, 0x5DU, 0x42U, 0x61U, 0x80U, 0xE6U, 0xE9U,
		0x71U, 0x05U, 0x17U, 0x5BU, 0xAFU, 0x70U, 0x30U, 0x18U,
		0xBCU, 0x23U, 0x18U, 0x15U, 0xCBU, 0xB8U, 0xA6U, 0x3EU,
		0x83U, 0xB8U, 0x4AU, 0xFEU, 0x38U, 0xFCU, 0x25U, 0x87U,
	};
	const uint8_t expected_out[TRNG_GEN_LEN] = {
		0xEEU, 0xA7U, 0x5BU, 0xB6U, 0x2BU, 0x97U, 0xF0U, 0xC0U,
		0x0FU, 0xD6U, 0xABU, 0x13U, 0x00U, 0x87U, 0x7EU, 0xF4U,
		0x00U, 0x7FU, 0xD7U, 0x56U, 0xFEU, 0xE5U, 0xDFU, 0xA6U,
		0x55U, 0x5BU, 0xB2U, 0x86U, 0xDDU, 0x81U, 0x73U, 0xB2U
	};
	uint8_t out[TRNG_GEN_LEN] = { 0 };

	if (!trng)
		return TEE_ERROR_GENERIC;

	memcpy(&tests.init_seed, ext_seed, sizeof(ext_seed));
	memcpy(tests.pstr, pers_str, sizeof(pers_str));

	if (trng_instantiate(trng, &tests))
		goto error;

	// dump_trng_usr_cfg(tests);

	if (trng_reseed(trng, reseed_entropy, 7))
		goto error;

	if (trng_generate(trng, out, sizeof(out), false))
		goto error;

	// int i;
	// IMSG("out");
	// for(i = 0; i < TRNG_GEN_LEN; i++)
	// {
	// 	IMSG("0x%08" PRIx32, out[i]);
	// }

	// IMSG("expected_out");
	// for(i = 0; i < TRNG_GEN_LEN; i++)
	// {
	// 	IMSG("0x%08" PRIx32, expected_out[i]);
	// }
	if (memcmp(out, expected_out, TRNG_GEN_LEN)) {
		EMSG("K.A.T mismatch");
		goto error;
	}
	if (trng_release(trng))
		goto error;
	return TEE_SUCCESS;
error:
	trng->status = TRNG_ERROR;
	return TEE_ERROR_GENERIC;
}

TEE_Result versal_trng_get_random_bytes(struct versal_trng *trng,
					void *buf, size_t len)
{
	uint8_t random[TRNG_SEC_STRENGTH_LEN] = { 0 };
	uint8_t *p = buf;
	size_t i = 0;

	for (i = 0; i < len / TRNG_SEC_STRENGTH_LEN; i++) {
		if (trng_generate(trng, p + i * TRNG_SEC_STRENGTH_LEN,
				  TRNG_SEC_STRENGTH_LEN, false))
			panic();
	}

	if (len % TRNG_SEC_STRENGTH_LEN) {
		if (trng_generate(trng, random, TRNG_SEC_STRENGTH_LEN,
				  false))
			panic();
		memcpy(p + i * TRNG_SEC_STRENGTH_LEN, random,
		       len % TRNG_SEC_STRENGTH_LEN);
	}

	return TEE_SUCCESS;
}

TEE_Result versal_trng_hw_init(struct versal_trng *trng,
			       struct trng_usr_cfg *usr_cfg)
{
	trng->cfg.addr = (vaddr_t)core_mmu_add_mapping(MEM_AREA_IO_SEC,
						       trng->cfg.base,
						       trng->cfg.len);
	if (!trng->cfg.addr) {
		EMSG("Failed to map TRNG");
		panic();
	}

	switch (trng->cfg.version) {
	case TRNG_V1:
		if (trng_kat_test(trng)) {
			EMSG("KAT Failed");
			panic();
		}
		break;

	case TRNG_V2:
		// trng_kat_test_v3(trng);
		if (trng_kat_test_v2(trng)) {
			EMSG("KAT Failed");
			panic();
		}
		break;

	default:
		return TEE_ERROR_BAD_PARAMETERS;
	}

	if (trng_health_test(trng)) {
		EMSG("RunHealthTest Failed");
		panic();
	}

	if (trng_instantiate(trng, usr_cfg)) {
		EMSG("Driver instantiation Failed");
		panic();
	}

	if (trng_reseed(trng, NULL, usr_cfg->dfmul)) {
		EMSG("Reseed Failed");
		panic();
	}

	return TEE_SUCCESS;
}

#ifndef CFG_VERSAL_RNG_PLM
#define TRNG_BASE            0xF1230000
#define TRNG_SIZE            0x10000

static struct versal_trng versal_trng = {
	.cfg.base = TRNG_BASE,
	.cfg.len = TRNG_SIZE,
	.cfg.version = TRNG_V1,
};

TEE_Result hw_get_random_bytes(void *buf, size_t len)
{
	return versal_trng_get_random_bytes(&versal_trng, buf, len);
}

static TEE_Result trng_hrng_mode_init(void)
{
	const uint8_t pers_str[TRNG_PERS_STR_LEN] = {
		0xB2, 0x80, 0x7E, 0x4C, 0xD0, 0xE4, 0xE2, 0xA9,
		0x2F, 0x1F, 0x5D, 0xC1, 0xA2, 0x1F, 0x40, 0xFC,
		0x1F, 0x24, 0x5D, 0x42, 0x61, 0x80, 0xE6, 0xE9,
		0x71, 0x05, 0x17, 0x5B, 0xAF, 0x70, 0x30, 0x18,
		0xBC, 0x23, 0x18, 0x15, 0xCB, 0xB8, 0xA6, 0x3E,
		0x83, 0xB8, 0x4A, 0xFE, 0x38, 0xFC, 0x25, 0x87,
	};

	/* configure in hybrid mode with derivative function enabled */
	struct trng_usr_cfg usr_cfg = {
		.mode = TRNG_HRNG,
		.seed_life = CFG_VERSAL_TRNG_SEED_LIFE,
		.predict_en = false,
		.df_disable = false,
		.dfmul = CFG_VERSAL_TRNG_DF_MUL,
		.iseed_en =  false,
		.pstr_en = true,
	};

	memcpy(usr_cfg.pstr, pers_str, TRNG_PERS_STR_LEN);

	return versal_trng_hw_init(&versal_trng, &usr_cfg);
}

driver_init(trng_hrng_mode_init);
#else
#define SEC_MODULE_SHIFT 8
#define SEC_MODULE_ID 5

#define CRYPTO_API_ID(__x) (SHIFT_U32(SEC_MODULE_ID, SEC_MODULE_SHIFT) | (__x))

#define VERSAL_TRNG_GENERATE 22

#define VERSAL_TRNG_SEC_STRENGTH_IN_BYTES 32

TEE_Result hw_get_random_bytes(void *buf, size_t len)
{
	uint32_t a = 0;
	uint32_t b = 0;
	struct versal_ipi_cmd cmd = { };
	struct versal_mbox_mem p = { };
	TEE_Result ret = TEE_SUCCESS;
	uint32_t status = 0;
	uint32_t offset = 0;

	ret = versal_mbox_alloc(len, NULL, &p);
	if (ret)
		return ret;

	cmd.data[0] = CRYPTO_API_ID(VERSAL_TRNG_GENERATE);
	cmd.ibuf[0].mem = p;

	while (len > VERSAL_TRNG_SEC_STRENGTH_IN_BYTES) {
		reg_pair_from_64(virt_to_phys(p.buf) + offset, &b, &a);

		cmd.data[1] = a;
		cmd.data[2] = b;
		cmd.data[3] = (uint32_t)VERSAL_TRNG_SEC_STRENGTH_IN_BYTES;

		ret = versal_mbox_notify_pmc(&cmd, NULL, &status);
		if (!ret) {
			memcpy((uint8_t *)buf + offset,
			       (uint8_t *)p.buf + offset,
			       VERSAL_TRNG_SEC_STRENGTH_IN_BYTES);
		} else {
			DMSG("Getting randomness returned 0x%" PRIx32, status);
			goto out;
		}

		offset += VERSAL_TRNG_SEC_STRENGTH_IN_BYTES;
		len -= VERSAL_TRNG_SEC_STRENGTH_IN_BYTES;
	}

	reg_pair_from_64(virt_to_phys(p.buf) + offset, &b, &a);

	cmd.data[1] = a;
	cmd.data[2] = b;
	cmd.data[3] = (uint32_t)len;

	ret = versal_mbox_notify_pmc(&cmd, NULL, &status);
	if (!ret)
		memcpy((uint8_t *)buf + offset, (uint8_t *)p.buf + offset, len);
	else
		DMSG("Getting randomness returned 0x%" PRIx32, status);

out:
	versal_mbox_free(&p);
	return ret;
}
#endif

void plat_rng_init(void)
{
}
