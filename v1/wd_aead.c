/*
 * Copyright 2019 Huawei Technologies Co.,Ltd.All rights reserved.
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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/mman.h>

#include "wd.h"
#include "wd_util.h"
#include "wd_aead.h"

#define MAX_AEAD_AUTH_SIZE		64
#define MAX_AEAD_RETRY_CNT		20000000
#define AEAD_CCM_GCM_MIN		4U
#define AEAD_CCM_GCM_MAX		16

static int g_aead_mac_len[WCRYPTO_MAX_DIGEST_TYPE] = {
	WCRYPTO_SM3_LEN, WCRYPTO_MD5_LEN, WCRYPTO_SHA1_LEN,
	WCRYPTO_SHA256_LEN, WCRYPTO_SHA224_LEN,
	WCRYPTO_SHA384_LEN, WCRYPTO_SHA512_LEN,
	WCRYPTO_SHA512_224_LEN, WCRYPTO_SHA512_256_LEN
};

struct wcrypto_aead_cookie {
	struct wcrypto_aead_tag tag;
	struct wcrypto_aead_msg msg;
};

struct wcrypto_aead_ctx {
	struct wd_cookie_pool pool;
	unsigned long ctx_id;
	void *ckey;
	void *akey;
	__u16 ckey_bytes;
	__u16 akey_bytes;
	__u16 auth_size;
	__u16 iv_blk_size;
	struct wd_queue *q;
	struct wcrypto_aead_ctx_setup setup;
	__u64 long_data_len;
	__u8 *civ;
	__u8 *mac;
};

static void del_ctx_key(struct wcrypto_aead_ctx *ctx)
{
	struct wd_mm_br *br = &(ctx->setup.br);
	__u8 tmp[MAX_CIPHER_KEY_SIZE] = { 0 };

	/**
	 * When data_fmt is 'WD_SGL_BUF',  'akey' and 'ckey' is a sgl, and if u
	 * want to clear the SGL buffer, we can only use 'wd_sgl_cp_from_pbuf'
	 * whose 'pbuf' is all zero.
	 */
	if (ctx->ckey && ctx->ckey_bytes) {
		if (ctx->setup.data_fmt == WD_FLAT_BUF)
			memset(ctx->ckey, 0, MAX_CIPHER_KEY_SIZE);
		else if (ctx->setup.data_fmt == WD_SGL_BUF)
			wd_sgl_cp_from_pbuf(ctx->ckey, 0, tmp, MAX_CIPHER_KEY_SIZE);
	}

	if (ctx->akey && ctx->akey_bytes) {
		if (ctx->setup.data_fmt == WD_FLAT_BUF)
			memset(ctx->akey, 0, MAX_AEAD_KEY_SIZE);
		else if (ctx->setup.data_fmt == WD_SGL_BUF)
			wd_sgl_cp_from_pbuf(ctx->akey, 0, tmp, MAX_AEAD_KEY_SIZE);
	}

	if (br && br->free) {
		if (ctx->ckey)
			br->free(br->usr, ctx->ckey);
		if (ctx->akey)
			br->free(br->usr, ctx->akey);
		if (ctx->civ)
			br->free(br->usr, ctx->civ);
		if (ctx->mac)
			br->free(br->usr, ctx->mac);
	}
}

static int get_iv_block_size(int mode)
{
	int ret;

	/* AEAD just used AES and SM4 algorithm */
	switch (mode) {
	case WCRYPTO_CIPHER_CBC:
	case WCRYPTO_CIPHER_CCM:
		ret = AES_BLOCK_SIZE;
		break;
	case WCRYPTO_CIPHER_GCM:
		ret = GCM_BLOCK_SIZE;
		break;
	default:
		ret = 0;
	}

	return ret;
}

static int create_ctx_para_check(struct wd_queue *q,
	struct wcrypto_aead_ctx_setup *setup)
{
	if (!q || !q->qinfo || !setup) {
		WD_ERR("input param is NULL\n");
		return -WD_EINVAL;
	}

	if (!setup->br.alloc || !setup->br.free ||
		!setup->br.iova_map || !setup->br.iova_unmap) {
		WD_ERR("fail to create cipher ctx user mm br!\n");
		return -WD_EINVAL;
	}
	if (!q->capa.alg || strcmp(q->capa.alg, "aead")) {
		WD_ERR("fail to matching algorithm! %s\n", q->capa.alg);
		return -WD_EINVAL;
	}

	return WD_SUCCESS;
}

static int init_aead_cookie(struct wcrypto_aead_ctx *ctx,
	struct wcrypto_aead_ctx_setup *setup)
{
	struct wcrypto_aead_cookie *cookie;
	__u32 flags = ctx->q->capa.flags;
	__u32 cookies_num, i;
	int ret;

	cookies_num = wd_get_ctx_cookies_num(flags, WD_CTX_COOKIES_NUM);
	ret = wd_init_cookie_pool(&ctx->pool,
		sizeof(struct wcrypto_aead_cookie), cookies_num);
	if (ret) {
		WD_ERR("failed to init cookie pool!\n");
		return ret;
	}

	for (i = 0; i < cookies_num; i++) {
		cookie = (void *)((uintptr_t)ctx->pool.cookies +
			i * ctx->pool.cookies_size);
		cookie->msg.alg_type = WCRYPTO_AEAD;
		cookie->msg.calg = setup->calg;
		cookie->msg.cmode = setup->cmode;
		cookie->msg.dalg = setup->dalg;
		cookie->msg.dmode = setup->dmode;
		cookie->msg.data_fmt = setup->data_fmt;
		cookie->tag.wcrypto_tag.ctx = ctx;
		cookie->tag.wcrypto_tag.ctx_id = ctx->ctx_id;
		cookie->msg.usr_data = (uintptr_t)&cookie->tag;
	}

	return 0;
}

static int wcrypto_setup_qinfo(struct wcrypto_aead_ctx_setup *setup,
			       struct q_info *qinfo, __u32 *ctx_id)
{
	int ret = -WD_EINVAL;

	/* lock at ctx creating/deleting */
	wd_spinlock(&qinfo->qlock);
	if (!qinfo->br.alloc && !qinfo->br.iova_map)
		memcpy(&qinfo->br, &setup->br, sizeof(qinfo->br));

	if (qinfo->br.usr != setup->br.usr) {
		WD_ERR("Err mm br in creating aead ctx!\n");
		goto unlock;
	}

	if (qinfo->ctx_num >= WD_MAX_CTX_NUM) {
		WD_ERR("err: create too many aead ctx!\n");
		goto unlock;
	}

	ret = wd_alloc_id(qinfo->ctx_id, WD_MAX_CTX_NUM, ctx_id, 0,
		WD_MAX_CTX_NUM);
	if (ret) {
		WD_ERR("fail to alloc ctx id!\n");
		goto unlock;
	}
	qinfo->ctx_num++;
	ret = WD_SUCCESS;

unlock:
	wd_unspinlock(&qinfo->qlock);
	return ret;
}

/* Before initiate this context, we should get a queue from WD */
void *wcrypto_create_aead_ctx(struct wd_queue *q,
	struct wcrypto_aead_ctx_setup *setup)
{
	struct q_info *qinfo;
	struct wcrypto_aead_ctx *ctx;
	__u32 ctx_id = 0;
	int ret;

	if (create_ctx_para_check(q, setup))
		return NULL;

	qinfo = q->qinfo;
	/* lock at ctx creating/deleting */
	if (wcrypto_setup_qinfo(setup, qinfo, &ctx_id))
		return NULL;

	ctx = malloc(sizeof(struct wcrypto_aead_ctx));
	if (!ctx) {
		WD_ERR("fail to alloc ctx memory!\n");
		goto free_ctx_id;
	}
	memset(ctx, 0, sizeof(struct wcrypto_aead_ctx));
	memcpy(&ctx->setup, setup, sizeof(ctx->setup));
	ctx->q = q;
	ctx->ctx_id = ctx_id + 1;
	ctx->ckey = setup->br.alloc(setup->br.usr, MAX_CIPHER_KEY_SIZE);
	if (!ctx->ckey) {
		WD_ERR("fail to alloc cipher ctx key!\n");
		goto free_ctx;
	}
	ctx->akey = setup->br.alloc(setup->br.usr, MAX_AEAD_KEY_SIZE);
	if (!ctx->akey) {
		WD_ERR("fail to alloc authenticate ctx key!\n");
		goto free_ctx_ckey;
	}
	ctx->civ = setup->br.alloc(setup->br.usr, AES_BLOCK_SIZE);
	if (!ctx->civ) {
		WD_ERR("fail to alloc civ for aead ctx!\n");
		goto free_ctx_akey;
	}
	ctx->mac = setup->br.alloc(setup->br.usr, MAX_AEAD_AUTH_SIZE);
	if (!ctx->mac) {
		WD_ERR("fail to alloc mac for aead ctx!\n");
		goto free_ctx_civ;
	}

	ctx->iv_blk_size = get_iv_block_size(setup->cmode);
	ret = init_aead_cookie(ctx, setup);
	if (ret)
		goto free_ctx_mac;

	return ctx;

free_ctx_mac:
	setup->br.free(setup->br.usr, ctx->mac);
free_ctx_civ:
	setup->br.free(setup->br.usr, ctx->civ);
free_ctx_akey:
	setup->br.free(setup->br.usr, ctx->akey);
free_ctx_ckey:
	setup->br.free(setup->br.usr, ctx->ckey);
free_ctx:
	free(ctx);
free_ctx_id:
	wd_spinlock(&qinfo->qlock);
	qinfo->ctx_num--;
	wd_free_id(qinfo->ctx_id, WD_MAX_CTX_NUM, ctx_id, WD_MAX_CTX_NUM);
	wd_unspinlock(&qinfo->qlock);

	return NULL;
}

int wcrypto_aead_setauthsize(void *ctx, __u16 authsize)
{
	struct wcrypto_aead_ctx *ctxt = ctx;

	if (!ctx) {
		WD_ERR("input param is NULL!\n");
		return -WD_EINVAL;
	}

	if (ctxt->setup.cmode == WCRYPTO_CIPHER_CCM) {
		if (authsize < AEAD_CCM_GCM_MIN ||
		    authsize > AEAD_CCM_GCM_MAX ||
		    authsize % (AEAD_CCM_GCM_MIN >> 1)) {
			WD_ERR("failed to check aead CCM authsize, size = %u\n",
				authsize);
			return -WD_EINVAL;
		}
	} else if (ctxt->setup.cmode == WCRYPTO_CIPHER_GCM) {
		if (authsize < AEAD_CCM_GCM_MIN << 1 ||
		    authsize > AEAD_CCM_GCM_MAX) {
			WD_ERR("failed to check aead GCM authsize, size = %u\n",
				authsize);
			return -WD_EINVAL;
		}
	} else {
		if (ctxt->setup.dalg >= WCRYPTO_MAX_DIGEST_TYPE || !authsize ||
		    authsize > g_aead_mac_len[ctxt->setup.dalg]) {
			WD_ERR("failed to check aead mac authsize, size = %u\n",
				authsize);
			return -WD_EINVAL;
		}
	}

	ctxt->auth_size = authsize;

	return WD_SUCCESS;
}

int wcrypto_aead_getauthsize(void *ctx)
{
	struct wcrypto_aead_ctx *ctxt = ctx;

	if (!ctx) {
		WD_ERR("input param is NULL!\n");
		return -WD_EINVAL;
	}

	return ctxt->auth_size;
}

int wcrypto_aead_get_maxauthsize(void *ctx)
{
	struct wcrypto_aead_ctx *ctxt = ctx;

	if (!ctx) {
		WD_ERR("input param is NULL!\n");
		return -WD_EINVAL;
	}

	if (ctxt->setup.cmode == WCRYPTO_CIPHER_CCM ||
		ctxt->setup.cmode == WCRYPTO_CIPHER_GCM)
		return WCRYPTO_CCM_GCM_LEN;

	if (ctxt->setup.dalg >= WCRYPTO_MAX_DIGEST_TYPE) {
		WD_ERR("fail to check authenticate alg!\n");
		return -WD_EINVAL;
	}

	return g_aead_mac_len[ctxt->setup.dalg];
}

static int aes_key_len_check(__u16 ckey_len)
{
	switch (ckey_len) {
	case AES_KEYSIZE_128:
	case AES_KEYSIZE_192:
	case AES_KEYSIZE_256:
		return WD_SUCCESS;
	default:
		return -WD_EINVAL;
	}
}

static int cipher_key_len_check(int alg, __u16 ckey_len)
{
	int ret = WD_SUCCESS;

	switch (alg) {
	case WCRYPTO_CIPHER_SM4:
		if (ckey_len != SM4_KEY_SIZE)
			ret = -WD_EINVAL;
		break;
	case WCRYPTO_CIPHER_AES:
		ret = aes_key_len_check(ckey_len);
		break;
	default:
		return -WD_EINVAL;
	}

	return ret;
}

int wcrypto_set_aead_ckey(void *ctx, __u8 *key, __u16 key_len)
{
	struct wcrypto_aead_ctx *ctxt = ctx;
	int ret;

	if (!ctx || !ctxt->ckey || !key) {
		WD_ERR("input param is NULL!\n");
		return -WD_EINVAL;
	}

	ret = cipher_key_len_check(ctxt->setup.calg, key_len);
	if (ret != WD_SUCCESS) {
		WD_ERR("fail to check key length, alg = %u\n", ctxt->setup.calg);
		return ret;
	}

	ctxt->ckey_bytes = key_len;

	if (ctxt->setup.data_fmt == WD_SGL_BUF)
		wd_sgl_cp_from_pbuf(ctxt->ckey, 0, key, key_len);
	else
		memcpy(ctxt->ckey, key, key_len);

	return ret;
}

int wcrypto_set_aead_akey(void *ctx, __u8 *key, __u16 key_len)
{
	struct wcrypto_aead_ctx *ctxt = ctx;

	if (!ctx || !ctxt->akey || (key_len && !key)) {
		WD_ERR("failed to check authenticate key param!\n");
		return -WD_EINVAL;
	}

	if (ctxt->setup.dalg > WCRYPTO_SHA256) {
		if (key_len > MAX_HMAC_KEY_SIZE)
			goto err_key_len;
	} else {
		if (key_len > MAX_HMAC_KEY_SIZE >> 1)
			goto err_key_len;
	}

	ctxt->akey_bytes = key_len;

	if (!key_len)
		return WD_SUCCESS;

	if (ctxt->setup.data_fmt == WD_SGL_BUF)
		wd_sgl_cp_from_pbuf(ctxt->akey, 0, key, key_len);
	else
		memcpy(ctxt->akey, key, key_len);

	return WD_SUCCESS;

err_key_len:
	WD_ERR("failed to check authenticate key length, size = %u\n", key_len);
	return -WD_EINVAL;
}

static void aead_requests_uninit(struct wcrypto_aead_msg **req,
				struct wcrypto_aead_ctx *ctx, __u32 num)
{
	__u32 i;

	for (i = 0; i < num; i++) {
		if (req[i]->aiv)
			ctx->setup.br.free(ctx->setup.br.usr, req[i]->aiv);
	}
}

static int check_op_data(struct wcrypto_aead_op_data **op,
			 struct wcrypto_aead_ctx *ctx, __u32 idx)
{
	if (unlikely(op[idx]->op_type == WCRYPTO_CIPHER_ENCRYPTION_DIGEST &&
	    op[idx]->out_buf_bytes < op[idx]->out_bytes + ctx->auth_size)) {
		WD_ERR("fail to check out buffer length %u!\n", idx);
		return -WD_EINVAL;
	}

	if (unlikely(op[idx]->iv_bytes != ctx->iv_blk_size ||
	    op[idx]->iv_bytes == 0)) {
		WD_ERR("fail to check IV length %u!\n", idx);
		return -WD_EINVAL;
	}

	if (unlikely(op[idx]->in_bytes == 0 ||
	    (op[idx]->in_bytes & (AES_BLOCK_SIZE - 1)))) {
		if (ctx->setup.cmode == WCRYPTO_CIPHER_CBC) {
			WD_ERR("failed to check aead input data length!\n");
			return -WD_EINVAL;
		}
	}
	if (unlikely(op[idx]->state >= WCRYPTO_AEAD_MSG_INVALID)) {
		WD_ERR("fail to check message state: %u, idx: %u!\n",
		       op[idx]->state, idx);
		return -WD_EINVAL;
	} else if (idx && op[idx]->state != WCRYPTO_AEAD_MSG_BLOCK) {
		WD_ERR("fail to send multiple messages for stream mode!\n");
		return -WD_EINVAL;
	}

	return 0;
}

static void fill_stream_msg(struct wcrypto_aead_msg *req,
			    struct wcrypto_aead_op_data *op,
			    struct wcrypto_aead_ctx *ctx)
{
	req->msg_state = op->state;
	req->mac = ctx->mac;
	switch (op->state) {
	case WCRYPTO_AEAD_MSG_FIRST:
		if (req->cmode == WCRYPTO_CIPHER_GCM) {
			req->iv = ctx->civ;
			memset(ctx->civ, 0, WCRYPTO_CCM_GCM_LEN);
			memcpy(ctx->civ, op->iv, op->iv_bytes);
		}
		break;
	case WCRYPTO_AEAD_MSG_MIDDLE:
		if (req->cmode == WCRYPTO_CIPHER_GCM) {
			req->iv = ctx->civ;
			ctx->long_data_len += op->in_bytes;
			req->long_data_len = ctx->long_data_len;
		}
		break;
	case WCRYPTO_AEAD_MSG_END:
		if (req->cmode == WCRYPTO_CIPHER_GCM) {
			req->iv = ctx->civ;
			req->long_data_len = ctx->long_data_len + op->in_bytes;
			ctx->long_data_len = 0;
		}
		break;
	default:
		return;
	}
}

static int aead_requests_init(struct wcrypto_aead_msg **req,
			     struct wcrypto_aead_op_data **op,
			     struct wcrypto_aead_ctx *ctx, __u32 num)
{
	int ret;
	__u32 i;

	for (i = 0; i < num; i++) {
		ret = check_op_data(op, ctx, i);
		if (ret)
			goto err_uninit_requests;

		req[i]->calg = ctx->setup.calg;
		req[i]->cmode = ctx->setup.cmode;
		req[i]->dalg = ctx->setup.dalg;
		req[i]->dmode = ctx->setup.dmode;
		req[i]->ckey = ctx->ckey;
		req[i]->ckey_bytes = ctx->ckey_bytes;
		req[i]->akey = ctx->akey;
		req[i]->akey_bytes = ctx->akey_bytes;
		req[i]->op_type = op[i]->op_type;
		req[i]->iv = op[i]->iv;
		req[i]->iv_bytes = op[i]->iv_bytes;
		req[i]->in = op[i]->in;
		req[i]->in_bytes = op[i]->in_bytes;
		req[i]->out = op[i]->out;
		req[i]->out_bytes = op[i]->out_bytes;
		req[i]->assoc_bytes = op[i]->assoc_size;
		req[i]->auth_bytes = ctx->auth_size;

		req[i]->aiv = ctx->setup.br.alloc(ctx->setup.br.usr,
						  MAX_AEAD_KEY_SIZE);
		if (unlikely(!req[i]->aiv)) {
			WD_ERR("fail to alloc auth iv memory %u!\n", i);
			ret = -WD_ENOMEM;
			goto err_uninit_requests;
		}
	}

	fill_stream_msg(req[0], op[0], ctx);

	return WD_SUCCESS;

err_uninit_requests:
	aead_requests_uninit(req, ctx, i);
	return ret;
}

static int aead_recv_sync(struct wcrypto_aead_ctx *a_ctx,
			  struct wcrypto_aead_op_data **a_opdata, __u32 num)
{
	struct wcrypto_aead_msg *resp[WCRYPTO_MAX_BURST_NUM];
	__u32 recv_count = 0;
	__u64 rx_cnt = 0;
	__u32 i;
	int ret;

	for (i = 0; i < num; i++)
		resp[i] = (void *)(uintptr_t)a_ctx->ctx_id;

	while (true) {
		ret = wd_burst_recv(a_ctx->q, (void **)(resp + recv_count),
				    num - recv_count);
		if (ret > 0) {
			recv_count += ret;
			if (recv_count == num)
				break;

			rx_cnt = 0;
		} else if (ret == 0) {
			if (++rx_cnt > MAX_AEAD_RETRY_CNT) {
				WD_ERR("%s:wcrypto_recv timeout, num = %u, recv_count = %u!\n",
					__func__, num, recv_count);
				break;
			}
		} else {
			WD_ERR("do aead wcrypto_recv error!\n");
			return ret;
		}
	}

	for (i = 0; i < recv_count; i++) {
		a_opdata[i]->out = (void *)resp[i]->out;
		a_opdata[i]->out_bytes = resp[i]->out_bytes;
		a_opdata[i]->status = resp[i]->result;
	}

	return recv_count;
}

static int param_check(struct wcrypto_aead_ctx *a_ctx,
		       struct wcrypto_aead_op_data **a_opdata,
		       void **tag, __u32 num)
{
	__u32 i;
	int ret;

	if (unlikely(!a_ctx || !a_opdata || !num || num > WCRYPTO_MAX_BURST_NUM)) {
		WD_ERR("invalid: input param err!\n");
		return -WD_EINVAL;
	}

	for (i = 0; i < num; i++) {
		if (unlikely(!a_opdata[i])) {
			WD_ERR("invalid: aead opdata[%u] is NULL\n", i);
			return -WD_EINVAL;
		}

		ret = wd_check_src_dst_ptr(a_opdata[i]->in, a_opdata[i]->in_bytes, a_opdata[i]->out, a_opdata[i]->out_bytes);
		if (unlikely(ret)) {
			WD_ERR("invalid: src/dst addr is NULL when src/dst size is non-zero!\n");
			return -WD_EINVAL;
		}

		if (unlikely(!a_opdata[i]->iv)) {
			WD_ERR("invalid: aead input iv is NULL!\n");
			return -WD_EINVAL;
		}

		if (unlikely(tag && !tag[i])) {
			WD_ERR("invalid: tag[%u] is NULL!\n", i);
			return -WD_EINVAL;
		}
	}

	if (unlikely(tag && !a_ctx->setup.cb)) {
		WD_ERR("invalid: aead ctx call back is NULL!\n");
		return -WD_EINVAL;
	}

	return WD_SUCCESS;
}

int wcrypto_burst_aead(void *a_ctx, struct wcrypto_aead_op_data **opdata,
		       void **tag, __u32 num)
{
	struct wcrypto_aead_cookie *cookies[WCRYPTO_MAX_BURST_NUM] = { NULL };
	struct wcrypto_aead_msg *req[WCRYPTO_MAX_BURST_NUM];
	struct wcrypto_aead_ctx *ctxt = a_ctx;
	__u32 i;
	int ret;

	if (param_check(ctxt, opdata, tag, num))
		return -WD_EINVAL;

	ret = wd_get_cookies(&ctxt->pool, (void **)cookies, num);
	if (unlikely(ret))
		return ret;

	for (i = 0; i < num; i++) {
		cookies[i]->tag.priv = opdata[i]->priv;
		req[i] = &cookies[i]->msg;
		if (tag)
			cookies[i]->tag.wcrypto_tag.tag = tag[i];
	}

	ret = aead_requests_init(req, opdata, ctxt, num);
	if (unlikely(ret))
		goto fail_with_cookies;

	ret = wd_burst_send(ctxt->q, (void **)req, num);
	if (unlikely(ret)) {
		WD_ERR("failed to send req %d!\n", ret);
		goto fail_with_send;
	}

	if (tag)
		return ret;

	ret = aead_recv_sync(ctxt, opdata, num);

fail_with_send:
	aead_requests_uninit(req, ctxt, num);
fail_with_cookies:
	wd_put_cookies(&ctxt->pool, (void **)cookies, num);
	return ret;
}

int wcrypto_do_aead(void *ctx, struct wcrypto_aead_op_data *opdata, void *tag)
{
	int ret;

	if (!tag) {
		ret = wcrypto_burst_aead(ctx, &opdata, NULL, 1);
		if (likely(ret == 1))
			return GET_NEGATIVE(opdata->status);
		if (unlikely(ret == 0))
			return -WD_ETIMEDOUT;
	} else {
		ret = wcrypto_burst_aead(ctx, &opdata, &tag, 1);
	}

	return ret;
}

int wcrypto_aead_poll(struct wd_queue *q, unsigned int num)
{
	struct wcrypto_aead_msg *aead_resp = NULL;
	struct wcrypto_aead_ctx *ctx;
	struct wcrypto_aead_tag *tag;
	unsigned int tmp = num;
	int count = 0;
	int ret;

	if (unlikely(!q)) {
		WD_ERR("queue is NULL!\n");
		return -WD_EINVAL;
	}

	do {
		aead_resp = NULL;
		ret = wd_recv(q, (void **)&aead_resp);
		if (ret == 0)
			break;
		else if (ret == -WD_HW_EACCESS) {
			if (!aead_resp) {
				WD_ERR("the aead recv err from req_cache!\n");
				return ret;
			}
			aead_resp->result = WD_HW_EACCESS;
		} else if (ret < 0) {
			WD_ERR("recv err at aead poll!\n");
			return ret;
		}
		count++;
		tag = (void *)(uintptr_t)aead_resp->usr_data;
		ctx = tag->wcrypto_tag.ctx;
		ctx->setup.cb(aead_resp, tag->wcrypto_tag.tag);
		aead_requests_uninit(&aead_resp, ctx, 1);
		wd_put_cookies(&ctx->pool, (void **)&tag, 1);
	} while (--tmp);

	return count;
}

void wcrypto_del_aead_ctx(void *ctx)
{
	struct wcrypto_aead_ctx *ctxt;
	struct q_info *qinfo;

	if (!ctx) {
		WD_ERR("Delete aead ctx is NULL!\n");
		return;
	}
	ctxt = ctx;
	qinfo = ctxt->q->qinfo;
	wd_uninit_cookie_pool(&ctxt->pool);
	wd_spinlock(&qinfo->qlock);
	if (qinfo->ctx_num <= 0) {
		wd_unspinlock(&qinfo->qlock);
		WD_ERR("fail to delete aead ctx!\n");
		return;
	}
	wd_free_id(qinfo->ctx_id, WD_MAX_CTX_NUM, ctxt->ctx_id - 1,
		WD_MAX_CTX_NUM);
	if (!(--qinfo->ctx_num))
		memset(&qinfo->br, 0, sizeof(qinfo->br));
	wd_unspinlock(&qinfo->qlock);
	del_ctx_key(ctxt);
	free(ctx);
}
