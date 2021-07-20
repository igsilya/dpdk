/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2021 NVIDIA Corporation & Affiliates
 */

#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_errno.h>
#include <rte_log.h>
#include <rte_pci.h>
#include <rte_memory.h>

#include <mlx5_glue.h>
#include <mlx5_common.h>
#include <mlx5_common_pci.h>
#include <mlx5_devx_cmds.h>
#include <mlx5_common_os.h>

#include "mlx5_crypto_utils.h"
#include "mlx5_crypto.h"

#define MLX5_CRYPTO_DRIVER_NAME crypto_mlx5
#define MLX5_CRYPTO_LOG_NAME pmd.crypto.mlx5
#define MLX5_CRYPTO_MAX_QPS 1024
#define MLX5_CRYPTO_MAX_SEGS 56

#define MLX5_CRYPTO_FEATURE_FLAGS \
	(RTE_CRYPTODEV_FF_SYMMETRIC_CRYPTO | RTE_CRYPTODEV_FF_HW_ACCELERATED | \
	 RTE_CRYPTODEV_FF_CIPHER_WRAPPED_KEY | \
	 RTE_CRYPTODEV_FF_CIPHER_MULTIPLE_DATA_UNITS)

TAILQ_HEAD(mlx5_crypto_privs, mlx5_crypto_priv) mlx5_crypto_priv_list =
				TAILQ_HEAD_INITIALIZER(mlx5_crypto_priv_list);
static pthread_mutex_t priv_list_lock = PTHREAD_MUTEX_INITIALIZER;

int mlx5_crypto_logtype;

uint8_t mlx5_crypto_driver_id;

const struct rte_cryptodev_capabilities mlx5_crypto_caps[] = {
	{		/* AES XTS */
		.op = RTE_CRYPTO_OP_TYPE_SYMMETRIC,
		{.sym = {
			.xform_type = RTE_CRYPTO_SYM_XFORM_CIPHER,
			{.cipher = {
				.algo = RTE_CRYPTO_CIPHER_AES_XTS,
				.block_size = 16,
				.key_size = {
					.min = 32,
					.max = 64,
					.increment = 32
				},
				.iv_size = {
					.min = 16,
					.max = 16,
					.increment = 0
				},
				.dataunit_set =
				RTE_CRYPTO_CIPHER_DATA_UNIT_LEN_512_BYTES |
				RTE_CRYPTO_CIPHER_DATA_UNIT_LEN_4096_BYTES,
			}, }
		}, }
	},
};

static const char mlx5_crypto_drv_name[] = RTE_STR(MLX5_CRYPTO_DRIVER_NAME);

static const struct rte_driver mlx5_drv = {
	.name = mlx5_crypto_drv_name,
	.alias = mlx5_crypto_drv_name
};

static struct cryptodev_driver mlx5_cryptodev_driver;

struct mlx5_crypto_session {
	uint32_t bs_bpt_eo_es;
	/**< bsf_size, bsf_p_type, encryption_order and encryption standard,
	 * saved in big endian format.
	 */
	uint32_t bsp_res;
	/**< crypto_block_size_pointer and reserved 24 bits saved in big
	 * endian format.
	 */
	uint32_t iv_offset:16;
	/**< Starting point for Initialisation Vector. */
	struct mlx5_crypto_dek *dek; /**< Pointer to dek struct. */
	uint32_t dek_id; /**< DEK ID */
} __rte_packed;

static void
mlx5_crypto_dev_infos_get(struct rte_cryptodev *dev,
			  struct rte_cryptodev_info *dev_info)
{
	RTE_SET_USED(dev);
	if (dev_info != NULL) {
		dev_info->driver_id = mlx5_crypto_driver_id;
		dev_info->feature_flags = MLX5_CRYPTO_FEATURE_FLAGS;
		dev_info->capabilities = mlx5_crypto_caps;
		dev_info->max_nb_queue_pairs = MLX5_CRYPTO_MAX_QPS;
		dev_info->min_mbuf_headroom_req = 0;
		dev_info->min_mbuf_tailroom_req = 0;
		dev_info->sym.max_nb_sessions = 0;
		/*
		 * If 0, the device does not have any limitation in number of
		 * sessions that can be used.
		 */
	}
}

static int
mlx5_crypto_dev_configure(struct rte_cryptodev *dev,
			  struct rte_cryptodev_config *config)
{
	struct mlx5_crypto_priv *priv = dev->data->dev_private;

	if (config == NULL) {
		DRV_LOG(ERR, "Invalid crypto dev configure parameters.");
		return -EINVAL;
	}
	if ((config->ff_disable & RTE_CRYPTODEV_FF_SYMMETRIC_CRYPTO) != 0) {
		DRV_LOG(ERR,
			"Disabled symmetric crypto feature is not supported.");
		return -ENOTSUP;
	}
	if (mlx5_crypto_dek_setup(priv) != 0) {
		DRV_LOG(ERR, "Dek hash list creation has failed.");
		return -ENOMEM;
	}
	priv->dev_config = *config;
	DRV_LOG(DEBUG, "Device %u was configured.", dev->driver_id);
	return 0;
}

static void
mlx5_crypto_dev_stop(struct rte_cryptodev *dev)
{
	RTE_SET_USED(dev);
}

static int
mlx5_crypto_dev_start(struct rte_cryptodev *dev)
{
	RTE_SET_USED(dev);
	return 0;
}

static int
mlx5_crypto_dev_close(struct rte_cryptodev *dev)
{
	struct mlx5_crypto_priv *priv = dev->data->dev_private;

	mlx5_crypto_dek_unset(priv);
	DRV_LOG(DEBUG, "Device %u was closed.", dev->driver_id);
	return 0;
}

static unsigned int
mlx5_crypto_sym_session_get_size(struct rte_cryptodev *dev __rte_unused)
{
	return sizeof(struct mlx5_crypto_session);
}

static int
mlx5_crypto_sym_session_configure(struct rte_cryptodev *dev,
				  struct rte_crypto_sym_xform *xform,
				  struct rte_cryptodev_sym_session *session,
				  struct rte_mempool *mp)
{
	struct mlx5_crypto_priv *priv = dev->data->dev_private;
	struct mlx5_crypto_session *sess_private_data;
	struct rte_crypto_cipher_xform *cipher;
	uint8_t encryption_order;
	int ret;

	if (unlikely(xform->next != NULL)) {
		DRV_LOG(ERR, "Xform next is not supported.");
		return -ENOTSUP;
	}
	if (unlikely((xform->type != RTE_CRYPTO_SYM_XFORM_CIPHER) ||
		     (xform->cipher.algo != RTE_CRYPTO_CIPHER_AES_XTS))) {
		DRV_LOG(ERR, "Only AES-XTS algorithm is supported.");
		return -ENOTSUP;
	}
	ret = rte_mempool_get(mp, (void *)&sess_private_data);
	if (ret != 0) {
		DRV_LOG(ERR,
			"Failed to get session %p private data from mempool.",
			sess_private_data);
		return -ENOMEM;
	}
	cipher = &xform->cipher;
	sess_private_data->dek = mlx5_crypto_dek_prepare(priv, cipher);
	if (sess_private_data->dek == NULL) {
		rte_mempool_put(mp, sess_private_data);
		DRV_LOG(ERR, "Failed to prepare dek.");
		return -ENOMEM;
	}
	if (cipher->op == RTE_CRYPTO_CIPHER_OP_ENCRYPT)
		encryption_order = MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_MEMORY;
	else
		encryption_order = MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_WIRE;
	sess_private_data->bs_bpt_eo_es = rte_cpu_to_be_32
			(MLX5_BSF_SIZE_64B << MLX5_BSF_SIZE_OFFSET |
			 MLX5_BSF_P_TYPE_CRYPTO << MLX5_BSF_P_TYPE_OFFSET |
			 encryption_order << MLX5_ENCRYPTION_ORDER_OFFSET |
			 MLX5_ENCRYPTION_STANDARD_AES_XTS);
	switch (xform->cipher.dataunit_len) {
	case 0:
		sess_private_data->bsp_res = 0;
		break;
	case 512:
		sess_private_data->bsp_res = rte_cpu_to_be_32
					     ((uint32_t)MLX5_BLOCK_SIZE_512B <<
					     MLX5_BLOCK_SIZE_OFFSET);
		break;
	case 4096:
		sess_private_data->bsp_res = rte_cpu_to_be_32
					     ((uint32_t)MLX5_BLOCK_SIZE_4096B <<
					     MLX5_BLOCK_SIZE_OFFSET);
		break;
	default:
		DRV_LOG(ERR, "Cipher data unit length is not supported.");
		return -ENOTSUP;
	}
	sess_private_data->iv_offset = cipher->iv.offset;
	sess_private_data->dek_id =
			rte_cpu_to_be_32(sess_private_data->dek->obj->id &
					 0xffffff);
	set_sym_session_private_data(session, dev->driver_id,
				     sess_private_data);
	DRV_LOG(DEBUG, "Session %p was configured.", sess_private_data);
	return 0;
}

static void
mlx5_crypto_sym_session_clear(struct rte_cryptodev *dev,
			      struct rte_cryptodev_sym_session *sess)
{
	struct mlx5_crypto_priv *priv = dev->data->dev_private;
	struct mlx5_crypto_session *spriv = get_sym_session_private_data(sess,
								dev->driver_id);

	if (unlikely(spriv == NULL)) {
		DRV_LOG(ERR, "Failed to get session %p private data.", spriv);
		return;
	}
	mlx5_crypto_dek_destroy(priv, spriv->dek);
	set_sym_session_private_data(sess, dev->driver_id, NULL);
	rte_mempool_put(rte_mempool_from_obj(spriv), spriv);
	DRV_LOG(DEBUG, "Session %p was cleared.", spriv);
}

static int
mlx5_crypto_queue_pair_release(struct rte_cryptodev *dev, uint16_t qp_id)
{
	struct mlx5_crypto_qp *qp = dev->data->queue_pairs[qp_id];

	if (qp->qp_obj != NULL)
		claim_zero(mlx5_devx_cmd_destroy(qp->qp_obj));
	if (qp->umem_obj != NULL)
		claim_zero(mlx5_glue->devx_umem_dereg(qp->umem_obj));
	if (qp->umem_buf != NULL)
		rte_free(qp->umem_buf);
	mlx5_mr_btree_free(&qp->mr_ctrl.cache_bh);
	mlx5_devx_cq_destroy(&qp->cq_obj);
	rte_free(qp);
	dev->data->queue_pairs[qp_id] = NULL;
	return 0;
}

static int
mlx5_crypto_qp2rts(struct mlx5_crypto_qp *qp)
{
	/*
	 * In Order to configure self loopback, when calling these functions the
	 * remote QP id that is used is the id of the same QP.
	 */
	if (mlx5_devx_cmd_modify_qp_state(qp->qp_obj, MLX5_CMD_OP_RST2INIT_QP,
					  qp->qp_obj->id)) {
		DRV_LOG(ERR, "Failed to modify QP to INIT state(%u).",
			rte_errno);
		return -1;
	}
	if (mlx5_devx_cmd_modify_qp_state(qp->qp_obj, MLX5_CMD_OP_INIT2RTR_QP,
					  qp->qp_obj->id)) {
		DRV_LOG(ERR, "Failed to modify QP to RTR state(%u).",
			rte_errno);
		return -1;
	}
	if (mlx5_devx_cmd_modify_qp_state(qp->qp_obj, MLX5_CMD_OP_RTR2RTS_QP,
					  qp->qp_obj->id)) {
		DRV_LOG(ERR, "Failed to modify QP to RTS state(%u).",
			rte_errno);
		return -1;
	}
	return 0;
}

static void
mlx5_crypto_qp_init(struct mlx5_crypto_priv *priv, struct mlx5_crypto_qp *qp)
{
	uint32_t i;

	for (i = 0 ; i < qp->entries_n; i++) {
		struct mlx5_wqe_cseg *cseg = RTE_PTR_ADD(qp->umem_buf, i *
							 priv->wqe_set_size);
		struct mlx5_wqe_umr_cseg *ucseg = (struct mlx5_wqe_umr_cseg *)
								     (cseg + 1);
		struct mlx5_wqe_umr_bsf_seg *bsf =
			(struct mlx5_wqe_umr_bsf_seg *)(RTE_PTR_ADD(cseg,
						       priv->umr_wqe_size)) - 1;
		struct mlx5_wqe_rseg *rseg;

		/* Init UMR WQE. */
		cseg->sq_ds = rte_cpu_to_be_32((qp->qp_obj->id << 8) |
					 (priv->umr_wqe_size / MLX5_WSEG_SIZE));
		cseg->flags = RTE_BE32(MLX5_COMP_ONLY_FIRST_ERR <<
				       MLX5_COMP_MODE_OFFSET);
		cseg->misc = rte_cpu_to_be_32(qp->mkey[i]->id);
		ucseg->if_cf_toe_cq_res = RTE_BE32(1u << MLX5_UMRC_IF_OFFSET);
		ucseg->mkey_mask = RTE_BE64(1u << 0); /* Mkey length bit. */
		ucseg->ko_to_bs = rte_cpu_to_be_32
			((RTE_ALIGN(priv->max_segs_num, 4u) <<
			 MLX5_UMRC_KO_OFFSET) | (4 << MLX5_UMRC_TO_BS_OFFSET));
		bsf->keytag = priv->keytag;
		/* Init RDMA WRITE WQE. */
		cseg = RTE_PTR_ADD(cseg, priv->umr_wqe_size);
		cseg->flags = RTE_BE32((MLX5_COMP_ALWAYS <<
				      MLX5_COMP_MODE_OFFSET) |
				      MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE);
		rseg = (struct mlx5_wqe_rseg *)(cseg + 1);
		rseg->rkey = rte_cpu_to_be_32(qp->mkey[i]->id);
	}
}

static int
mlx5_crypto_indirect_mkeys_prepare(struct mlx5_crypto_priv *priv,
				  struct mlx5_crypto_qp *qp)
{
	struct mlx5_umr_wqe *umr;
	uint32_t i;
	struct mlx5_devx_mkey_attr attr = {
		.pd = priv->pdn,
		.umr_en = 1,
		.crypto_en = 1,
		.set_remote_rw = 1,
		.klm_num = RTE_ALIGN(priv->max_segs_num, 4),
	};

	for (umr = (struct mlx5_umr_wqe *)qp->umem_buf, i = 0;
	   i < qp->entries_n; i++, umr = RTE_PTR_ADD(umr, priv->wqe_set_size)) {
		attr.klm_array = (struct mlx5_klm *)&umr->kseg[0];
		qp->mkey[i] = mlx5_devx_cmd_mkey_create(priv->ctx, &attr);
		if (!qp->mkey[i]) {
			DRV_LOG(ERR, "Failed to allocate indirect mkey.");
			return -1;
		}
	}
	return 0;
}

static int
mlx5_crypto_queue_pair_setup(struct rte_cryptodev *dev, uint16_t qp_id,
			     const struct rte_cryptodev_qp_conf *qp_conf,
			     int socket_id)
{
	struct mlx5_crypto_priv *priv = dev->data->dev_private;
	struct mlx5_devx_qp_attr attr = {0};
	struct mlx5_crypto_qp *qp;
	uint16_t log_nb_desc = rte_log2_u32(qp_conf->nb_descriptors);
	uint32_t umem_size = RTE_BIT32(log_nb_desc) *
			      priv->wqe_set_size +
			      sizeof(*qp->db_rec) * 2;
	uint32_t alloc_size = sizeof(*qp);
	struct mlx5_devx_cq_attr cq_attr = {
		.uar_page_id = mlx5_os_get_devx_uar_page_id(priv->uar),
	};

	if (dev->data->queue_pairs[qp_id] != NULL)
		mlx5_crypto_queue_pair_release(dev, qp_id);
	alloc_size = RTE_ALIGN(alloc_size, RTE_CACHE_LINE_SIZE);
	alloc_size += (sizeof(struct rte_crypto_op *) +
		       sizeof(struct mlx5_devx_obj *)) *
		       RTE_BIT32(log_nb_desc);
	qp = rte_zmalloc_socket(__func__, alloc_size, RTE_CACHE_LINE_SIZE,
				socket_id);
	if (qp == NULL) {
		DRV_LOG(ERR, "Failed to allocate QP memory.");
		rte_errno = ENOMEM;
		return -rte_errno;
	}
	if (mlx5_devx_cq_create(priv->ctx, &qp->cq_obj, log_nb_desc,
				&cq_attr, socket_id) != 0) {
		DRV_LOG(ERR, "Failed to create CQ.");
		goto error;
	}
	qp->umem_buf = rte_zmalloc_socket(__func__, umem_size, 4096, socket_id);
	if (qp->umem_buf == NULL) {
		DRV_LOG(ERR, "Failed to allocate QP umem.");
		rte_errno = ENOMEM;
		goto error;
	}
	qp->umem_obj = mlx5_glue->devx_umem_reg(priv->ctx,
					       (void *)(uintptr_t)qp->umem_buf,
					       umem_size,
					       IBV_ACCESS_LOCAL_WRITE);
	if (qp->umem_obj == NULL) {
		DRV_LOG(ERR, "Failed to register QP umem.");
		goto error;
	}
	if (mlx5_mr_btree_init(&qp->mr_ctrl.cache_bh, MLX5_MR_BTREE_CACHE_N,
			       priv->dev_config.socket_id) != 0) {
		DRV_LOG(ERR, "Cannot allocate MR Btree for qp %u.",
			(uint32_t)qp_id);
		rte_errno = ENOMEM;
		goto error;
	}
	qp->mr_ctrl.dev_gen_ptr = &priv->mr_scache.dev_gen;
	attr.pd = priv->pdn;
	attr.uar_index = mlx5_os_get_devx_uar_page_id(priv->uar);
	attr.cqn = qp->cq_obj.cq->id;
	attr.log_page_size = rte_log2_u32(sysconf(_SC_PAGESIZE));
	attr.rq_size =  0;
	attr.sq_size = RTE_BIT32(log_nb_desc);
	attr.dbr_umem_valid = 1;
	attr.wq_umem_id = qp->umem_obj->umem_id;
	attr.wq_umem_offset = 0;
	attr.dbr_umem_id = qp->umem_obj->umem_id;
	attr.dbr_address = RTE_BIT64(log_nb_desc) * priv->wqe_set_size;
	qp->qp_obj = mlx5_devx_cmd_create_qp(priv->ctx, &attr);
	if (qp->qp_obj == NULL) {
		DRV_LOG(ERR, "Failed to create QP(%u).", rte_errno);
		goto error;
	}
	qp->db_rec = RTE_PTR_ADD(qp->umem_buf, (uintptr_t)attr.dbr_address);
	if (mlx5_crypto_qp2rts(qp))
		goto error;
	qp->mkey = (struct mlx5_devx_obj **)RTE_ALIGN((uintptr_t)(qp + 1),
							   RTE_CACHE_LINE_SIZE);
	qp->ops = (struct rte_crypto_op **)(qp->mkey + RTE_BIT32(log_nb_desc));
	qp->entries_n = 1 << log_nb_desc;
	if (mlx5_crypto_indirect_mkeys_prepare(priv, qp)) {
		DRV_LOG(ERR, "Cannot allocate indirect memory regions.");
		rte_errno = ENOMEM;
		goto error;
	}
	mlx5_crypto_qp_init(priv, qp);
	qp->priv = priv;
	dev->data->queue_pairs[qp_id] = qp;
	return 0;
error:
	mlx5_crypto_queue_pair_release(dev, qp_id);
	return -1;
}

static struct rte_cryptodev_ops mlx5_crypto_ops = {
	.dev_configure			= mlx5_crypto_dev_configure,
	.dev_start			= mlx5_crypto_dev_start,
	.dev_stop			= mlx5_crypto_dev_stop,
	.dev_close			= mlx5_crypto_dev_close,
	.dev_infos_get			= mlx5_crypto_dev_infos_get,
	.stats_get			= NULL,
	.stats_reset			= NULL,
	.queue_pair_setup		= mlx5_crypto_queue_pair_setup,
	.queue_pair_release		= mlx5_crypto_queue_pair_release,
	.sym_session_get_size		= mlx5_crypto_sym_session_get_size,
	.sym_session_configure		= mlx5_crypto_sym_session_configure,
	.sym_session_clear		= mlx5_crypto_sym_session_clear,
	.sym_get_raw_dp_ctx_size	= NULL,
	.sym_configure_raw_dp_ctx	= NULL,
};

static void
mlx5_crypto_hw_global_release(struct mlx5_crypto_priv *priv)
{
	if (priv->pd != NULL) {
		claim_zero(mlx5_glue->dealloc_pd(priv->pd));
		priv->pd = NULL;
	}
	if (priv->uar != NULL) {
		mlx5_glue->devx_free_uar(priv->uar);
		priv->uar = NULL;
	}
}

static int
mlx5_crypto_pd_create(struct mlx5_crypto_priv *priv)
{
#ifdef HAVE_IBV_FLOW_DV_SUPPORT
	struct mlx5dv_obj obj;
	struct mlx5dv_pd pd_info;
	int ret;

	priv->pd = mlx5_glue->alloc_pd(priv->ctx);
	if (priv->pd == NULL) {
		DRV_LOG(ERR, "Failed to allocate PD.");
		return errno ? -errno : -ENOMEM;
	}
	obj.pd.in = priv->pd;
	obj.pd.out = &pd_info;
	ret = mlx5_glue->dv_init_obj(&obj, MLX5DV_OBJ_PD);
	if (ret != 0) {
		DRV_LOG(ERR, "Fail to get PD object info.");
		mlx5_glue->dealloc_pd(priv->pd);
		priv->pd = NULL;
		return -errno;
	}
	priv->pdn = pd_info.pdn;
	return 0;
#else
	(void)priv;
	DRV_LOG(ERR, "Cannot get pdn - no DV support.");
	return -ENOTSUP;
#endif /* HAVE_IBV_FLOW_DV_SUPPORT */
}

static int
mlx5_crypto_hw_global_prepare(struct mlx5_crypto_priv *priv)
{
	if (mlx5_crypto_pd_create(priv) != 0)
		return -1;
	priv->uar = mlx5_devx_alloc_uar(priv->ctx, -1);
	if (priv->uar == NULL || mlx5_os_get_devx_uar_reg_addr(priv->uar) ==
	    NULL) {
		rte_errno = errno;
		claim_zero(mlx5_glue->dealloc_pd(priv->pd));
		DRV_LOG(ERR, "Failed to allocate UAR.");
		return -1;
	}
	return 0;
}


static int
mlx5_crypto_args_check_handler(const char *key, const char *val, void *opaque)
{
	struct mlx5_crypto_devarg_params *devarg_prms = opaque;
	struct mlx5_devx_crypto_login_attr *attr = &devarg_prms->login_attr;
	unsigned long tmp;
	FILE *file;
	int ret;
	int i;

	if (strcmp(key, "class") == 0)
		return 0;
	if (strcmp(key, "wcs_file") == 0) {
		file = fopen(val, "rb");
		if (file == NULL) {
			rte_errno = ENOTSUP;
			return -rte_errno;
		}
		for (i = 0 ; i < MLX5_CRYPTO_CREDENTIAL_SIZE ; i++) {
			ret = fscanf(file, "%02hhX", &attr->credential[i]);
			if (ret <= 0) {
				fclose(file);
				DRV_LOG(ERR,
					"Failed to read credential from file.");
				rte_errno = EINVAL;
				return -rte_errno;
			}
		}
		fclose(file);
		devarg_prms->login_devarg = true;
		return 0;
	}
	errno = 0;
	tmp = strtoul(val, NULL, 0);
	if (errno) {
		DRV_LOG(WARNING, "%s: \"%s\" is an invalid integer.", key, val);
		return -errno;
	}
	if (strcmp(key, "max_segs_num") == 0) {
		if (!tmp || tmp > MLX5_CRYPTO_MAX_SEGS) {
			DRV_LOG(WARNING, "Invalid max_segs_num: %d, should"
				" be less than %d.",
				(uint32_t)tmp, MLX5_CRYPTO_MAX_SEGS);
			rte_errno = EINVAL;
			return -rte_errno;
		}
		devarg_prms->max_segs_num = (uint32_t)tmp;
	} else if (strcmp(key, "import_kek_id") == 0) {
		attr->session_import_kek_ptr = (uint32_t)tmp;
	} else if (strcmp(key, "credential_id") == 0) {
		attr->credential_pointer = (uint32_t)tmp;
	} else if (strcmp(key, "keytag") == 0) {
		devarg_prms->keytag = tmp;
	} else {
		DRV_LOG(WARNING, "Invalid key %s.", key);
	}
	return 0;
}

static int
mlx5_crypto_parse_devargs(struct rte_devargs *devargs,
			  struct mlx5_crypto_devarg_params *devarg_prms)
{
	struct mlx5_devx_crypto_login_attr *attr = &devarg_prms->login_attr;
	struct rte_kvargs *kvlist;

	/* Default values. */
	attr->credential_pointer = 0;
	attr->session_import_kek_ptr = 0;
	devarg_prms->keytag = 0;
	devarg_prms->max_segs_num = 8;
	if (devargs == NULL) {
		DRV_LOG(ERR,
	"No login devargs in order to enable crypto operations in the device.");
		rte_errno = EINVAL;
		return -1;
	}
	kvlist = rte_kvargs_parse(devargs->args, NULL);
	if (kvlist == NULL) {
		DRV_LOG(ERR, "Failed to parse devargs.");
		rte_errno = EINVAL;
		return -1;
	}
	if (rte_kvargs_process(kvlist, NULL, mlx5_crypto_args_check_handler,
			   devarg_prms) != 0) {
		DRV_LOG(ERR, "Devargs handler function Failed.");
		rte_kvargs_free(kvlist);
		rte_errno = EINVAL;
		return -1;
	}
	rte_kvargs_free(kvlist);
	if (devarg_prms->login_devarg == false) {
		DRV_LOG(ERR,
	"No login credential devarg in order to enable crypto operations "
	"in the device.");
		rte_errno = EINVAL;
		return -1;
	}
	return 0;
}

/**
 * Callback for memory event.
 *
 * @param event_type
 *   Memory event type.
 * @param addr
 *   Address of memory.
 * @param len
 *   Size of memory.
 */
static void
mlx5_crypto_mr_mem_event_cb(enum rte_mem_event event_type, const void *addr,
			    size_t len, void *arg __rte_unused)
{
	struct mlx5_crypto_priv *priv;

	/* Must be called from the primary process. */
	MLX5_ASSERT(rte_eal_process_type() == RTE_PROC_PRIMARY);
	switch (event_type) {
	case RTE_MEM_EVENT_FREE:
		pthread_mutex_lock(&priv_list_lock);
		/* Iterate all the existing mlx5 devices. */
		TAILQ_FOREACH(priv, &mlx5_crypto_priv_list, next)
			mlx5_free_mr_by_addr(&priv->mr_scache,
					     priv->ctx->device->name,
					     addr, len);
		pthread_mutex_unlock(&priv_list_lock);
		break;
	case RTE_MEM_EVENT_ALLOC:
	default:
		break;
	}
}

/**
 * DPDK callback to register a PCI device.
 *
 * This function spawns crypto device out of a given PCI device.
 *
 * @param[in] pci_drv
 *   PCI driver structure (mlx5_crypto_driver).
 * @param[in] pci_dev
 *   PCI device information.
 *
 * @return
 *   0 on success, 1 to skip this driver, a negative errno value otherwise
 *   and rte_errno is set.
 */
static int
mlx5_crypto_pci_probe(struct rte_pci_driver *pci_drv,
			struct rte_pci_device *pci_dev)
{
	struct ibv_device *ibv;
	struct rte_cryptodev *crypto_dev;
	struct ibv_context *ctx;
	struct mlx5_devx_obj *login;
	struct mlx5_crypto_priv *priv;
	struct mlx5_crypto_devarg_params devarg_prms = { 0 };
	struct mlx5_hca_attr attr = { 0 };
	struct rte_cryptodev_pmd_init_params init_params = {
		.name = "",
		.private_data_size = sizeof(struct mlx5_crypto_priv),
		.socket_id = pci_dev->device.numa_node,
		.max_nb_queue_pairs =
				RTE_CRYPTODEV_PMD_DEFAULT_MAX_NB_QUEUE_PAIRS,
	};
	uint16_t rdmw_wqe_size;
	int ret;

	RTE_SET_USED(pci_drv);
	if (rte_eal_process_type() != RTE_PROC_PRIMARY) {
		DRV_LOG(ERR, "Non-primary process type is not supported.");
		rte_errno = ENOTSUP;
		return -rte_errno;
	}
	ibv = mlx5_os_get_ibv_device(&pci_dev->addr);
	if (ibv == NULL) {
		DRV_LOG(ERR, "No matching IB device for PCI slot "
			PCI_PRI_FMT ".", pci_dev->addr.domain,
			pci_dev->addr.bus, pci_dev->addr.devid,
			pci_dev->addr.function);
		return -rte_errno;
	}
	DRV_LOG(INFO, "PCI information matches for device \"%s\".", ibv->name);
	ctx = mlx5_glue->dv_open_device(ibv);
	if (ctx == NULL) {
		DRV_LOG(ERR, "Failed to open IB device \"%s\".", ibv->name);
		rte_errno = ENODEV;
		return -rte_errno;
	}
	if (mlx5_devx_cmd_query_hca_attr(ctx, &attr) != 0 ||
	    attr.crypto == 0 || attr.aes_xts == 0) {
		DRV_LOG(ERR, "Not enough capabilities to support crypto "
			"operations, maybe old FW/OFED version?");
		claim_zero(mlx5_glue->close_device(ctx));
		rte_errno = ENOTSUP;
		return -ENOTSUP;
	}
	ret = mlx5_crypto_parse_devargs(pci_dev->device.devargs, &devarg_prms);
	if (ret) {
		DRV_LOG(ERR, "Failed to parse devargs.");
		return -rte_errno;
	}
	login = mlx5_devx_cmd_create_crypto_login_obj(ctx,
						      &devarg_prms.login_attr);
	if (login == NULL) {
		DRV_LOG(ERR, "Failed to configure login.");
		return -rte_errno;
	}
	crypto_dev = rte_cryptodev_pmd_create(ibv->name, &pci_dev->device,
					&init_params);
	if (crypto_dev == NULL) {
		DRV_LOG(ERR, "Failed to create device \"%s\".", ibv->name);
		claim_zero(mlx5_glue->close_device(ctx));
		return -ENODEV;
	}
	DRV_LOG(INFO,
		"Crypto device %s was created successfully.", ibv->name);
	crypto_dev->dev_ops = &mlx5_crypto_ops;
	crypto_dev->dequeue_burst = NULL;
	crypto_dev->enqueue_burst = NULL;
	crypto_dev->feature_flags = MLX5_CRYPTO_FEATURE_FLAGS;
	crypto_dev->driver_id = mlx5_crypto_driver_id;
	priv = crypto_dev->data->dev_private;
	priv->ctx = ctx;
	priv->login_obj = login;
	priv->pci_dev = pci_dev;
	priv->crypto_dev = crypto_dev;
	if (mlx5_crypto_hw_global_prepare(priv) != 0) {
		rte_cryptodev_pmd_destroy(priv->crypto_dev);
		claim_zero(mlx5_glue->close_device(priv->ctx));
		return -1;
	}
	if (mlx5_mr_btree_init(&priv->mr_scache.cache,
			     MLX5_MR_BTREE_CACHE_N * 2, rte_socket_id()) != 0) {
		DRV_LOG(ERR, "Failed to allocate shared cache MR memory.");
		mlx5_crypto_hw_global_release(priv);
		rte_cryptodev_pmd_destroy(priv->crypto_dev);
		claim_zero(mlx5_glue->close_device(priv->ctx));
		rte_errno = ENOMEM;
		return -rte_errno;
	}
	priv->mr_scache.reg_mr_cb = mlx5_common_verbs_reg_mr;
	priv->mr_scache.dereg_mr_cb = mlx5_common_verbs_dereg_mr;
	priv->keytag = rte_cpu_to_be_64(devarg_prms.keytag);
	priv->max_segs_num = devarg_prms.max_segs_num;
	priv->umr_wqe_size = sizeof(struct mlx5_wqe_umr_bsf_seg) +
			     sizeof(struct mlx5_umr_wqe) +
			     RTE_ALIGN(priv->max_segs_num, 4) *
			     sizeof(struct mlx5_wqe_dseg);
	rdmw_wqe_size = sizeof(struct mlx5_rdma_write_wqe) +
			      sizeof(struct mlx5_wqe_dseg) *
			      (priv->max_segs_num <= 2 ? 2 : 2 +
			       RTE_ALIGN(priv->max_segs_num - 2, 4));
	priv->wqe_set_size = priv->umr_wqe_size + rdmw_wqe_size;
	priv->umr_wqe_stride = priv->umr_wqe_size / MLX5_SEND_WQE_BB;
	priv->max_rdmar_ds = rdmw_wqe_size / sizeof(struct mlx5_wqe_dseg);
	/* Register callback function for global shared MR cache management. */
	if (TAILQ_EMPTY(&mlx5_crypto_priv_list))
		rte_mem_event_callback_register("MLX5_MEM_EVENT_CB",
						mlx5_crypto_mr_mem_event_cb,
						NULL);
	pthread_mutex_lock(&priv_list_lock);
	TAILQ_INSERT_TAIL(&mlx5_crypto_priv_list, priv, next);
	pthread_mutex_unlock(&priv_list_lock);
	return 0;
}

static int
mlx5_crypto_pci_remove(struct rte_pci_device *pdev)
{
	struct mlx5_crypto_priv *priv = NULL;

	pthread_mutex_lock(&priv_list_lock);
	TAILQ_FOREACH(priv, &mlx5_crypto_priv_list, next)
		if (rte_pci_addr_cmp(&priv->pci_dev->addr, &pdev->addr) != 0)
			break;
	if (priv)
		TAILQ_REMOVE(&mlx5_crypto_priv_list, priv, next);
	pthread_mutex_unlock(&priv_list_lock);
	if (priv) {
		if (TAILQ_EMPTY(&mlx5_crypto_priv_list))
			rte_mem_event_callback_unregister("MLX5_MEM_EVENT_CB",
							  NULL);
		mlx5_mr_release_cache(&priv->mr_scache);
		mlx5_crypto_hw_global_release(priv);
		rte_cryptodev_pmd_destroy(priv->crypto_dev);
		claim_zero(mlx5_devx_cmd_destroy(priv->login_obj));
		claim_zero(mlx5_glue->close_device(priv->ctx));
	}
	return 0;
}

static const struct rte_pci_id mlx5_crypto_pci_id_map[] = {
		{
			RTE_PCI_DEVICE(PCI_VENDOR_ID_MELLANOX,
					PCI_DEVICE_ID_MELLANOX_CONNECTX6)
		},
		{
			.vendor_id = 0
		}
};

static struct mlx5_pci_driver mlx5_crypto_driver = {
	.driver_class = MLX5_CLASS_CRYPTO,
	.pci_driver = {
		.driver = {
			.name = RTE_STR(MLX5_CRYPTO_DRIVER_NAME),
		},
		.id_table = mlx5_crypto_pci_id_map,
		.probe = mlx5_crypto_pci_probe,
		.remove = mlx5_crypto_pci_remove,
		.drv_flags = 0,
	},
};

RTE_INIT(rte_mlx5_crypto_init)
{
	mlx5_common_init();
	if (mlx5_glue != NULL)
		mlx5_pci_driver_register(&mlx5_crypto_driver);
}

RTE_PMD_REGISTER_CRYPTO_DRIVER(mlx5_cryptodev_driver, mlx5_drv,
			       mlx5_crypto_driver_id);

RTE_LOG_REGISTER_DEFAULT(mlx5_crypto_logtype, NOTICE)
RTE_PMD_EXPORT_NAME(MLX5_CRYPTO_DRIVER_NAME, __COUNTER__);
RTE_PMD_REGISTER_PCI_TABLE(MLX5_CRYPTO_DRIVER_NAME, mlx5_crypto_pci_id_map);
RTE_PMD_REGISTER_KMOD_DEP(MLX5_CRYPTO_DRIVER_NAME, "* ib_uverbs & mlx5_core & mlx5_ib");