/*
 * Copyright (c) 2018  Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#ifndef smb_read_write_h
#define smb_read_write_h

#define SMB_RW_HASH_SZ 8            /* Number of global worker threads */

void smb_rw_init(void);
void smb_rw_cleanup(void);
void smb_rw_proxy(void *arg);

/* smb_rw_arg commands */
typedef enum _SMB_RW_CMD_FLAGS
{
    SMB_READ_WRITE = 0x0001,         /* Read/write */
    SMB_LEASE_BREAK_ACK = 0x0002     /* Lease break ack exchange */
} _SMB_RW_CMD_FLAGS;

/* smb_rw_arg flags */
typedef enum _SMB_RW_ARG_FLAGS
{
    SMB_RW_QUEUED = 0x0001,         /* enqueued and waiting to be sent */
    SMB_RW_IN_USE = 0x0002,         /* this pb is currently being used */
    SMB_RW_REPLY_RCVD = 0x0004      /* reply has arrived */
} _SMB_RW_ARG_FLAGS;

struct smb_rw_arg {
    /* Common */
    TAILQ_ENTRY(smb_rw_arg) sra_svcq;
    
    uint32_t command;
    uint32_t flags;
    lck_mtx_t rw_arg_lock;
    int error;

    /* Read/write */
    struct smb2_rw_rq *read_writep;
    struct smb_rq *rqp;
    user_ssize_t resid;
    
    /* Lease Break Ack */
    struct smb_share *share;
    struct smbiod *iod;
    uint64_t lease_key_hi;
    uint64_t lease_key_low;
    uint32_t lease_state;
    uint32_t ret_lease_state;
    vfs_context_t context;
};


#endif /* smb_read_write_h */
