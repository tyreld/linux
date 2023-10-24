/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ibmvfc-nvme.c -- IBM Power Virtual Fibre Channel NVMeoF HBA driver
 *
 * Written By: Tyrel Datwyler <tyreld@linux.ibm.com>, IBM Corporation
 *
 * Copyright (C) IBM Corporation, 2022
 */

#include <linux/dmapool.h>
#include <scsi/scsi_transport_fc.h>

#include "ibmvfc-nvme.h"

static void ibmvfc_nvme_localport_delete(struct nvme_fc_local_port *lport)
{
	struct ibmvfc_host *vhost = lport->private;

	vhost->nvme_local_port = NULL;
	complete(&vhost->nvme_delete_done);
}

static void ibmvfc_nvme_remoteport_delete(struct nvme_fc_remote_port *rport)
{
	struct ibmvfc_target *tgt = rport->private;

	tgt->nvme_remote_port = NULL;
	complete(&tgt->nvme_delete_done);
}

static int ibmvfc_nvme_create_queue(struct nvme_fc_local_port *lport, unsigned int qidx,
				    u16 qsize, void **handle)
{
	struct ibmvfc_host *vhost = lport->private;
	struct ibmvfc_nvme_qhandle *qhandle;

	qhandle = kzalloc(sizeof(struct ibmvfc_nvme_qhandle), GFP_KERNEL);
	if (!qhandle)
		return -ENOMEM;

	qhandle->cpu_id = raw_smp_processor_id();
	qhandle->qidx = qidx;

	/* Admin and first IO queue are both mapped to index 0 */
	if (qidx)
		qhandle->index = (qidx - 1) % vhost->nvme_scrqs.active_queues;
	else
		qhandle->index = qidx;

	qhandle->queue = &vhost->nvme_scrqs.scrqs[qhandle->index];

	*handle = qhandle;
	return 0;
}

static void ibmvfc_nvme_delete_queue(struct nvme_fc_local_port *lport, unsigned int qidx,
				     void *handle)
{
	kfree(handle);
}

static void ibmvfc_ls_req_done(struct ibmvfc_event *evt)
{
	struct ibmvfc_target *tgt = evt->tgt;
	struct ibmvfc_passthru_mad *mad = &evt->xfer_iu->passthru;
	struct fcnvme_ls_rqst_w0 *ls_rqst;
	struct fcnvme_ls_cr_assoc_acc *ls_resp;
	u32 status = be16_to_cpu(mad->common.status);
	int rc = 0;

	ls_rqst = (struct fcnvme_ls_rqst_w0 *)evt->ls_req->rqstaddr;
	ls_resp = (struct fcnvme_ls_cr_assoc_acc *)evt->ls_req->rspaddr;

	switch (status) {
	case IBMVFC_MAD_SUCCESS:
		tgt_dbg(tgt, "ls_req succeeded\n");
		if ((ls_rqst->ls_cmd == FCNVME_LS_CREATE_ASSOCIATION) &&
		    (ls_resp->hdr.w0.ls_cmd == FCNVME_LS_ACC)) {
			tgt->assoc_id = be64_to_cpu(ls_resp->associd.association_id);
			tgt_dbg(tgt, "assoc_id 0x%llx\n", tgt->assoc_id);
		}
		break;
	case IBMVFC_MAD_DRIVER_FAILED:
		break;
	case IBMVFC_MAD_FAILED:
	default:
		tgt_info(tgt, "ls_req failed: %s (%x:%x) rc=0x%02X\n",
			 ibmvfc_get_cmd_error(be16_to_cpu(mad->iu.status), be16_to_cpu(mad->iu.error)),
			 be16_to_cpu(mad->iu.status), be16_to_cpu(mad->iu.error), status);
		break;
	}

	if (status)
		rc = -ENXIO;

	evt->ls_req->done(evt->ls_req, rc);

	kref_put(&tgt->kref, ibmvfc_release_tgt);
	ibmvfc_free_event(evt);
}

static void ibmvfc_init_ls_req(struct ibmvfc_event *evt, struct nvmefc_ls_req *ls_req)
{
	struct ibmvfc_passthru_mad *mad = &evt->iu.passthru;

	memset(mad, 0, sizeof(*mad));
	mad->common.version = cpu_to_be32(2);
	mad->common.opcode = cpu_to_be32(IBMVFC_NVMF_PASSTHRU);
	mad->common.length = cpu_to_be16(sizeof(*mad) - sizeof(mad->fc_iu) - sizeof(mad->iu));
	mad->cmd_ioba.va = cpu_to_be64((u64)be64_to_cpu(evt->crq.ioba) +
				       offsetof(struct ibmvfc_passthru_mad, iu));
	mad->cmd_ioba.len = cpu_to_be32(sizeof(mad->iu));
	mad->iu.cmd_len = cpu_to_be32(ls_req->rqstlen);
	mad->iu.rsp_len = cpu_to_be32(ls_req->rsplen);
	mad->iu.cmd.va = cpu_to_be64(ls_req->rqstdma);
	mad->iu.cmd.len = cpu_to_be32(ls_req->rqstlen);
	mad->iu.rsp.va = cpu_to_be64(ls_req->rspdma);
	mad->iu.rsp.len = cpu_to_be32(ls_req->rsplen);
}

static int ibmvfc_nvme_ls_req(struct nvme_fc_local_port *lport,
			      struct nvme_fc_remote_port *rport,
			      struct nvmefc_ls_req *ls_req)
{
	struct ibmvfc_host *vhost = lport->private;
	struct ibmvfc_target *tgt = rport->private;
	struct ibmvfc_passthru_mad *mad;
	struct ibmvfc_event *evt;

	kref_get(&tgt->kref);
	evt = ibmvfc_get_event(&vhost->crq);
	if (!evt)
		return -EBUSY;

	ibmvfc_init_event(evt, ibmvfc_ls_req_done, IBMVFC_MAD_FORMAT);
	evt->tgt = tgt;
	evt->ls_req = ls_req;
	ls_req->private = evt;

	ibmvfc_init_ls_req(evt, ls_req);
	mad = &evt->iu.passthru;
	mad->iu.flags = cpu_to_be32(IBMVFC_FC4_LS_DSC_CTRL);
	mad->iu.scsi_id = cpu_to_be64(tgt->scsi_id);
	mad->iu.cancel_key = cpu_to_be32((u64)evt);
	mad->iu.target_wwpn = cpu_to_be64(tgt->wwpn);

	ibmvfc_dbg(vhost, "nvme_ls_req\n");
	if (ibmvfc_send_event(evt, vhost, IBMVFC_FC4_LS_PLUS_CANCEL_TIMEOUT)) {
		kref_put(&tgt->kref, ibmvfc_release_tgt);
		ibmvfc_free_event(evt);
		return -ENXIO;
	}

	return 0;
}

static void ibmvfc_nvme_ls_abort(struct nvme_fc_local_port *lport,
				struct nvme_fc_remote_port *rport,
				struct nvmefc_ls_req *ls_abort)
{
}

static void ibmvfc_nvme_done(struct ibmvfc_event *evt)
{
	struct ibmvfc_cmd *vfc_cmd = &evt->xfer_iu->cmd;
	struct nvmefc_fcp_req *fcp_req = evt->fcp_req;
	struct nvme_fc_ersp_iu *ersp = (struct nvme_fc_ersp_iu *)fcp_req->rspaddr;
	struct nvme_completion *cqe = &ersp->cqe;
	struct nvme_command *sqe = &((struct nvme_fc_cmd_iu *)fcp_req->cmdaddr)->sqe;

	ibmvfc_dbg(evt->vhost, "fc_done: (%x:%x)\n", be16_to_cpu(vfc_cmd->status),
		   be16_to_cpu(vfc_cmd->error));
	ibmvfc_dbg(evt->vhost, "fc_done: cmdlen: %d, rsplen %d, payload_len %d\n",
		   fcp_req->cmdlen, fcp_req->rsplen, fcp_req->payload_length);

	fcp_req->status = 0;
	if (!vfc_cmd->status) {
		fcp_req->rcv_rsplen = NVME_FC_SIZEOF_ZEROS_RSP;
		fcp_req->transferred_length = fcp_req->payload_length;
	} else if (be16_to_cpu(vfc_cmd->status) & IBMVFC_FC_NVME_STATUS) {
		fcp_req->rcv_rsplen = sizeof(struct nvme_fc_ersp_iu);
		fcp_req->transferred_length = be32_to_cpu(ersp->xfrd_len);
		if (be16_to_cpu(vfc_cmd->error) & IBMVFC_NVMS_VALID_NODMA_CQE) {
			cqe->command_id = sqe->common.command_id;
		}
	} else {
		fcp_req->status = -EIO;
	}

	fcp_req->done(fcp_req);
	ibmvfc_free_event(evt);
}

static struct ibmvfc_cmd *ibmvfc_nvme_init_vfc_cmd(struct ibmvfc_event *evt,
						   struct nvme_fc_remote_port *rport,
						   struct nvmefc_fcp_req *fcp_req)
{
	struct ibmvfc_target *tgt = rport->private;
	struct ibmvfc_cmd *vfc_cmd = &evt->iu.cmd;

	memset(vfc_cmd, 0, sizeof(*vfc_cmd));

	vfc_cmd->resp.va = cpu_to_be64(fcp_req->rspdma);
	vfc_cmd->resp.len = cpu_to_be32(fcp_req->rsplen);
	vfc_cmd->frame_type = cpu_to_be32(IBMVFC_NVME_FCP_TYPE);
	vfc_cmd->flags |= cpu_to_be16(IBMVFC_NVMEOF_PROTOCOL);
	vfc_cmd->payload_len = cpu_to_be32(fcp_req->cmdlen);
	vfc_cmd->resp_len = cpu_to_be32(fcp_req->rsplen);
	vfc_cmd->cancel_key = cpu_to_be32((u64)evt);
	vfc_cmd->target_wwpn = cpu_to_be64(rport->port_name);
	vfc_cmd->tgt_scsi_id = cpu_to_be64(rport->port_id);
	vfc_cmd->assoc_id = cpu_to_be64(tgt->assoc_id);

	memcpy(&vfc_cmd->v3nvme.iu, fcp_req->cmdaddr, fcp_req->cmdlen);

	return vfc_cmd;
}

static void ibmvfc_nvme_map_sg_list(struct nvmefc_fcp_req *fcp_req,
				    struct srp_direct_buf *md)
{
	int i;
	struct scatterlist *sg;

	for_each_sg(fcp_req->first_sgl, sg, fcp_req->sg_cnt, i) {
		md[i].va = cpu_to_be64(sg_dma_address(sg));
		md[i].len = cpu_to_be32(sg_dma_len(sg));
		md[i].key = 0;
	}
}

static int ibmvfc_nvme_map_sg_data(struct nvmefc_fcp_req *fcp_req,
				    struct ibmvfc_event *evt,
				    struct ibmvfc_cmd *vfc_cmd)
{
	struct srp_direct_buf *data = &vfc_cmd->ioba;
	struct ibmvfc_host *vhost = evt->vhost;

	if (!fcp_req->sg_cnt) {
		vfc_cmd->flags |= cpu_to_be16(IBMVFC_NO_MEM_DESC);
		return 0;
	}

	if (fcp_req->io_dir == NVMEFC_FCP_WRITE)
		vfc_cmd->flags |= cpu_to_be16(IBMVFC_WRITE);
	else
		vfc_cmd->flags |= cpu_to_be16(IBMVFC_READ);

	if (fcp_req->sg_cnt == 1) {
		ibmvfc_nvme_map_sg_list(fcp_req, data);
		return 0;
	}

	vfc_cmd->flags |= cpu_to_be16(IBMVFC_SCATTERLIST);

	if (!evt->ext_list) {
		evt->ext_list = dma_pool_alloc(vhost->sg_pool, GFP_ATOMIC,
					       &evt->ext_list_token);

		if (!evt->ext_list) {
			return -ENOMEM;
		}
	}

	ibmvfc_nvme_map_sg_list(fcp_req, evt->ext_list);

	data->va = cpu_to_be64(evt->ext_list_token);
	data->len = cpu_to_be32(fcp_req->sg_cnt * sizeof(struct srp_direct_buf));
	data->key = 0;
	return 0;
}

static int ibmvfc_nvme_fcp_io(struct nvme_fc_local_port *lport,
			      struct nvme_fc_remote_port *rport,
			      void *hw_queue_handle,
			      struct nvmefc_fcp_req *fcp_req)
{
	struct ibmvfc_host *vhost = lport->private;
	struct ibmvfc_nvme_qhandle *qhandle = hw_queue_handle;
	struct ibmvfc_cmd *vfc_cmd;
	struct ibmvfc_event *evt;
	int rc;

	ibmvfc_dbg(vhost, "nvme_fcp_io\n");
	evt = ibmvfc_get_event(qhandle->queue);
	if (!evt)
		return -EBUSY;

	evt->hwq = qhandle->index;
	ibmvfc_dbg(vhost, "vfc-nvme-mq-%d\n", evt->hwq);

	ibmvfc_init_event(evt, ibmvfc_nvme_done, IBMVFC_CMD_FORMAT);
	evt->fcp_req = fcp_req;
	fcp_req->private = evt;

	vfc_cmd = ibmvfc_nvme_init_vfc_cmd(evt, rport, fcp_req);

	vfc_cmd->correlation = cpu_to_be64((u64)evt);

	if (likely(!(rc = ibmvfc_nvme_map_sg_data(fcp_req, evt, vfc_cmd))))
		return ibmvfc_send_event(evt, vhost, 0);

	ibmvfc_free_event(evt);

	return rc;
}

static void ibmvfc_nvme_fcp_abort(struct nvme_fc_local_port *lport,
				  struct nvme_fc_remote_port *rport,
				  void *hw_queue_handle,
				  struct nvmefc_fcp_req *abort_req)
{
}

static struct nvme_fc_port_template ibmvfc_nvme_fc_transport = {
	.localport_delete	= ibmvfc_nvme_localport_delete,
	.remoteport_delete	= ibmvfc_nvme_remoteport_delete,
	.create_queue		= ibmvfc_nvme_create_queue,
	.delete_queue		= ibmvfc_nvme_delete_queue,
	.ls_req			= ibmvfc_nvme_ls_req,
	.ls_abort		= ibmvfc_nvme_ls_abort,
	.fcp_io			= ibmvfc_nvme_fcp_io,
	.fcp_abort		= ibmvfc_nvme_fcp_abort,
	.map_queues		= NULL,
	.max_hw_queues		= IBMVFC_NVME_HW_QUEUES,
	.max_sgl_segments	= 1024,
	.max_dif_sgl_segments	= 64,
	.dma_boundary		= 0xFFFFFFFF,
	.local_priv_sz		= sizeof(struct ibmvfc_host *),
	.remote_priv_sz		= sizeof(struct ibmvfc_target *),
	.lsrqst_priv_sz		= sizeof(struct ibmvfc_event *),
	.fcprqst_priv_sz	= sizeof(struct ibmvfc_event *),
};

int ibmvfc_nvme_register_remoteport(struct ibmvfc_target *tgt)
{
	struct ibmvfc_host *vhost = tgt->vhost;
	struct nvme_fc_port_info req;
	int rc;

	if (!vhost->nvme_local_port)
		dev_err(vhost->dev, "Attempt to register NVMe fc remoteport without valid localport\n");

	memset(&req, 0, sizeof(struct nvme_fc_port_info));
	req.node_name = tgt->ids.node_name;
	req.port_name = tgt->ids.port_name;
	req.port_id = tgt->ids.port_id;
	req.port_role = FC_PORT_ROLE_NVME_TARGET;

	rc = nvme_fc_register_remoteport(vhost->nvme_local_port, &req,
					 &tgt->nvme_remote_port);

	if (!rc) {
		ibmvfc_log(vhost, 2, "register_remoteport: traddr=nn-0x%llx:pn-0x%llx PortID:%x\n",
			   req.node_name, req.port_name, req.port_id);
		tgt->nvme_remote_port->private = tgt;
	}

	return 0;
}

void ibmvfc_nvme_unregister_remoteport(struct ibmvfc_target *tgt)
{
	int rc;

	init_completion(&tgt->nvme_delete_done);
	rc = nvme_fc_unregister_remoteport(tgt->nvme_remote_port);

	wait_for_completion(&tgt->nvme_delete_done);
}

int ibmvfc_nvme_register(struct ibmvfc_host *vhost)
{
	struct nvme_fc_port_info pinfo;
	int rc;

	pinfo.node_name = fc_host_node_name(vhost->host);
	pinfo.port_name = fc_host_port_name(vhost->host);
	pinfo.port_id = fc_host_port_id(vhost->host);
	pinfo.port_role = FC_PORT_ROLE_NVME_INITIATOR;
	pinfo.dev_loss_tmo = 0;

	rc = nvme_fc_register_localport(&pinfo, &ibmvfc_nvme_fc_transport,
					get_device(vhost->dev),
					&vhost->nvme_local_port);

	if (!rc) {
		ibmvfc_log(vhost, 2, "register_localport: host-traddr=nn-0x%llx:pn-0x%llx on portID:%x\n",
			   pinfo.node_name, pinfo.port_name, pinfo.port_id);
		vhost->nvme_local_port->private = vhost;
	} else
		dev_err(vhost->dev, "Failed to register NVMe fc localport (%d)\n", rc);

	return rc;
}

void ibmvfc_nvme_unregister(struct ibmvfc_host *vhost)
{
	int rc;

	if (vhost->nvme_local_port) {
		init_completion(&vhost->nvme_delete_done);
		rc = nvme_fc_unregister_localport(vhost->nvme_local_port);
		if (!rc)
			wait_for_completion(&vhost->nvme_delete_done);
	}
}
