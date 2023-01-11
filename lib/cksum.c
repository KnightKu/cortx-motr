/* -*- C -*- */
/*
 * Copyright (c) 2020-2021 Seagate Technology LLC and/or its Affiliates
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
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */


#include "lib/memory.h"               /* m0_alloc, m0_free */
#include "lib/cksum_data.h"

#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_LIB
#include "lib/trace.h"

#define m0_cksum_print(buf, seg, dbuf, msg) \
do { \
	struct m0_vec *vec = &(buf)->ov_vec; \
	char *dst = (char *)(buf)->ov_buf[seg]; \
	char *data = (char *)(dbuf)->ov_buf[seg]; \
	M0_LOG(M0_DEBUG, msg " count[%d] = %"PRIu64 \
		   " cksum = %c%c data = %c%c", \
		   seg, vec->v_count[seg], dst[0], dst[1], data[0],data[1]); \
} while(0)

/**
 * Calculate checksum/protection info for data/KV
 *
 * @param pi  pi struct m0_md5_pi
 *        This function will calculate the checksum and set
 *        pi_value field of struct m0_md5_pi.
 * @param seed seed value (pis_obj_id+pis_data_unit_offset) required to
 *        calculate the checksum. If this pointer is NULL that means either
 *        this checksum calculation is meant for KV or user does
 *        not want seeding.
 * @param m0_bufvec - Set of buffers for which checksum is computed.
 * @param flag if flag is M0_PI_CALC_UNIT_ZERO, it means this api is called for
 *        first data unit and MD5_Init should be invoked.
 */
M0_INTERNAL int m0_calculate_md5(struct m0_md5_pi *pi,
				 struct m0_pi_seed *seed,
				 struct m0_bufvec *bvec,
				 enum m0_pi_calc_flag flag)
{
#if HAS_MD5
	MD5_CTX context;
	int i;
	int rc;

	M0_PRE(pi != NULL);
	M0_PRE(ergo(bvec != NULL && bvec->ov_vec.v_nr != 0,
		    bvec != NULL && bvec->ov_vec.v_count != NULL &&
		    bvec->ov_buf != NULL));

	pi->pimd5_hdr.pih_size = sizeof(struct m0_md5_pi) /
				 M0_CKSUM_DATA_ROUNDOFF_BYTE;
	if (M0_CKSUM_PAD_MD5)
		memset(pi->pimd5_pad,0,sizeof(pi->pimd5_pad));

	/* This call is for first data unit, need to initialize prev_context */
	if (flag & M0_PI_CALC_UNIT_ZERO) {
		rc = MD5_Init(&context);
		if (rc != 1) {
			return M0_ERR_INFO(rc, "MD5_Init failed v_nr: %d",
					   bvec->ov_vec.v_nr);
		}
	}

	if (bvec != NULL) {
		for (i = 0; i < bvec->ov_vec.v_nr; i++) {
			rc = MD5_Update(&context, bvec->ov_buf[i],
					bvec->ov_vec.v_count[i]);
			if (rc != 1) {
				return M0_ERR_INFO(rc, "MD5_Update failed."
						   "v_nr=%d, "
						   "ov_buf[%d]=%p, "
						   "ov_vec.v_count[%d]=%lu",
						   bvec->ov_vec.v_nr, i,
						   bvec->ov_buf[i], i,
						   bvec->ov_vec.v_count[i]);
			}
		}
	}

	if (seed != NULL) {
		/**
		 * seed_str have string represention for 3 uint64_t(8 bytes)
		 * range for uint64_t is 0 to 18,446,744,073,709,551,615 at
		 * max 20 chars per var, for three var it will be 3*20, +1 '\0'.
		 * seed_str needs to be 61 bytes, round off and taking 64 bytes.
		 */
		char seed_str[64] = {'\0'};
		snprintf(seed_str, sizeof(seed_str), "%" PRIx64 "%" PRIx64
			 "%"PRIx64, seed->pis_obj_id.f_container,
			 seed->pis_obj_id.f_key, seed->pis_data_unit_offset);
		rc = MD5_Update(&context, (unsigned char *)seed_str,
				sizeof(seed_str));
		if (rc != 1) {
			return M0_ERR_INFO(rc, "MD5_Update fail =%d"
					   "f_container 0x%" PRIx64
					   " f_key 0x%"PRIx64
					   " data_unit_offset 0x%" PRIx64
					   " seed_str %s",
					   bvec->ov_vec.v_nr,
					   seed->pis_obj_id.f_container,
					   seed->pis_obj_id.f_key,
					   seed->pis_data_unit_offset,
					   (char *)seed_str);
		}
	}

	if (!(flag & M0_PI_SKIP_CALC_FINAL)) {
		rc = MD5_Final(pi->pimd5_value, &context);
		if (rc != 1) {
			return M0_ERR_INFO(rc, "MD5_Final fail =%d",
					   bvec->ov_vec.v_nr);
		}
	}
#endif /* HAS_MD5 */
	return  0;
}

/**
 * Calculate checksum/protection info for data/KV
 *
 * @param pi  pi struct m0_md5_inc_context_pi
 *        This function will calculate the checksum and set
 *        pi_value field of struct m0_md5_inc_context_pi.
 * @param seed seed value (pis_obj_id+pis_data_unit_offset) required to
 *        calculate the checksum. If this pointer is NULL that means either
 *        this checksum calculation is meant for KV or user does
 *        not want seeding.
 * @param m0_bufvec - Set of buffers for which checksum is computed.
 * @param flag if flag is M0_PI_CALC_UNIT_ZERO, it means this api is called for
 *        first data unit and MD5_Init should be invoked.
 * @param[out] curr_context context of data unit N, will be required to
 *             calculate checksum for next data unit, N+1. Curre_context is
 *             calculated and set in this func.
 * @param[out] pi_value_without_seed - Caller may need checksum value without
 *             seed and with seed. With seed checksum is set in pi_value of PI
 *             type struct. Without seed checksum is set in this field.
 */
M0_INTERNAL int m0_calculate_md5_inc_context(
		struct m0_md5_inc_context_pi *pi,
		struct m0_pi_seed *seed,
		struct m0_bufvec *bvec,
		enum m0_pi_calc_flag flag,
		unsigned char *curr_context,
		unsigned char *pi_value_without_seed)
{
#if HAS_MD5
	MD5_CTX context;
	int     i;
	int     rc;

	M0_PRE(pi != NULL);
	M0_PRE(curr_context != NULL);
	M0_PRE(ergo(bvec != NULL && bvec->ov_vec.v_nr != 0,
				bvec != NULL && bvec->ov_vec.v_count != NULL &&
				bvec->ov_buf != NULL));

	pi->pimd5c_hdr.pih_size = sizeof(struct m0_md5_inc_context_pi) /
				  M0_CKSUM_DATA_ROUNDOFF_BYTE;
	if (M0_CKSUM_PAD_MD5_INC_CXT)
		memset(pi->pi_md5c_pad,0,sizeof(pi->pi_md5c_pad));

	/* This call is for first data unit, need to initialize prev_context */
	if (flag & M0_PI_CALC_UNIT_ZERO) {
		rc = MD5_Init((MD5_CTX *)&pi->pimd5c_prev_context);
		if (rc != 1) {
			return M0_ERR_INFO(rc, "MD5_Init failed v_nr=%d",
					   bvec->ov_vec.v_nr);
		}
	}

	/* memcpy, so that we do not change the prev_context */
	memcpy(curr_context, &pi->pimd5c_prev_context, sizeof(MD5_CTX));

	/* get the curr context by updating it*/
	if (bvec != NULL) {
		for (i = 0; i < bvec->ov_vec.v_nr; i++) {
			rc = MD5_Update((MD5_CTX *)curr_context,
					bvec->ov_buf[i],
					bvec->ov_vec.v_count[i]);
			if (rc != 1) {
				return M0_ERR_INFO(rc, "MD5_Update failed."
						   "v_nr=%d, "
						   "ov_buf[%d]=%p, "
						   "ov_vec.v_count[%d]=%lu",
						   bvec->ov_vec.v_nr, i,
						   bvec->ov_buf[i], i,
						   bvec->ov_vec.v_count[i]);
			}
		}
	}

	/* If caller wants checksum without seed and with seed, caller needs to
	 * pass 'pi_value_without_seed'. pi_value will be used to return with
	 * seed checksum. 'pi_value_without_seed' will be used to return non
	 * seeded checksum.
	 */
	if (pi_value_without_seed != NULL) {
		/*
		 * NOTE: MD5_final() changes the context itself and curr_context
		 * should not be finalised, thus copy it and use it for
		 * MD5_final
		 */
		memcpy((void *)&context, (void *)curr_context, sizeof(MD5_CTX));

		rc = MD5_Final(pi_value_without_seed, &context);
		if (rc != 1) {
			return M0_ERR_INFO(rc, "MD5_Final failed"
					   "pi_value_without_seed=%p"
					   "v_nr=%d", pi_value_without_seed,
					   bvec->ov_vec.v_nr);
		}
	}

	/* if seed is passed, memcpy and update the context calculated so far.
	 * calculate checksum with seed, set the pi_value with seeded checksum.
	 * If seed is not passed than memcpy context and calculate checksum
	 * without seed, set the pi_value with unseeded checksum.
	 * NOTE: curr_context will always have context without seed.
	 */
	memcpy((void *)&context, (void *)curr_context, sizeof(MD5_CTX));

	if (seed != NULL) {
		/**
		 * seed_str have string represention for 3 uint64_t(8 bytes)
		 * range for uint64_t is 0 to 18,446,744,073,709,551,615 at
		 * max 20 chars per var, for three var it will be 3*20, +1 '\0'.
		 * seed_str needs to be 61 bytes, round off and taking 64 bytes.
		 */
		char seed_str[64] = {'\0'};
		snprintf(seed_str, sizeof(seed_str), "%" PRIx64 "%" PRIx64
			 "%"PRIx64, seed->pis_obj_id.f_container,
			 seed->pis_obj_id.f_key, seed->pis_data_unit_offset);
		rc = MD5_Update(&context, (unsigned char *)seed_str,
				sizeof(seed_str));
		if (rc != 1) {

			return M0_ERR_INFO(rc, "MD5_Update fail v_nr=%d"
					   "f_container 0x%" PRIx64
					   " f_key 0x%"PRIx64
					   " data_unit_offset 0x%" PRIx64
					   " seed_str %s",
					   bvec->ov_vec.v_nr,
					   seed->pis_obj_id.f_container,
					   seed->pis_obj_id.f_key,
					   seed->pis_data_unit_offset,
					   (char *)seed_str);
		}
	}

	if (!(flag & M0_PI_SKIP_CALC_FINAL)) {
		rc = MD5_Final(pi->pimd5c_value, &context);
		if (rc != 1) {
			return M0_ERR_INFO(rc, "MD5_Final fail v_nr=%d",
					   bvec->ov_vec.v_nr);
		}
	}
#endif
	return  0;
}

M0_INTERNAL uint32_t m0_cksum_get_size(enum m0_pi_algo_type pi_type)
{
#if HAS_MD5
	switch (pi_type) {
	case M0_PI_TYPE_MD5_INC_CONTEXT:
		return sizeof(struct m0_md5_inc_context_pi);
		break;
	case M0_PI_TYPE_MD5:
		return sizeof(struct m0_md5_pi);
		break;
	default:
		break;
	}
#endif
	return 0;
}

M0_INTERNAL uint32_t m0_cksum_get_max_size(void)
{
	return (sizeof(struct m0_md5_pi) >
		sizeof(struct m0_md5_inc_context_pi) ?
		sizeof(struct m0_md5_pi) :
		sizeof(struct m0_md5_inc_context_pi));
}

int m0_client_calculate_pi(struct m0_generic_pi *pi,
			   struct m0_pi_seed *seed,
			   struct m0_bufvec *bvec,
			   enum m0_pi_calc_flag flag,
			   unsigned char *curr_context,
			   unsigned char *pi_value_without_seed)
{
	int rc = 0;
	M0_ENTRY();
#if HAS_MD5
	switch (pi->pi_hdr.pih_type) {
	case M0_PI_TYPE_MD5: {
		struct m0_md5_pi *md5_pi =
			(struct m0_md5_pi *) pi;
		rc = m0_calculate_md5(md5_pi, seed, bvec, flag);
		}
		break;
	case M0_PI_TYPE_MD5_INC_CONTEXT: {
		struct m0_md5_inc_context_pi *md5_context_pi =
			(struct m0_md5_inc_context_pi *) pi;
		rc = m0_calculate_md5_inc_context(md5_context_pi, seed, bvec,
						  flag, curr_context,
						  pi_value_without_seed);
		}
		break;
	}
#endif
	return rc;
}

M0_EXPORTED(m0_client_calculate_pi);

bool m0_calc_verify_cksum_one_unit(struct m0_generic_pi *pi,
                                   struct m0_pi_seed *seed,
                                   struct m0_bufvec *bvec)
{
#if HAS_MD5
	switch (pi->pi_hdr.pih_type) {
	case M0_PI_TYPE_MD5_INC_CONTEXT:
	{
		struct m0_md5_inc_context_pi md5_ctx_pi;
		unsigned char *curr_context = m0_alloc(sizeof(MD5_CTX));
		memset(&md5_ctx_pi, 0, sizeof(struct m0_md5_inc_context_pi));
		if (curr_context == NULL) {
			return false;
		}
		memcpy(md5_ctx_pi.pimd5c_prev_context,
		       ((struct m0_md5_inc_context_pi *)pi)->pimd5c_prev_context,
		       sizeof(MD5_CTX));
		md5_ctx_pi.pimd5c_hdr.pih_type = M0_PI_TYPE_MD5_INC_CONTEXT;
		m0_client_calculate_pi((struct m0_generic_pi *)&md5_ctx_pi,
					seed, bvec, M0_PI_NO_FLAG,
					curr_context, NULL);
		m0_free(curr_context);
		if (memcmp(((struct m0_md5_inc_context_pi *)pi)->pimd5c_value,
			   md5_ctx_pi.pimd5c_value,
			   MD5_DIGEST_LENGTH) == 0) {
			return true;
		}
		else {
			M0_LOG(M0_ERROR, "checksum fail "
					 "f_container 0x%" PRIx64
					 " f_key 0x%"PRIx64
					 " data_unit_offset 0x%"PRIx64,
					 seed->pis_obj_id.f_container,
					 seed->pis_obj_id.f_key,
					 seed->pis_data_unit_offset);
			return false;
		}
		break;
	}
	default:
		M0_IMPOSSIBLE("pi_type = %d", pi->pi_hdr.pih_type);
	}
#endif
	return true;
}

M0_EXPORTED(m0_calc_verify_cksum_one_unit);

#undef M0_TRACE_SUBSYSTEM

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
/*
 * vim: tabstop=8 shiftwidth=8 noexpandtab textwidth=80 nowrap
 */
