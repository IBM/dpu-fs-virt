/*
#
# Copyright 2022- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include <sys/time.h>
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>
#include <nfsc/libnfs-raw-nfs.h>
#include <nfsc/libnfs-raw-nfs4.h>
#include <linux/fuse.h>
#include <poll.h>
#include <err.h>

#include "virtionfs.h"
#include "mpool.h"
#include "helpers.h"
#include "nfs_v4.h"
#include "fuse_ll.h"


// static uint32_t supported_attrs_attributes[1] = {
//     (1 << FATTR4_SUPPORTED_ATTRS)
// };
static uint32_t standard_attributes[2] = {
    (1 << FATTR4_TYPE |
     1 << FATTR4_SIZE |
     1 << FATTR4_FILEID),
    (1 << (FATTR4_MODE - 32) |
     1 << (FATTR4_NUMLINKS - 32) |
     1 << (FATTR4_OWNER - 32) |
     1 << (FATTR4_OWNER_GROUP - 32) |
     1 << (FATTR4_SPACE_USED - 32) |
     1 << (FATTR4_TIME_ACCESS - 32) |
     1 << (FATTR4_TIME_METADATA - 32) |
     1 << (FATTR4_TIME_MODIFY - 32))
};

int nfs4_op_putfh(struct virtionfs *vnfs, nfs_argop4 *op, uint64_t *nodeid)
{
    op->argop = OP_PUTFH;
    if (*nodeid == FUSE_ROOT_ID) {
        op->nfs_argop4_u.opputfh.object.nfs_fh4_val = vnfs->rootfh.nfs_fh4_val;
        op->nfs_argop4_u.opputfh.object.nfs_fh4_len = vnfs->rootfh.nfs_fh4_len;
    } else {
        op->nfs_argop4_u.opputfh.object.nfs_fh4_val = (char *) nodeid;
        op->nfs_argop4_u.opputfh.object.nfs_fh4_len = sizeof(*nodeid);
    }
    return 1;
}

struct setattr_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;
    struct fuse_out_header *out_hdr;
    struct fuse_attr_out *out_attr;
};

void setattr_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
    struct setattr_cb_data *cb_data = (struct setattr_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:LOOKUP unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:LOOKUP unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }

    cb_data->out_hdr->error = -ENOSYS;

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);}

int setattr(struct fuse_session *se, struct virtionfs *vnfs,
            struct fuse_in_header *in_hdr, struct stat *s, int valid, struct fuse_file_info *fi,
            struct fuse_out_header *out_hdr, struct fuse_attr_out *out_attr,
            struct snap_fs_dev_io_done_ctx *cb)
{
    struct setattr_cb_data *cb_data = mpool_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->out_hdr = out_hdr;
    cb_data->out_attr = out_attr;

    return EWOULDBLOCK;
}

struct lookup_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;
    struct fuse_out_header *out_hdr;
    struct fuse_entry_out *out_entry;
};

void lookup_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
    struct lookup_cb_data *cb_data = (struct lookup_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:LOOKUP unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:LOOKUP unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
	fprintf(stderr, "NFS:LOOKUP unsuccessful: nfs op=%d, nfs error=%d\n",
                res->resarray.resarray_val[0].resop, res->resarray.resarray_val[0].nfs_resop4_u.opputrootfh.status);
	fprintf(stderr, "NFS:LOOKUP unsuccessful: nfs op=%d, nfs error=%d\n",
                res->resarray.resarray_val[1].resop, res->resarray.resarray_val[1].nfs_resop4_u.oplookup.status);
	fprintf(stderr, "NFS:LOOKUP unsuccessful: nfs op=%d, nfs error=%d\n",
                res->resarray.resarray_val[2].resop, res->resarray.resarray_val[2].nfs_resop4_u.opgetattr.status);
	fprintf(stderr, "NFS:LOOKUP unsuccessful: nfs op=%d, nfs error=%d\n",
                res->resarray.resarray_val[3].resop, res->resarray.resarray_val[3].nfs_resop4_u.opgetfh.status);
        goto ret;
    }

    char *attrs = res->resarray.resarray_val[2].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4.obj_attributes.attr_vals.attrlist4_val;
    u_int attrs_len = res->resarray.resarray_val[2].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4.obj_attributes.attr_vals.attrlist4_len;
    int ret = nfs_parse_attributes(vnfs->nfs, &cb_data->out_entry->attr, attrs, attrs_len);
    if (ret != 0) {
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    // Finish the attr
    cb_data->out_entry->attr_valid = 0;
    cb_data->out_entry->attr_valid_nsec = 0;
    cb_data->out_entry->entry_valid = 0;
    cb_data->out_entry->entry_valid_nsec = 0;

    // Get the NFS:FH and return it as the FUSE:nodeid
    cb_data->out_entry->nodeid = *((uint64_t *) res->resarray.resarray_val[3].nfs_resop4_u.opgetfh.GETFH4res_u.resok4.object.nfs_fh4_val);

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}

int lookup(struct fuse_session *se, struct virtionfs *vnfs,
                        struct fuse_in_header *in_hdr, char *in_name,
                        struct fuse_out_header *out_hdr, struct fuse_entry_out *out_entry,
                        struct snap_fs_dev_io_done_ctx *cb)
{
    struct lookup_cb_data *cb_data = mpool_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->out_hdr = out_hdr;
    cb_data->out_entry = out_entry;

    COMPOUND4args args;
    nfs_argop4 op[4];

    // PUTFH
    nfs4_op_putfh(vnfs, &op[0], &in_hdr->nodeid);
    // LOOKUP
    nfs4_op_lookup(vnfs->nfs, &op[1], in_name);
    // FH now replaced with in_name's FH
    // GETATTR
    nfs4_op_getattr(vnfs->nfs, &op[2], standard_attributes, 2);
    // GETFH
    op[3].argop = OP_GETFH;

    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    if (rpc_nfs4_compound_async(vnfs->rpc, lookup_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send nfs4 LOOKUP request\n");
        mpool_free(vnfs->p, cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

struct getattr_cb_data {
    struct snap_fs_dev_io_done_ctx *cb;
    struct virtionfs *vnfs;
    struct fuse_out_header *out_hdr;
    struct fuse_attr_out *out_attr;
};

void getattr_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
    struct getattr_cb_data *cb_data = (struct getattr_cb_data *)private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:LOOKUP unsuccessful: rpc error=%d\n", status);
        cb_data->out_hdr->error = -EREMOTEIO;
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:LOOKUP unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }

    char *attrs = res->resarray.resarray_val[1].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4.obj_attributes.attr_vals.attrlist4_val;
    u_int attrs_len = res->resarray.resarray_val[1].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4.obj_attributes.attr_vals.attrlist4_len;
    if (nfs_parse_attributes(vnfs->nfs, &cb_data->out_attr->attr, attrs, attrs_len) == 0) {
        // This is not filled in by the parse_attributes fn
        cb_data->out_attr->attr.rdev = 0;
        cb_data->out_attr->attr_valid = 0;
        cb_data->out_attr->attr_valid_nsec = 0;
    } else {
        cb_data->out_hdr->error = -EREMOTEIO;
    }

ret:;
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}

int getattr(struct fuse_session *se, struct virtionfs *vnfs,
                      struct fuse_in_header *in_hdr, struct fuse_getattr_in *in_getattr,
                      struct fuse_out_header *out_hdr, struct fuse_attr_out *out_attr,
                    struct snap_fs_dev_io_done_ctx *cb)
{
    struct getattr_cb_data *cb_data = mpool_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->out_hdr = out_hdr;
    cb_data->out_attr = out_attr;

    COMPOUND4args args;
    nfs_argop4 op[2];

    nfs4_op_putfh(vnfs, &op[0], &in_hdr->nodeid);
    nfs4_op_getattr(vnfs->nfs, &op[1], standard_attributes, 2);
    
    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    if (rpc_nfs4_compound_async(vnfs->rpc, getattr_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send nfs4 GETATTR request\n");
        mpool_free(vnfs->p, cb_data);
        out_hdr->error = -EREMOTEIO;
        return 0;
    }

    return EWOULDBLOCK;
}

struct lookup_true_rootfh_cb_data {
    struct virtionfs *vnfs;
    struct fuse_out_header *out_hdr;
    struct snap_fs_dev_io_done_ctx *cb;
    char *export;
};

static void lookup_true_rootfh_cb(struct rpc_context *rpc, int status, void *data,
                       void *private_data) {
    struct lookup_true_rootfh_cb_data *cb_data = private_data;
    struct virtionfs *vnfs = cb_data->vnfs;
    COMPOUND4res *res = data;

    if (status != RPC_STATUS_SUCCESS) {
    	fprintf(stderr, "RPC with NFS:LOOKUP_TRUE_ROOTFH unsuccessful: rpc error=%d\n", status);
        goto ret;
    }
    if (res->status != NFS4_OK) {
        cb_data->out_hdr->error = -nfs_error_to_fuse_error(res->status);
    	fprintf(stderr, "NFS:LOOKUP_TRUE_ROOTFH unsuccessful: nfs error=%d, fuse error=%d\n",
                res->status, cb_data->out_hdr->error);
        goto ret;
    }

    int i = nfs4_find_op(vnfs->nfs, res, OP_GETFH);
    assert(i >= 0);

    // Store the filehandle of the TRUE root (aka the filehandle of where our export lives)
    vnfs->rootfh.nfs_fh4_len = res->resarray.resarray_val[i].nfs_resop4_u.opgetfh.GETFH4res_u.resok4.object.nfs_fh4_len;
    vnfs->rootfh.nfs_fh4_val = malloc(vnfs->rootfh.nfs_fh4_len);
    char *res_fh4_val = res->resarray.resarray_val[i].nfs_resop4_u.opgetfh.GETFH4res_u.resok4.object.nfs_fh4_val;
    memcpy(vnfs->rootfh.nfs_fh4_val, res_fh4_val, vnfs->rootfh.nfs_fh4_len);

ret:
    free(cb_data->export);
    struct snap_fs_dev_io_done_ctx *cb = cb_data->cb;
    mpool_free(vnfs->p, cb_data);
    cb->cb(SNAP_FS_DEV_OP_SUCCESS, cb->user_arg);
}

static int lookup_true_rootfh(struct virtionfs *vnfs, struct fuse_out_header *out_hdr,
    struct snap_fs_dev_io_done_ctx *cb)
{
    struct lookup_true_rootfh_cb_data *cb_data = mpool_alloc(vnfs->p);
    if (!cb_data) {
        out_hdr->error = -ENOMEM;
        return 0;
    }

    cb_data->cb = cb;
    cb_data->vnfs = vnfs;
    cb_data->out_hdr = out_hdr;

    char *export = strdup(vnfs->export);
    cb_data->export = export;
    int export_len = strlen(export);
    // Chop off the last slash, this is to count the correct number
    // of path elements
    if (export[export_len-1] == '/') {
        export[export_len-1] = '\0';
    }
    // Count the slashes
    size_t count = 0;
    while(*vnfs->export) if (*vnfs->export++ == '/') ++count;

    COMPOUND4args args;
    nfs_argop4 op[2+count];
    int i = 0;

    // PUTFH
    op[i++].argop = OP_PUTROOTFH;
    // LOOKUP
    char *token = strtok(export, "/");
    while (i < count+1) {
        nfs4_op_lookup(vnfs->nfs, &op[i++], token);
        token = strtok(NULL, "/");
    }
    // GETFH
    op[i].argop = OP_GETFH;

    memset(&args, 0, sizeof(args));
    args.argarray.argarray_len = sizeof(op) / sizeof(nfs_argop4);
    args.argarray.argarray_val = op;

    if (rpc_nfs4_compound_async(vnfs->rpc, lookup_true_rootfh_cb, &args, cb_data) != 0) {
    	fprintf(stderr, "Failed to send nfs4 LOOKUP request\n");
        mpool_free(vnfs->p, cb_data);
        return -1;
    }

    return 0;
}

int init(struct fuse_session *se, struct virtionfs *vnfs,
    struct fuse_in_header *in_hdr, struct fuse_init_in *in_init,
    struct fuse_conn_info *conn, struct fuse_out_header *out_hdr,
    struct snap_fs_dev_io_done_ctx *cb)
{
    if (conn->capable & FUSE_CAP_EXPORT_SUPPORT)
        conn->want |= FUSE_CAP_EXPORT_SUPPORT;

    if ((vnfs->timeout_sec || vnfs->timeout_nsec) && conn->capable & FUSE_CAP_WRITEBACK_CACHE)
        conn->want |= FUSE_CAP_WRITEBACK_CACHE;

    if (conn->capable & FUSE_CAP_FLOCK_LOCKS)
        conn->want |= FUSE_CAP_FLOCK_LOCKS;

    // FUSE_CAP_SPLICE_READ is enabled in libfuse3 by default,
    // see do_init() in in fuse_lowlevel.c
    // We do not want this as splicing is not a thing with virtiofs
    conn->want &= ~FUSE_CAP_SPLICE_READ;
    conn->want &= ~FUSE_CAP_SPLICE_WRITE;

    int ret;
    if (in_hdr->uid != 0 && in_hdr->gid != 0) {
        ret = seteuid(in_hdr->uid);
        if (ret == -1) {
            warn("%s: Could not set uid of fuser to %d", __func__, in_hdr->uid);
            goto ret_errno;
        }
        ret = setegid(in_hdr->gid);
        if (ret == -1) {
            warn("%s: Could not set gid of fuser to %d", __func__, in_hdr->gid);
            goto ret_errno;
        }
    } else {
        printf("%s, init was not supplied with a non-zero uid and gid. "
        "Thus all operations will go through the name of uid %d and gid %d\n", __func__, getuid(), getgid());
    }

    ret = nfs_mount(vnfs->nfs, vnfs->server, vnfs->export);
    if (ret != 0) {
        printf("Failed to mount nfs\n");
        goto ret_errno;
    }
    if (nfs_mt_service_thread_start(vnfs->nfs)) {
        printf("Failed to start libnfs service thread\n");
        goto ret_errno;
    }

    if (lookup_true_rootfh(vnfs, out_hdr, cb)) {
        printf("Failed to retreive root filehandle for the given export\n");
        out_hdr->error = -ENOENT;
        return 0;
    }

    // TODO WARNING
    // By returning 0, we allow the host to imediately start sending us requests,
    // even though the lookup_true_rootfh might not be done yet
    // This introduces a race condition, where if the rootfh is not found yet
    // virtionfs will crash horribly
    return 0;
ret_errno:
    if (ret == -1)
        out_hdr->error = -errno;
    return 0;
}

void virtionfs_assign_ops(struct fuse_ll_operations *ops) {
    ops->init = (typeof(ops->init)) init;
    ops->lookup = (typeof(ops->lookup)) lookup;
    ops->getattr = (typeof(ops->getattr)) getattr;
    // NFS accepts the NFS:fh (received from a NFS:lookup==FUSE:lookup) as
    // its parameter to the dir ops like readdir
    ops->opendir = NULL;
    //ops->setattr = (typeof(ops->setattr)) setattr;
}

void virtionfs_main(char *server, char *export,
               bool debug, double timeout, uint32_t nthreads,
               struct virtiofs_emu_params *emu_params) {
    struct virtionfs *vnfs = calloc(1, sizeof(struct virtionfs));
    if (!vnfs) {
        warn("Failed to init virtionfs");
        return;
    }
    vnfs->server = server;
    if (export[0] != '/') {
        fprintf(stderr, "export must start with a '/'\n");
        return;
    }
    vnfs->export = export;
    vnfs->debug = debug;
    vnfs->timeout_sec = calc_timeout_sec(timeout);
    vnfs->timeout_nsec = calc_timeout_nsec(timeout);

    vnfs->nfs = nfs_init_context();
    if (vnfs->nfs == NULL) {
        warn("Failed to init nfs context\n");
        goto virtionfs_main_ret_c;
    }
    nfs_set_version(vnfs->nfs, NFS_V4);
    vnfs->rpc = nfs_get_rpc_context(vnfs->nfs);

    vnfs->p = calloc(1, sizeof(struct mpool));
    if (!vnfs->p) {
        warn("Failed to init virtionfs");
        goto virtionfs_main_ret_b;
    }
    if (mpool_init(vnfs->p, sizeof(struct getattr_cb_data), 10) < 0) {
        warn("Failed to init virtionfs");
        goto virtionfs_main_ret_a;
    }

    struct fuse_ll_operations ops;
    memset(&ops, 0, sizeof(ops));
    virtionfs_assign_ops(&ops);

    virtiofs_emu_fuse_ll_main(&ops, emu_params, vnfs, debug);
    printf("nfsclient finished\n");

    mpool_destroy(vnfs->p);
virtionfs_main_ret_a:
    free(vnfs->p);
virtionfs_main_ret_b:
    nfs_destroy_context(vnfs->nfs);
virtionfs_main_ret_c:
    free(vnfs);
}
