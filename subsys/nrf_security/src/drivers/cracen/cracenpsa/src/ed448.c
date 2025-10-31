/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *  
 */
#include <string.h>
#include <sxsymcrypt/hashdefs.h>
#include <silexpk/ed448.h>
#include <cracen/ec_helpers.h>
#include <sxsymcrypt/hash.h>
#include <cracen/mem_helpers.h>
#include <cracen/statuscodes.h>
#include "common.h"
#include "cracen_psa_eddsa.h"

#define AREA2_MEM_OFFSET 57
#define AREA4_MEM_OFFSET 171

/* This is the ASCII string with the
 * PHflag 1 and context size 0 appended as defined in:
 * https://datatracker.ietf.org/doc/html/rfc8032.html#section-2.
 * It is used for domain separation between Ed448 and Ed448ph.
 * This can not be stored as a const due to hardware limitations
 */
static uint8_t dom4[] = {0x53, 0x69, 0x67, 0x45, 0x64, 0x34, 0x34, 0x38};

static int ed448_calculate_r(uint8_t *workmem, const uint8_t *message, size_t message_length,
			       bool prehash)
{
	uint8_t const *hash_array[] = {dom4, workmem, message};
	size_t hash_array_lengths[] = {sizeof(dom4), SX_ED448_SZ, message_length};
	size_t offset = prehash ? 0 : 1;
	size_t input_count = 3 - offset;

	return cracen_hash_all_inputs(hash_array + offset, hash_array_lengths + offset, input_count,
				      &sxhashalg_shake256_114, workmem + SX_ED448_DGST_SZ);
}

static int ed448_calculate_k(uint8_t *workmem, uint8_t *point_r, const uint8_t *message,
			       size_t message_length, bool prehash)
{
	uint8_t const *hash_array[] = {dom4, point_r, workmem, message};
	size_t hash_array_lengths[] = {sizeof(dom4), SX_ED448_SZ, SX_ED448_SZ, message_length};
	size_t offset = prehash ? 0 : 1;
	size_t input_count = 4 - offset;

	return cracen_hash_all_inputs(&hash_array[offset], &hash_array_lengths[offset], input_count,
				      &sxhashalg_shake256_114, workmem);
}

static int ed448_sign_internal(const uint8_t *priv_key, uint8_t *signature,
				 const uint8_t *message, size_t message_length, bool prehash)
{
	int status;
	uint8_t workmem[5 * SX_ED448_SZ];
	uint8_t pnt_r[SX_ED448_DGST_SZ];
	uint8_t *area_1 = workmem;
	uint8_t *area_2 = workmem + AREA2_MEM_OFFSET;
	uint8_t *area_4 = workmem + AREA4_MEM_OFFSET;

	/* Hash the private key, the digest is stored in the first 57 bytes of workmem*/
	status = cracen_hash_input(priv_key, SX_ED448_SZ, &sxhashalg_shake256_114, area_1);
	if (status != SX_OK) {
		return status;
	}

	/* Obtain r by hashing (prefix || message), where prefix is the second
	 * half of the private key digest.
	 */
	status = ed448_calculate_r(area_2, message, message_length, prehash);
	if (status != SX_OK) {
		return status;
	}

	/* Perform point multiplication R = [r]B. This is the encoded point R,
	 * which is the first part of the signature.
	 */
	status = sx_ed448_ptmult((const struct sx_ed448_dgst *)area_4,
				   (struct sx_ed448_pt *)pnt_r);
	if (status != SX_OK) {
		return status;
	}

	/* The secret scalar s is computed in place from the first half of the
	 * private key digest.
	 */
	decode_scalar_448(area_1);

	/* Clear second half of private key digest: sx_ed448_ptmult()
	 * works on an input of SX_ED448_DGST_SZ bytes.
	 */
	safe_memset(area_2, sizeof(workmem) - SX_ED448_SZ, 0, SX_ED448_SZ);
	/* Perform point multiplication A = [s]B,
	 * to obtain the public key A. which is stored in workmem[32:63]
	 */
	status = sx_ed448_ptmult((const struct sx_ed448_dgst *)area_1,
				   (struct sx_ed448_pt *)area_2);

	if (status != SX_OK) {
		return status;
	}

	status = ed448_calculate_k(area_2, pnt_r, message, message_length, prehash);
	if (status != SX_OK) {
		return status;
	}

	/* Compute (r + k * s) mod L. This gives the second part of the
	 * signature, which is the encoded S which is stored in pnt_r.
	 */
	status = sx_ed448_sign((const struct sx_ed448_dgst *)area_2,
				 (const struct sx_ed448_dgst *)area_4,
				 (const struct sx_ed448_v *)area_1,
				 (struct sx_ed448_v *)(pnt_r + SX_ED448_PT_SZ));
	if (status != SX_OK) {
		return status;
	}

	memcpy(signature, pnt_r, SX_ED448_DGST_SZ);
	safe_memzero(workmem, sizeof(workmem));

	return status;
}

int cracen_ed448_sign(const uint8_t *priv_key, uint8_t *signature, const uint8_t *message,
			size_t message_length)
{
	return ed448_sign_internal(priv_key, signature, message, message_length, false);
}

int cracen_ed448ph_sign(const uint8_t *priv_key, uint8_t *signature, const uint8_t *message,
			  size_t message_length, bool is_message)
{
	uint8_t hashedmessage[SX_ED448_DGST_SZ];
	int status;

	if (is_message) {
		status = cracen_hash_input(message, message_length, &sxhashalg_shake256_114,
					   hashedmessage);
		if (status != SX_OK) {
			return status;
		}

		return ed448_sign_internal(priv_key, signature, hashedmessage, SX_ED448_DGST_SZ,
					     true);
	} else {
		return ed448_sign_internal(priv_key, signature, message, message_length, true);
	}
}

static int ed448_verify_internal(const uint8_t *pub_key, const uint8_t *message,
				   size_t message_length, const uint8_t *signature, bool prehash)
{
	int status;
	uint8_t digest[SX_ED448_DGST_SZ];
	size_t ed448_sz = SX_ED448_SZ;
	size_t offset = prehash ? 0 : 1;
	size_t input_count = 4 - offset;

	uint8_t const *hash_array[] = {dom4, signature, pub_key, message};
	size_t hash_array_lengths[] = {sizeof(dom4), ed448_sz, ed448_sz, message_length};

	status = cracen_hash_all_inputs(&hash_array[offset], &hash_array_lengths[offset],
					input_count, &sxhashalg_shake256_114, digest);
	if (status != SX_OK) {
		return status;
	}
	status =
		sx_ed448_verify((struct sx_ed448_dgst *)digest, (struct sx_ed448_pt *)pub_key,
				  (const struct sx_ed448_v *)(signature + SX_ED448_SZ),
				  (const struct sx_ed448_pt *)signature);

	return status;
}

int cracen_ed448_verify(const uint8_t *pub_key, const uint8_t *message, size_t message_length,
			  const uint8_t *signature)
{
	return ed448_verify_internal(pub_key, message, message_length, signature, false);
}

int cracen_ed448ph_verify(const uint8_t *pub_key, const uint8_t *message, size_t message_length,
			    const uint8_t *signature, bool is_message)
{
	int status;
	uint8_t message_digest[SX_ED448_DGST_SZ];

	if (is_message) {
		status = cracen_hash_input(message, message_length, &sxhashalg_shake256_114,
					   message_digest);
		if (status != SX_OK) {
			return status;
		}

		return ed448_verify_internal(pub_key, message_digest, SX_ED448_DGST_SZ,
					       signature, true);
	}
	return ed448_verify_internal(pub_key, message, message_length, signature, true);
}

int cracen_ed448_create_pubkey(const uint8_t *priv_key, uint8_t *pub_key)
{
	int status;
	uint8_t digest[SX_ED448_DGST_SZ];
	uint8_t *pub_key_A = digest + SX_ED448_SZ;

	status = cracen_hash_input(priv_key, SX_ED448_SZ, &sxhashalg_shake256_114, digest);
	if (status != SX_OK) {
		return status;
	}
	/* The secret scalar s is computed in place from the first half of the
	 * private key digest.
	 */
	decode_scalar_448(digest);

	/* Clear second half of private key digest: ed448_ptmult()
	 * works on an input of SX_ED448_DGST_SZ bytes.
	 */
	safe_memset(pub_key_A, SX_ED448_SZ, 0, SX_ED448_SZ);

	/* Perform point multiplication A = [s]B, to obtain the public key A. */
	status = sx_ed448_ptmult((const struct sx_ed448_dgst *)digest,
				   (struct sx_ed448_pt *)pub_key_A);

	if (status != SX_OK) {
		return status;
	}

	memcpy(pub_key, pub_key_A, SX_ED448_SZ);
	safe_memzero(digest, SX_ED448_DGST_SZ);

	return status;
}
