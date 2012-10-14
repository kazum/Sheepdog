/*
 * Copyright (C) 2009-2011 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>

#include "sheep_priv.h"

static inline void gateway_init_fwd_hdr(struct sd_req *fwd, struct sd_req *hdr)
{
	memcpy(fwd, hdr, sizeof(*fwd));
	fwd->opcode = gateway_to_peer_opcode(hdr->opcode);
	fwd->proto_ver = SD_SHEEP_PROTO_VER;
}

/*
 * Try our best to read one copy and read local first.
 *
 * Return success if any read succeed. We don't call gateway_forward_request()
 * because we only read once.
 */
int gateway_read_obj(struct request *req)
{
	int i, ret = SD_RES_SUCCESS;
	unsigned wlen, rlen;
	struct sd_req fwd_hdr;
	struct sd_rsp *rsp = (struct sd_rsp *)&fwd_hdr;
	struct sd_vnode *v;
	struct sd_vnode *obj_vnodes[SD_MAX_COPIES];
	uint64_t oid = req->rq.obj.oid;
	int nr_copies, j;

	if (is_object_cache_enabled() && !req->local &&
	    !bypass_object_cache(req)) {
		ret = object_cache_handle_request(req);
		goto out;
	}

	nr_copies = get_req_copy_number(req);
	oid_to_vnodes(req->vinfo->vnodes, req->vinfo->nr_vnodes, oid,
		      nr_copies, obj_vnodes);
	for (i = 0; i < nr_copies; i++) {
		v = obj_vnodes[i];
		if (!vnode_is_local(v))
			continue;
		ret = peer_read_obj(req);
		if (ret == SD_RES_SUCCESS)
			goto out;

		eprintf("local read fail %x\n", ret);
		break;
	}

	/*
	 * Read random copy from cluster for better load balance, useful for
	 * reading base VM's COW objects
	 */
	j = random();
	for (i = 0; i < nr_copies; i++) {
		int idx = (i + j) % nr_copies;

		v = obj_vnodes[idx];
		if (vnode_is_local(v))
			continue;
		/*
		 * We need to re-init it because rsp and req share the same
		 * structure.
		 */
		gateway_init_fwd_hdr(&fwd_hdr, &req->rq);
		wlen = 0;
		rlen = fwd_hdr.data_length;
		ret = sheep_exec_req(&v->nid, &fwd_hdr, req->data, &wlen,
				     &rlen);
		if (ret != SD_RES_SUCCESS)
			continue;

		/* Read success */
		memcpy(&req->rp, rsp, sizeof(*rsp));
		break;
	}
out:
	if (ret == SD_RES_SUCCESS &&
	    req->rq.proto_ver < SD_PROTO_VER_TRIM_ZERO_SECTORS) {
		/* the client doesn't support trimming zero bytes */
		set_trimmed_sectors(req->data, req->rp.obj.offset,
				    req->rp.data_length, req->rq.data_length);
		req->rp.data_length = req->rq.data_length;
		req->rp.obj.offset = 0;
	}

	if (is_vdi_obj(oid) && req->rq.obj.offset == 0 && ret == SD_RES_NO_OBJ) {
		uint64_t vdi_oid = req->rq.obj.oid, deleted_vdi_oid;
		uint32_t length = req->rq.data_length;
		deleted_vdi_oid = vid_to_deleted_vdi_oid(oid_to_vid(vdi_oid));

		dprintf("read deleted vdi, %" PRIx64 "\n", deleted_vdi_oid);
		req->rq.obj.oid = deleted_vdi_oid;
		req->rq.data_length = min(req->rq.data_length,
					  (uint32_t)SD_INODE_HEADER_SIZE);
		ret = gateway_read_obj(req);
		req->rq.obj.oid = vdi_oid;
		req->rq.data_length = length;
	}
	return ret;
}

struct write_info_entry {
	struct pollfd pfd;
	struct node_id *nid;
	struct sockfd *sfd;
};

struct write_info {
	struct write_info_entry ent[SD_MAX_NODES];
	int nr_sent;
};

static inline void write_info_update(struct write_info *wi, int pos)
{
	dprintf("%d, %d\n", wi->nr_sent, pos);
	wi->nr_sent--;
	memmove(wi->ent + pos, wi->ent + pos + 1,
		sizeof(struct write_info_entry) * (wi->nr_sent - pos));
}

static inline void finish_one_write(struct write_info *wi, int i)
{
	sheep_put_sockfd(wi->ent[i].nid, wi->ent[i].sfd);
	write_info_update(wi, i);
}

static inline void finish_one_write_err(struct write_info *wi, int i)
{
	sheep_del_sockfd(wi->ent[i].nid, wi->ent[i].sfd);
	write_info_update(wi, i);
}

struct pfd_info {
	struct pollfd pfds[SD_MAX_NODES];
	int nr;
};

static inline void pfd_info_init(struct write_info *wi, struct pfd_info *pi)
{
	int i;
	for (i = 0; i < wi->nr_sent; i++)
		pi->pfds[i] = wi->ent[i].pfd;
	pi->nr = wi->nr_sent;
}

/*
 * Wait for all forward requests completion.
 *
 * Even if something goes wrong, we have to wait forward requests completion to
 * avoid interleaved requests.
 *
 * Return error code if any one request fails.
 */
static int wait_forward_request(struct write_info *wi, struct request *req)
{
	int nr_sent, err_ret = SD_RES_SUCCESS, ret, pollret, i;
	struct pfd_info pi;
	struct sd_rsp *rsp = &req->rp;
again:
	pfd_info_init(wi, &pi);
	pollret = poll(pi.pfds, pi.nr, 5000);
	if (pollret < 0) {
		if (errno == EINTR)
			goto again;

		panic("%m\n");
	} else if (pollret == 0) {
		eprintf("poll timeout %d\n", wi->nr_sent);

		if (req->rq.epoch == sys_epoch())
			goto again;

		nr_sent = wi->nr_sent;
		/* XXX Blinedly close all the connections */
		for (i = 0; i < nr_sent; i++)
			finish_one_write_err(wi, i);

		err_ret = SD_RES_NETWORK_ERROR;
		goto finish_write;
	}

	nr_sent = wi->nr_sent;
	for (i = 0; i < nr_sent; i++)
		if (pi.pfds[i].revents & POLLIN)
			break;
	if (i < nr_sent) {
		int re = pi.pfds[i].revents;
		dprintf("%d, revents %x\n", i, re);
		if (re & (POLLERR | POLLHUP | POLLNVAL)) {
			err_ret = SD_RES_NETWORK_ERROR;
			finish_one_write_err(wi, i);
			goto finish_write;
		}
		if (do_read(pi.pfds[i].fd, rsp, sizeof(*rsp))) {
			eprintf("remote node might have gone away\n");
			err_ret = SD_RES_NETWORK_ERROR;
			finish_one_write_err(wi, i);
			goto finish_write;
		}

		ret = rsp->result;
		if (ret != SD_RES_SUCCESS) {
			eprintf("fail %"PRIx32"\n", ret);
			err_ret = ret;
		}
		finish_one_write(wi, i);
	}
finish_write:
	if (wi->nr_sent > 0)
		goto again;

	return err_ret;
}

static inline void write_info_init(struct write_info *wi)
{
	int i;
	for (i = 0; i < SD_MAX_NODES; i++)
		wi->ent[i].pfd.fd = -1;
	wi->nr_sent = 0;
}

static inline void
write_info_advance(struct write_info *wi, struct node_id *nid,
		   struct sockfd *sfd)
{
	wi->ent[wi->nr_sent].nid = nid;
	wi->ent[wi->nr_sent].pfd.fd = sfd->fd;
	wi->ent[wi->nr_sent].pfd.events = POLLIN;
	wi->ent[wi->nr_sent].sfd = sfd;
	wi->nr_sent++;
}

static int init_target_nodes(struct request *req, bool all_node,
			uint64_t oid, struct sd_node **target_nodes)
{
	int i, nr_to_send;
	struct vnode_info *vinfo = req->vinfo;

	if (all_node) {
		nr_to_send = vinfo->nr_nodes;
		for (i = 0; i < nr_to_send; i++)
			target_nodes[i] = &vinfo->nodes[i];

		return nr_to_send;
	}

	nr_to_send = get_req_copy_number(req);
	oid_to_nodes(vinfo->vnodes, vinfo->nr_vnodes, oid, nr_to_send,
		vinfo->nodes, target_nodes);

	return nr_to_send;
}

static int gateway_forward_request(struct request *req, bool all_node)
{
	int i, err_ret = SD_RES_SUCCESS, ret, local = -1;
	unsigned wlen;
	uint64_t oid = req->rq.obj.oid;
	int nr_to_send;
	struct write_info wi;
	struct sd_op_template *op;
	struct sd_req hdr;
	struct sd_node *target_nodes[SD_MAX_NODES];

	dprintf("%"PRIx64"\n", oid);

	gateway_init_fwd_hdr(&hdr, &req->rq);
	op = get_sd_op(hdr.opcode);

	write_info_init(&wi);
	wlen = hdr.data_length;
	nr_to_send = init_target_nodes(req, all_node, oid, target_nodes);

	for (i = 0; i < nr_to_send; i++) {
		struct sockfd *sfd;
		struct node_id *nid;

		if (node_is_local(target_nodes[i])) {
			local = i;
			continue;
		}

		nid = &target_nodes[i]->nid;
		sfd = sheep_get_sockfd(nid);
		if (!sfd) {
			err_ret = SD_RES_NETWORK_ERROR;
			break;
		}

		ret = send_req(sfd->fd, &hdr, req->data, &wlen);
		if (ret) {
			sheep_del_sockfd(nid, sfd);
			err_ret = SD_RES_NETWORK_ERROR;
			dprintf("fail %d\n", ret);
			break;
		}
		write_info_advance(&wi, nid, sfd);
	}

	if (local != -1 && err_ret == SD_RES_SUCCESS) {
		assert(op);
		ret = sheep_do_op_work(op, req);

		if (ret != SD_RES_SUCCESS) {
			eprintf("fail to write local %"PRIx32"\n", ret);
			err_ret = ret;
		}
	}

	dprintf("nr_sent %d, err %x\n", wi.nr_sent, err_ret);
	if (wi.nr_sent > 0) {
		ret = wait_forward_request(&wi, req);
		if (ret != SD_RES_SUCCESS)
			err_ret = ret;
	}

	return err_ret;
}


static int prepare_obj_refcnt(const struct sd_req *hdr, uint32_t *vids,
			      struct generation_reference *refs)
{
	int ret;
	size_t nr_vids = hdr->data_length / sizeof(*vids);
	uint64_t offset;
	int start;
	struct sheepdog_inode *inode = xzalloc(sizeof(*inode));

	offset = hdr->obj.offset - offsetof(struct sheepdog_inode, data_vdi_id);
	start = offset / sizeof(*vids);

	ret = read_object(hdr->obj.oid, (char *)inode, sizeof(*inode), 0,
			  hdr->obj.copies);
	if (ret != SD_RES_SUCCESS) {
		eprintf("failed to read vdi, %" PRIx64 "\n", hdr->obj.oid);
		goto out;
	}

	memcpy(vids, inode->data_vdi_id + start, nr_vids * sizeof(vids[0]));
	memcpy(refs, inode->data_ref + start, nr_vids * sizeof(refs[0]));
out:
	free(inode);
	return ret;
}

/* This function decreases a refcnt of vid_to_data_oid(old_vid, idx)
   and increases one of vid_to_data_oid(new_vid, idx) */
static int update_obj_refcnt(const struct sd_req *hdr, uint32_t *vids,
			     uint32_t *new_vids, struct generation_reference *refs)
{
	int i, start, ret = SD_RES_SUCCESS;
	size_t nr_vids = hdr->data_length / sizeof(*vids);
	uint64_t offset;

	offset = hdr->obj.offset - offsetof(struct sheepdog_inode, data_vdi_id);
	start = offset / sizeof(*vids);

	for (i = 0; i < nr_vids; i++) {
		if (vids[i] == 0 || vids[i] == new_vids[i])
			continue;

		ret = discard_object_ref(vid_to_data_oid(vids[i], i + start),
					 refs[i].generation, refs[i].count);
		if (ret != SD_RES_SUCCESS)
			eprintf("fail, %d\n", ret);

		refs[i].generation = 0;
		refs[i].count = 0;
	}

	return write_object(hdr->obj.oid, (char *)refs, nr_vids * sizeof(*refs),
			    offsetof(struct sheepdog_inode, data_ref) +
				start * sizeof(*refs),
			    hdr->flags, false, hdr->obj.copies);
}

/* return true if the request updates a data_vdi_id field of a vdi object */
static bool is_data_vid_update(const struct sd_req *hdr)
{
	if (!is_vdi_obj(hdr->obj.oid))
		return false;
	if (hdr->obj.offset < SD_INODE_HEADER_SIZE)
		return false;
	if (hdr->obj.offset + hdr->data_length >
	    offsetof(struct sheepdog_inode, data_ref))
		return false;

	return true;
}

int gateway_write_obj(struct request *req)
{
	int ret;
	struct sd_req *hdr = &req->rq;
	uint32_t *vids = NULL, *new_vids = req->data;
	struct generation_reference *refs = NULL;

	if (is_object_cache_enabled() && !req->local && !bypass_object_cache(req))
		return object_cache_handle_request(req);

	if (is_data_vid_update(hdr)) {
		size_t nr_vids = hdr->data_length / sizeof(*vids);

		/* read the previous vids to discard their references later */
		vids = xzalloc(hdr->data_length);
		refs = xzalloc(sizeof(*refs) * nr_vids);
		if (hdr->flags & SD_FLAG_CMD_UPDATE_VID) {
			assert(nr_vids == 1);
			vids[0] = oid_to_vid(hdr->obj.cow_oid);
		} else {
			ret = prepare_obj_refcnt(hdr, vids, refs);
			if (ret != SD_RES_SUCCESS)
				goto out;
		}
	}

	ret = gateway_forward_request(req, false);
	if (ret != SD_RES_SUCCESS)
		goto out;

	if (is_data_vid_update(hdr)) {
		dprintf("udpate reference counts, %" PRIx64 "\n", hdr->obj.oid);

		if (hdr->flags & SD_FLAG_CMD_UPDATE_VID) {
			refs[0].generation = req->rp.obj.generation;
			refs[0].count = req->rp.obj.refcnt;
		}

		ret = update_obj_refcnt(hdr, vids, new_vids, refs);
	}
out:
	free(vids);
	free(refs);
	return ret;
}

int gateway_create_and_write_obj(struct request *req)
{
	if (is_object_cache_enabled() && !req->local && !bypass_object_cache(req))
		return object_cache_handle_request(req);

	return gateway_forward_request(req, false);
}

int gateway_remove_obj(struct request *req)
{
	return gateway_forward_request(req, false);
}

int gateway_flush_nodes(struct request *req)
{
	return gateway_forward_request(req, true);
}

int gateway_discard_ref(struct request *req)
{
	return gateway_forward_request(req, false);
}
