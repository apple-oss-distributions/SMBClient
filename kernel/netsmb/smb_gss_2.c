/*
 * Copyright (c) 2011-2012  Apple Inc. All rights reserved.
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

#include <sys/smb_apple.h>

#include <netsmb/smb.h>
#include <netsmb/smb_2.h>
#include <netsmb/smb_rq.h>
#include <netsmb/smb_rq_2.h>
#include <netsmb/smb_conn.h>
#include <netsmb/smb_conn_2.h>
#include <netsmb/smb_gss_2.h>

int
smb_gss_ssandx(struct smbiod *iod, uint32_t caps, uint16_t *action,
               vfs_context_t context)
{
    int retval;
    uint16_t sess_flags = 0;
    struct smb_session *sessionp = iod->iod_session; 
    
    if (sessionp->session_flags & SMBV_SMB2) {
        retval = smb2_smb_gss_session_setup(iod, &sess_flags, context);
        if (retval == 0) {
            if (iod->iod_flags & SMBIOD_ALTERNATE_CHANNEL) {
                if (sessionp->session_sopt.sv_sessflags & SMB2_SESSION_FLAG_IS_GUEST) {
                    SMBERROR("id %u SMB2_SESSION_FLAG_IS_GUEST should not be set on alt ch (0x%x)",
                             iod->iod_id, sessionp->session_sopt.sv_sessflags);
                    return EINVAL;
                }

                if ((sessionp->session_sopt.sv_sessflags & SMB2_SESSION_FLAG_IS_NULL) != (sess_flags & SMB2_SESSION_FLAG_IS_NULL)) {
                    // Just warnout no action needed
                    SMBWARNING("id %u ALternate channel SMB2_SESSION_FLAG_IS_NULL mismatch (0x%x, 0x%x)",
                             iod->iod_id, sessionp->session_sopt.sv_sessflags, sess_flags);
                }

                /*
                 * Ignore SMB2_SESSION_FLAG_ENCRYPT_DATA mismatch, since win server 2019 does not set
                 * this bit on alternate channel.
                 */

            } else {
                /* Save Flags field from Session Setup reply */
                sessionp->session_sopt.sv_sessflags = sess_flags;
            }

            /* Remap SMB 2/3 session flags to SMB 1 action flags */
            if (sess_flags & SMB2_SESSION_FLAG_IS_GUEST) {
                /* Return that we got logged in as Guest */
                *action |= SMB_ACT_GUEST;
            }
        }
    }
    else {
        retval = smb1_gss_ssandx(sessionp, caps, action, context);
    }
    return (retval);
}

