/*
 * Copyright (c) 2000-2001, Boris Popov
 * All rights reserved.
 *
 * Portions Copyright (C) 2001 - 2012 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    This product includes software developed by Boris Popov.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id: smbfs_subr.c,v 1.24 2006/02/03 04:04:12 lindak Exp $
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/time.h>
#include <sys/vnode.h>
#include <sys/sysctl.h>

#include <sys/kauth.h>

#include <sys/smb_apple.h>

#include <netsmb/smb.h>
#include <netsmb/smb_2.h>
#include <netsmb/smb_conn.h>
#include <netsmb/smb_subr.h>
#include <netsmb/smb_rq.h>
#include <netsmb/smb_dev.h>

#include <smbfs/smbfs.h>
#include <smbfs/smbfs_lockf.h>
#include <smbfs/smbfs_node.h>
#include <smbfs/smbfs_subr.h>
#include <smbfs/smbfs_subr_2.h>
#include <netsmb/smb_converter.h>

/* 
 * Time & date conversion routines taken from msdosfs. Although leap
 * year calculation is bogus, it's sufficient before 2100 :)
 */

/*  Number of seconds between 1970 and 1601 year */
uint64_t DIFF1970TO1601 = 11644473600ULL;

/*  Number of 100 nanosecond intervals in a second*/
uint64_t HUNDRED_NSEC_PER_SEC = (NSEC_PER_SEC/100);

/*
 * The nsec field is a NT Style File Time.
 *
 * A file time is a 64-bit value that represents the number of 100-nanosecond 
 * intervals that have elapsed since 12:00 A.M. January 1, 1601 Coordinated 
 * Universal Time (UTC). The system records file times when applications create,
 * access, write, and make changes to files.
 */
void
smb_time_NT2local(uint64_t nsec, struct timespec *tsp)
{
	tsp->tv_sec = (long)(nsec / HUNDRED_NSEC_PER_SEC - DIFF1970TO1601);
    tsp->tv_nsec = 100 * (nsec % HUNDRED_NSEC_PER_SEC);
}

void
smb_time_local2NT(struct timespec *tsp, uint64_t *nsec, int fat_fstype)
{
	/* 
	 * Remember that FAT file systems only have a two second interval for 
	 * time. NTFS volumes do not have have this limitation, so only force 
	 * the two second interval on FAT File Systems.
	 */
	if (fat_fstype)
		*nsec = (((uint64_t)(tsp->tv_sec) & ~1) + DIFF1970TO1601)
                                                * HUNDRED_NSEC_PER_SEC;
    else {
        /*
         * the SMB protocol supports time resolution of 100-nanoseconds intervals
         * therefore, the two least significant digits are not needed
         * remove the first two digits and only keep the 100-nanoseconds intervals
         */
        *nsec = (((uint64_t)tsp->tv_sec + DIFF1970TO1601) * HUNDRED_NSEC_PER_SEC)
                + (tsp->tv_nsec / 100);
    }
}

int 
smb_fphelp(struct smbmount *smp, struct mbchain *mbp, struct smbnode *np,
		   int usingUnicode, size_t *lenp)
{
	struct smbnode **npstack;
	struct smbnode **npp;
	int i, error = 0;
    int add_slash = 1;
    int lock_count = 0;
	struct smbnode **lock_stack;
	struct smbnode **locked_npp;
    SMB_MALLOC_TYPE_COUNT(npstack, struct smbnode *, SMBFS_MAXPATHCOMP, Z_WAITOK);
    npp = &npstack[0];
    SMB_MALLOC_TYPE_COUNT(lock_stack, struct smbnode *, SMBFS_MAXPATHCOMP+1, Z_WAITOK); /* stream file adds one */
    locked_npp = &lock_stack[0];
    vnode_t par_vp = NULL;

    if (smp->sm_args.network_path) {
        if (!(SS_TO_SESSION(smp->sm_share)->session_flags & SMBV_SMB2)) {
            /* Only SMB 1 wants the beginning '\' */
            if (usingUnicode)
                error = mb_put_uint16le(mbp, '\\');
            else
                error = mb_put_uint8(mbp, '\\');
            if (!error && lenp)
                *lenp += (usingUnicode) ? 2 : 1;
        }
		/* We have a starting path, that has already been converted add it to the path */
		if (!error)
			error = mb_put_mem(mbp, (const char *)smp->sm_args.network_path,
							   smp->sm_args.network_path_len, MB_MSYSTEM);
		if (!error && lenp)
			*lenp += smp->sm_args.network_path_len;
	} else if (SS_TO_SESSION(smp->sm_share)->session_flags & SMBV_SMB2) {
        /* No starting path. SMB 2/3 does not want the beginning '\' */
        add_slash = 0;
    }
    
    lck_rw_lock_shared(&np->n_parent_rwlock);   /* do our own locking */
    *locked_npp++ = np;     /* Save node to be unlocked later */
    lock_count += 1;

	/* 
     * This is a stream file, skip it. We always use the stream parent for 
     * the lookup. We still lock it to keep it from changing.
     */
	if (np->n_flag & N_ISSTREAM) {
        par_vp = smbfs_smb_get_parent(np, 0);   /* do our own locking */
        if (par_vp != NULL) {
            np = VTOSMB(par_vp);
            lck_rw_lock_shared(&np->n_parent_rwlock);   /* do our own locking */
            *locked_npp++ = np;     /* Save node to be unlocked later */
            lock_count += 1;
        }
        else {
            if (np->n_parent_vid != 0) {
                /* Parent got recycled already? */
                SMBWARNING_LOCK(np, "Missing parent for <%s> \n", np->n_name);
                error = ENOENT;
                goto done;
            }
        }
    }

	i = 0;
    par_vp = smbfs_smb_get_parent(np, 0);   /* do our own locking */
    if ((par_vp == NULL) &&
        (np->n_parent_vid != 0)) {
        /* Parent got recycled already? */
        SMBWARNING_LOCK(np, "Missing parent for <%s> \n", np->n_name);
        error = ENOENT;
        goto done;
    }

	while (par_vp != NULL) {
		if (i++ == SMBFS_MAXPATHCOMP) {
            error = ENAMETOOLONG;
            vnode_put(par_vp);
            goto done;
        }
        
		*npp++ = np;            /* Save node to build a path later */
        np = VTOSMB(par_vp);

        lck_rw_lock_shared(&np->n_parent_rwlock);   /* do our own locking */
        *locked_npp++ = np;     /* Save node to be unlocked later */
        lock_count += 1;
        
        par_vp = smbfs_smb_get_parent(np, 0);   /* do our own locking */
        if ((par_vp == NULL) &&
            (np->n_parent_vid != 0)) {
            /* Parent got recycled already? */
            SMBWARNING_LOCK(np, "Missing parent for <%s> \n", np->n_name);
            error = ENOENT;
            goto done;
        }
	}

	while (i--) {
		np = *--npp;
        if (add_slash == 1) {
            if (usingUnicode)
                error = mb_put_uint16le(mbp, '\\');
            else
                error = mb_put_uint8(mbp, '\\');
            
            if (!error && lenp)
                *lenp += (usingUnicode) ? 2 : 1;
        }
        else {
            /* add slashes from now on */
            add_slash = 1;
        }
        
		if (error)
			break;
        
        lck_rw_lock_shared(&np->n_name_rwlock);
		error = smb_put_dmem(mbp, (char *)(np->n_name), np->n_nmlen,
							 UTF_SFM_CONVERSIONS, usingUnicode, lenp);
        lck_rw_unlock_shared(&np->n_name_rwlock);
        
		if (error)
			break;
	}

done:
    /* Unlock all the nodes */
    for (i = 0; i < lock_count; i++) {
        lck_rw_unlock_shared(&lock_stack[i]->n_parent_rwlock);

        if ((i != 0) &&
            (lock_stack[i]->n_vnode != NULL)) {
            /* First vnode was not fetched with smbfs_smb_get_parent */
            vnode_put(lock_stack[i]->n_vnode);
        }
    }
    if (npstack) {
        SMB_FREE_TYPE_COUNT(struct smbnode *, SMBFS_MAXPATHCOMP, npstack);
    }
    if (lock_stack) {
        SMB_FREE_TYPE_COUNT(struct smbnode *, SMBFS_MAXPATHCOMP+1, lock_stack);
    }

	return error;
}

int
smbfs_fullpath(struct mbchain *mbp, struct smbnode *dnp, const char *name, 
			   size_t *lenp, int name_flags, int usingUnicode, uint8_t sep)
{
	int error; 
	size_t len = 0;

	if (lenp) {
		len = *lenp;
		*lenp = 0;
	}
	if (usingUnicode) {
		error = mb_put_padbyte(mbp);
		if (error)
			return error;
	}
	if (dnp != NULL) {
		struct smbmount *smp = dnp->n_mount;
		
		error = smb_fphelp(smp, mbp, dnp, usingUnicode, lenp);
		if (error) {
			return error;
        }
        
		if (((smp->sm_args.network_path == NULL) &&
             (dnp->n_ino == smp->sm_root_ino)
             && !name)) {
            name = ""; /* to get one backslash below */
        }
	}
	if (name) {
		if (usingUnicode)
			error = mb_put_uint16le(mbp, sep);
		else
			error = mb_put_uint8(mbp, sep);
		if (!error && lenp)
			*lenp += (usingUnicode) ? 2 : 1;
		if (error)
			return error;
		error = smb_put_dmem(mbp, name, len, name_flags, usingUnicode, lenp);
		if (error)
			return error;
	}
	/* 
	 * Currently only NTCreateAndX uses the length field. It doesn't expect
	 * the name len to inlcude the null bytes, so leave those off.
	 */
	error = mb_put_uint8(mbp, 0);
	if (usingUnicode && error == 0) {
		error = mb_put_uint8(mbp, 0);
	}
	return error;
}

int
smbfs_fullpath_stream(struct mbchain *mbp, struct smbnode *dnp,
                      const char *namep, const char *strm_namep,
                      size_t *lenp, size_t strm_name_len, int name_flags,
                      int usingUnicode, uint8_t sep)
{
	int error; 
	size_t len = 0;
    
	if (lenp) {
		len = *lenp;
		*lenp = 0;
	}
    
	if (usingUnicode) {
		error = mb_put_padbyte(mbp);
		if (error)
			return error;
	}
    
	if (dnp != NULL) {
		struct smbmount *smp = dnp->n_mount;
		
		error = smb_fphelp(smp, mbp, dnp, usingUnicode, lenp);
		if (error) {
			return error;
        }
        
		if (((smp->sm_args.network_path == NULL) &&
             (dnp->n_ino == smp->sm_root_ino)
             && !namep)) {
			namep = ""; /* to get one backslash below */
        }
	}
    
	if (namep) {
		if (usingUnicode)
			error = mb_put_uint16le(mbp, sep);
		else
			error = mb_put_uint8(mbp, sep);
		if (!error && lenp)
			*lenp += (usingUnicode) ? 2 : 1;
		if (error)
			return error;
		error = smb_put_dmem(mbp, namep, len, name_flags, usingUnicode, lenp);
		if (error)
			return error;
	}

    if (strm_namep) {
		if (usingUnicode)
			error = mb_put_uint16le(mbp, ':');
		else
			error = mb_put_uint8(mbp, ':');
		if (!error && lenp)
			*lenp += (usingUnicode) ? 2 : 1;
		if (error)
			return error;
		error = smb_put_dmem(mbp, strm_namep, strm_name_len, name_flags, usingUnicode, lenp);
		if (error)
			return error;
	}

    /*
	 * Currently only NTCreateAndX uses the length field. It doesn't expect
	 * the name len to include the null bytes, so leave those off.
	 */
	error = mb_put_uint8(mbp, 0);
	if (usingUnicode && error == 0) {
		error = mb_put_uint8(mbp, 0);
	}
    
	return error;
}

/*
 * They want the mount to start at some path offest. Take the path they gave us 
 * and create a buffer that can be added to the front of every path we send across 
 * the network. This new buffer will already be convert to a network style string.
 */
void 
smbfs_create_start_path(struct smbmount *smp, struct smb_mount_args *args, 
						int usingUnicode)
{
	int error;
	
	/* Just in case someone sends us a bad string */
	args->path[MAXPATHLEN - 1] = 0;
	
	/* 
	 * Path length cannot be bigger than MAXPATHLEN and cannot contain the 
	 * null byte 
	 */
	args->path_len = (args->path_len < MAXPATHLEN) ? args->path_len : (MAXPATHLEN - 1);
	
	/* path should never end with a slash */
	if (args->path[args->path_len - 1] == '/') {
		args->path_len -= 1;
		args->path[args->path_len] = 0;
	}
	
	/* Need to save the submount path for Resolve ID */
	smp->sm_args.local_path_len = args->path_len;
    SMB_MALLOC_DATA(smp->sm_args.local_path, smp->sm_args.local_path_len + 1, Z_WAITOK_ZERO);
	if (smp->sm_args.local_path == NULL) {
		/* Should never happen */
		smp->sm_args.local_path_len = 0;
		return;
	}
	memcpy(smp->sm_args.local_path, args->path, args->path_len);

	/* Create network path version of the submount path (assume max len) */
	smp->sm_args.network_path_len = (args->path_len * 2) + 2;
    SMB_MALLOC_DATA(smp->sm_args.network_path, smp->sm_args.network_path_len, Z_WAITOK);
	if (smp->sm_args.network_path == NULL) {
		/* Should never happen */
        SMB_FREE_DATA(smp->sm_args.local_path, smp->sm_args.local_path_len + 1);
		smp->sm_args.local_path_len = 0;

		smp->sm_args.network_path_len = 0;
		return;
	}
	
	/* 
	 * Convert it to a network style path, the convert routine will set the 
	 * precomposed flag 
	 */
	error = smb_convert_path_to_network(args->path, sizeof(args->path), 
										smp->sm_args.network_path,
										&smp->sm_args.network_path_len,
										'\\', SMB_UTF_SFM_CONVERSIONS,
										usingUnicode);
	if (error || (smp->sm_args.network_path_len == 0)) {
		SMBDEBUG("Deep Path Failed %d\n", error);
		
        SMB_FREE_DATA(smp->sm_args.local_path, smp->sm_args.local_path_len + 1);
		smp->sm_args.local_path_len = 0;
		
        SMB_FREE_DATA(smp->sm_args.network_path, smp->sm_args.network_path_len);
		smp->sm_args.network_path_len = 0;
	}
}

/*
 * Converts a network name to a local UTF-8 name.
 *
 * Returns a UTF-8 string or NULL.
 *	ntwrk_name - either UTF-16 or ASCII Code Page
 *	nmlen - on input the length of the network name
 *			on output the length of the UTF-8 name
 * NOTE:
 *	This routine will not free the ntwrk_name.
 */
char *
smbfs_ntwrkname_tolocal(const char *ntwrk_name, size_t *nmlen, size_t *allocsize, int usingUnicode)
{
	char *dst, *odst = NULL;
	size_t inlen, outlen, length;

	if (!nmlen || (*nmlen == 0))
		return NULL;
	/*
	 * In Mac OS X the local name can be larger and in-place conversions are
	 * not supported.
	 * So for UNICODE we can have up to 9 bytes for every UTF16 bytes. So 
	 * normally you would need 3 UTF8 bytes for every UTF16 character point, but 
	 * we also need to deal with preompose/decompose character sets, so make 
	 * sure the buffer is big enough to hanlde these case. That would be nine
	 * times the UTF16 length in bytes.
	 * For code pages cases we only need a buffer 3 times as large.
	 */
	if (usingUnicode) {
		length = MIN(*nmlen * 9, SMB_MAXPKTLEN);
	} else {
		length = MIN(*nmlen * 3, SMB_MAXPKTLEN);
	}
    *allocsize = length + 1;
    SMB_MALLOC_DATA(dst, *allocsize, Z_WAITOK_ZERO);
	outlen = length;
	inlen = *nmlen;
	odst = dst;
	(void)smb_convert_from_network( &ntwrk_name, &inlen, &dst, &outlen, 
								   UTF_SFM_CONVERSIONS, usingUnicode);
	*nmlen = length - outlen;
	/* 
	 * Always make sure its null terminate, remember we allocated an extra 
	 * byte so this is always safe. Should we resize the buffer here?
	 */
	odst[*nmlen] = 0;
	return odst;
}

/*
 * Given a smb mount point take a reference on the associate share and then 
 * return a pointer to that share.
 */
struct smb_share *
smb_get_share_with_reference(struct smbmount *smp)
{
	struct smb_share *share = NULL;

	lck_rw_lock_shared(&smp->sm_rw_sharelock);
	share = smp->sm_share;
	KASSERT(share, "smp->sm_share == NULL");
	smb_share_ref(share);
	lck_rw_unlock_shared(&smp->sm_rw_sharelock);
	return share;
}

int
smbfs_is_dir(struct smbnode *np)
{
    int is_dir = 0;

    /* Is it a directory? */
    if (np->n_vnode) {
        /* If we have a vnode, then check vnode type */
        if (vnode_isdir(np->n_vnode)) {
            is_dir = 1;
        }
    }
    else {
        /*
         * No vnode, so have to rely on n_dosattr instead.
         * If its a reparse point, then it is NOT a dir!
         */
        if ((np->n_dosattr & SMB_EFA_DIRECTORY) &&
            !(np->n_dosattr & SMB_EFA_REPARSE_POINT)) {
            is_dir = 1;
        }
    }

    return(is_dir);
}
