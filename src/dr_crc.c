/*
 * DisketteRecover - CRC-16/CCITT-FALSE plus the linear machinery that
 * makes the bit-flip search cheap.
 *
 * The CRC of a fixed-length message is affine over GF(2):
 *
 *     crc(m) = L(m) XOR K
 *
 * so for any error vector e,  crc(m XOR e) = crc(m) XOR L(e).
 * L(e) for a single set bit depends only on the bit's distance from the
 * end of the message, so all nbits masks are built in one linear pass
 * and a candidate of any weight is then tested with a couple of XORs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "dr.h"

#define POLY 0x1021

uint16_t dr_crc16(const uint8_t *buf, int len)
{
	uint16_t crc = 0xFFFF;
	int i, b;

	for (i = 0; i < len; i++) {
		crc ^= (uint16_t)buf[i] << 8;
		for (b = 0; b < 8; b++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ POLY)
					     : (uint16_t)(crc << 1);
	}
	return crc;
}

/* Advance a register by one zero bit. */
static inline uint16_t shift0(uint16_t r)
{
	return (r & 0x8000) ? (uint16_t)((r << 1) ^ POLY) : (uint16_t)(r << 1);
}

void dr_crc16_bit_masks(int nbits, uint16_t *masks)
{
	uint16_t m;
	int p;

	if (nbits <= 0)
		return;

	/* Flipping the very last message bit injects the polynomial once
	 * and nothing else follows it. */
	m = POLY;
	masks[nbits - 1] = m;

	for (p = nbits - 2; p >= 0; p--) {
		m = shift0(m);
		masks[p] = m;
	}
}
