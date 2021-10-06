/*
 * Copyright (c) 2000-2001, Boris Popov
 * All rights reserved.
 *
 * Portions Copyright (C) 2001 - 2013 Apple Inc. All rights reserved.
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
 */
#ifndef _FS_SMBFS_NODE_H_
#define _FS_SMBFS_NODE_H_

/*
 * OS X semantics expect that node id of 2 is the root of the share.
 * If File IDs are supported by the SMB 2/3 server, then we need to return 2
 * for the root vnode File ID . If some other item on the server has the 
 * File ID of 2, then we return the actual root File ID instead.
 * When vget is supported, then if they ask for File ID of 2, we return the 
 * root vnode.  If they ask for File ID that matches the actual root File ID,
 * then we return the vnode that has the File ID of 2 on the server.
 */
#define	SMBFS_ROOT_INO		2	/* just like in UFS */
#define	SMBFS_ROOT_PAR_INO	1	/* Maps to root vnode just like SMBFS_ROOT_INO */

/* Bits for smbnode.n_flag */
#define	NREFPARENT		0x00001	/* node holds parent from recycling */
#define	N_ISSTREAM		0x00002	/* Node is a stream */
#define	N_ISRSRCFRK		0x00004	/* Special stream node! */
#define	NISMAPPED		0x00008 /* This file has been mmapped */
#define NNEEDS_FLUSH	0x00010 /* Need to flush the file */
#define	NNEEDS_EOF_SET	0x00020	/* Need to set the eof, ignore the eof from the server */
#define	NATTRCHANGED	0x00040	/* Did we change the size of the file, clear cache on close */
#define	NALLOC			0x00080	/* being created */
#define	NWALLOC			0x00100	/* awaiting creation */
#define	NTRANSIT		0x00200	/* being reclaimed */
#define	NWTRANSIT		0x00400	/* awaiting reclaim */
#define	NDELETEONCLOSE	0x00800	/* We need to delete this item on close */
#define	NMARKEDFORDLETE	0x01000	/* This item will has been marked for deletion */
#define	NNEGNCENTRIES	0x02000	/* Directory has negative name cache entries */
#define NWINDOWSYMLNK	0x04000 /* This is a Conrad/Steve Window's symbolic links */
#define N_POLLNOTIFY	0x08000 /* Change notify is not support, poll */
#define NO_EXTENDEDOPEN 0x10000 /* The server doesn't support the extended open reply */
#define NHAS_POSIXMODES 0x20000 /* This node has a Windows NFS ACE that contains posix modes */

#define UNKNOWNUID ((uid_t)99)
#define UNKNOWNGID ((gid_t)99)

struct smbfs_fctx;

enum smbfslocktype {SMBFS_SHARED_LOCK = 1, SMBFS_EXCLUSIVE_LOCK = 2, SMBFS_RECLAIM_LOCK = 3};

/* Used in reconnect for open files. Look at openState. */
#define kNeedRevoke	0x01
#define kNeedReopen	0x02
#define kInReopen   0x04  // Reopen in progress, don't update metadata

enum {
    kAnyMatch = 1,
    kCheckDenyOrLocks = 2,
    kExactMatch = 3,
    kPreflightOpen = 4
};

/* Flags for smbfs_smb_get_parent() */
enum {
    kShareLock = 1
};

/* Carbon Read/Write and Deny bits */
enum {
	kAccessRead = 0x01,
	kAccessWrite = 0x02,
	kDenyRead = 0x10,
	kDenyWrite = 0x20,
	kOpenMask = 0x03,
	kDenyMask = 0x30,
	kAccessMask = 0x33
};

struct ByteRangeLockEntry {
	int64_t		offset;
	int64_t		length;
	uint32_t	lck_pid;
	struct ByteRangeLockEntry *next;
};

/* Used for Open Deny */
struct fileRefEntry {
    uint32_t        refcnt;     /* open file reference count */
    uint32_t        mmapped;    /* This entry has been mmaped */
    pid_t           p_pid;      /* proc that did the open */
    SMBFID          fid;        /* file handle, SMB 1 fid in volatile */
    uint16_t        accessMode; /* access mode for this open */
    uint32_t        rights;     /* nt granted rights */
    struct proc     *proc;      /* used in cluster IO strategy function */
    struct ByteRangeLockEntry *lockList;
    struct smb2_durable_handle *dur_handlep;
    struct fileRefEntry	*next;
};

/* enum cache flags */
enum {
    kDirCacheComplete = 0x01,   /* Entire dir is cached */
    kDirCachePartial = 0x02,    /* Max allowable number of entries cached */
    kDirCacheDirty = 0x04       /* Needs Meta Data and/or Finder Info */
};

struct smb_enum_cache {
    u_int64_t       flags;
    u_int32_t       chg_cnt;         /* copy of dirchangecnt when dir cached */
    off_t           count;           /* dir enum cache number of entries */
    off_t           offset;          /* last cache offset */
    off_t           start_offset;    /* first entry cache offset */
    time_t			timer;           /* dir enum cache timer */
    struct cached_dir_entry	*list;   /* dir enum cache */
};

struct smb_dir_cookie {
    uint64_t        key;
    struct timespec last_used;
    uint64_t        resume_offset;
    uint64_t        resume_node_id;
    char            resume_name[PATH_MAX];
};

#define kSMBDirCookieMaxCnt 25

struct smb_open_dir {
	uint32_t		refcnt;
	uint32_t		kq_refcnt;
	SMBFID          fid;			/* directory handle, SMB 1 fid in volatile */
    struct smbfs_fctx *fctx;        /* ff context */
    struct smbfs_fctx *rdir_fctx;   /* ff context for readdir */
	void			*nextEntry;		/* directory entry that didn't fit */
	int32_t			nextEntryLen;	/* size of directory entry that didn't fit */
	int32_t			nextEntryFlags; /* flags that the next entry was create with */
    off_t           offset;         /* last ff offset */
    off_t           rdir_offset;    /* last ff offset for readdir */
	uint32_t		needReopen;		/* Need to reopen the notification */
	uint32_t		needsUpdate;
    struct smb_enum_cache main_cache;           /* main dir cache */
    struct smb_enum_cache overflow_cache;       /* overflow dir cache */
	lck_mtx_t		enum_cache_list_lock;       /* dir enum list lock */
	u_int32_t       dirchangecnt;	/* changes each insert/delete. used by readdirattr */
	struct smb2_durable_handle dur_handle;	/* stores the dir lease */
    uint64_t        cookie_curr_key; /* Can never be zero */
    lck_mtx_t       cookie_lock;    /* smb_dir_cookie array lock */
    struct smb_dir_cookie cookies[kSMBDirCookieMaxCnt];
};

struct smb_open_file {
	int32_t         refcnt;		/* open file reference count */
	SMBFID          fid;		/* file handle, SMB 1 fid in volatile */
	uint32_t		rights;		/* nt granted rights */
	uint16_t		accessMode;	/* access mode used when opening  */
	uint32_t		mmapMode;	/* The mode we used when opening from mmap */
	int32_t			needClose;	/* we opened it in the read call */
	int32_t			openRWCnt;	/* number of rw opens */
	int32_t			openRCnt;	/* number of r opens */
	int32_t			openWCnt;	/* number of w opens */
	int32_t			openTotalWriteCnt; /* nbr of w opens (shared and nonshared) */
	int32_t			clusterCloseError;	/* Error to be return on user close */
	uint32_t		openState;	/* Do we need to revoke or reopen the file */
	lck_mtx_t		openStateLock;	/* Locks the openState */
	lck_mtx_t		clusterWriteLock; /* Used for cluster writes */
	lck_mtx_t		openDenyListLock;	/* Locks the open deny list */
	struct fileRefEntry	*openDenyList;
	struct smbfs_flock	*smbflock;	/*  Our flock structure */
	struct smb2_durable_handle dur_handle;	/* stores the shared file lease <future work> */
};

struct smbnode {
	lck_rw_t			n_rwlock;	
	void *				n_lastvop;	/* tracks last operation that locked the smbnode */
	void *				n_activation;
	uint32_t			n_lockState;	/* current lock state */
	uint32_t			n_flag;
    vnode_t             n_parent_vnode;
    uint32_t            n_parent_vid;
	vnode_t				n_vnode;
	struct smbmount		*n_mount;
	time_t				attribute_cache_timer;	/* attributes (MetaData) cache time */
	struct timespec		n_last_meta_set_time; /* last time we set attributes (MetaData) */
	struct timespec		n_crtime;	/* create time */
	struct timespec		n_mtime;	/* modify time */
	struct timespec		n_atime;	/* last access time */
	struct timespec		n_chtime;	/* change time */
	struct timespec		n_sizetime;     /* last time we set size */
	struct timespec		n_rename_time;  /* last rename time */
	u_quad_t			n_size;         /* stream size */
	uint8_t				waitOnClusterWrite;
	u_quad_t			n_data_alloc;	/* stream allocation size */
	int					n_dosattr;
	uint32_t			n_flags_mask;	/* When doing unix extensions the va_flags mask */
	uid_t				n_uid;
	gid_t				n_gid;
	mode_t				n_mode;
	mode_t				create_va_mode;
	uid_t				n_nfs_uid;
	gid_t				n_nfs_gid;
	int					set_create_va_mode;
	time_t				finfo_cache_timer;	/* finder info cache timer, only used by the data node */
	uint8_t				finfo[FINDERINFOSIZE];	/* finder info , only used by the data node */
	time_t				rfrk_cache_timer;		/* resource stream size cache timer, only used by the data node */
	u_quad_t			rfrk_size;		/* resource stream size, only used by the data node */
	u_quad_t			rfrk_alloc_size;/* resource stream alloc size */
	lck_mtx_t			rfrkMetaLock;	/* Locks the resource size and resource cache timer */
	uint64_t			n_ino;
	uint64_t			n_nlinks;		/* Currently only supported when using the new UNIX Extensions */
    SInt32              n_child_refcnt; /* Each child node holds a refcnt */
	union {
		struct smb_open_dir	dir;
		struct smb_open_file file;
	}open_type;
	void				*acl_cache_data;
	time_t				acl_cache_timer;
	int					acl_error;
	size_t				acl_cache_len;
	lck_mtx_t			f_ACLCacheLock;     /* Locks the ACL Cache */
	lck_rw_t			n_name_rwlock;      /* Read/Write lock for n_name */
	lck_rw_t			n_parent_rwlock;    /* Read/Write lock for n_parent_vid */
	char				*n_name;	/* node's file or directory name */
	size_t				n_nmlen;	/* node's name length */
	size_t				n_snmlen;	/* if a stream then the legnth of the stream name */
	char				*n_sname;	/* if a stream then the the name of the stream */
	LIST_ENTRY(smbnode)	n_hash;
	uint32_t			maxAccessRights;
	struct timespec		maxAccessRightChTime;	/* change time */
	uint32_t			n_reparse_tag;
	uint16_t			n_fstatus;				/* Does the node have any named streams */
	char				*n_symlink_target;
	size_t				n_symlink_target_len;
	time_t				n_symlink_cache_timer;
	struct timespec		n_last_write_time;
	uint64_t			n_lease_key_hi;			/* Used for Dir Lease or shared FID */
	uint64_t			n_lease_key_low;
	uint16_t			n_epoch;				/* lease epoch */

    struct timespec     n_last_close_mtime;     /* modify time on last close */
    u_quad_t            n_last_close_size;      /* data size on last close */

    struct smb_vnode_attr n_hifi_attrs;     /* Cached hifi attributes from server */
};

/* Directory items */
#define d_refcnt open_type.dir.refcnt
#define d_kqrefcnt open_type.dir.kq_refcnt
#define d_fctx open_type.dir.fctx
#define d_rdir_fctx open_type.dir.rdir_fctx
#define d_nextEntry open_type.dir.nextEntry
#define d_nextEntryFlags open_type.dir.nextEntryFlags
#define d_nextEntryLen open_type.dir.nextEntryLen
#define d_offset open_type.dir.offset
#define d_rdir_offset open_type.dir.rdir_offset
#define d_needReopen open_type.dir.needReopen
#define d_fid open_type.dir.fid
#define d_needsUpdate open_type.dir.needsUpdate
#define d_changecnt open_type.dir.dirchangecnt

#define d_main_cache open_type.dir.main_cache
#define d_overflow_cache open_type.dir.overflow_cache
#define d_enum_cache_list_lock open_type.dir.enum_cache_list_lock
#define d_dur_handle open_type.dir.dur_handle
#define d_cookie_lock open_type.dir.cookie_lock
#define d_cookies open_type.dir.cookies
#define d_cookie_cur_key open_type.dir.cookie_curr_key

/* File items */
#define f_refcnt open_type.file.refcnt
#define f_fid open_type.file.fid
#define f_rights open_type.file.rights
#define f_accessMode open_type.file.accessMode
#define f_mmapMode open_type.file.mmapMode
#define f_needClose open_type.file.needClose
#define f_openRWCnt open_type.file.openRWCnt
#define f_openRCnt open_type.file.openRCnt
#define f_openWCnt open_type.file.openWCnt
#define f_openTotalWCnt open_type.file.openTotalWriteCnt
#define f_openDenyList open_type.file.openDenyList
#define f_smbflock open_type.file.smbflock
#define f_openState open_type.file.openState
#define f_openStateLock open_type.file.openStateLock
#define f_clusterWriteLock open_type.file.clusterWriteLock
#define f_openDenyListLock open_type.file.openDenyListLock
#define f_clusterCloseError open_type.file.clusterCloseError
#define f_dur_handle open_type.file.dur_handle

/* Attribute cache timeouts in seconds */
#define	SMB_MINATTRTIMO 2
#define	SMB_MAXATTRTIMO 30

/*
 * Determine attrtimeo. It will be something between SMB_MINATTRTIMO and
 * SMB_MAXATTRTIMO where recently modified files have a short timeout
 * and files that haven't been modified in a long time have a long
 * timeout. This is the same algorithm used by NFS.
 */
#define SMB_CACHE_TIME(ts, np, attrtimeo) { \
	nanotime(&ts);	\
	attrtimeo = (ts.tv_sec - np->n_mtime.tv_sec) / 10; \
	if (attrtimeo < SMB_MINATTRTIMO)	\
		attrtimeo = SMB_MINATTRTIMO;	\
	else if (attrtimeo > SMB_MAXATTRTIMO) \
        attrtimeo = SMB_MAXATTRTIMO; \
	nanouptime(&ts);	\
}

#define SMB_DIR_CACHE_TIME(ts, np, attrtimeo, max_to, min_to) { \
    nanotime(&ts);	\
    attrtimeo = (ts.tv_sec - np->n_mtime.tv_sec) / 10; \
    if (attrtimeo < min_to)	\
        attrtimeo = min_to;	\
    else if (attrtimeo > max_to) \
        attrtimeo = max_to; \
    nanouptime(&ts);	\
}

#define VTOSMB(vp)	((struct smbnode *)vnode_fsnode(vp))
#define SMBTOV(np)	((vnode_t )(np)->n_vnode)

/* smbfs_nget flags */
typedef enum _SMBFS_NGET_FLAGS
{
    SMBFS_NGET_CREATE_VNODE = 0x0001,
    SMBFS_NGET_LOOKUP_ONLY = 0x0002,
    SMBFS_NGET_NO_CACHE_UPDATE = 0x0004
} _SMBFS_NGET_FLAGS;

/* smb_get_uid_gid_mode flags */
typedef enum _SMBFS_GET_UGM_FLAGS
{
    SMBFS_GET_UGM_IS_DIR = 0x0001,
    SMBFS_GET_UGM_REMOVE_POSIX_MODES = 0x0002
} _SMBFS_GET_UGM_FLAGS;

extern lck_attr_t *smbfs_lock_attr;
extern lck_grp_t *smbfs_mutex_group;
extern lck_grp_t *smbfs_rwlock_group;

struct rename_lock_list {
	struct smbnode *np;
	UInt32 is_locked;
	struct rename_lock_list *next;
};

struct smbfattr;

/* Global Lease Hash functions */
struct smb_lease {
	LIST_ENTRY(smb_lease) lease_hash;
	vnode_t             vnode;
	uint32_t            vid;
	SMBFID				fid;			/* to find File Ref */
};

void smb2_lease_hash_add(vnode_t vp, SMBFID fid, uint64_t lease_key_hi, uint64_t lease_key_low);
struct smb_lease *smb2_lease_hash_get(uint64_t lease_key_hi, uint64_t lease_key_low);
void smb2_lease_hash_remove(vnode_t vp, struct smb_lease *in_leasep,
							uint64_t lease_key_hi, uint64_t lease_key_low);

int smbnode_lock(struct smbnode *np, enum smbfslocktype);
int smbnode_trylock(struct smbnode *np, enum smbfslocktype locktype);
int smbnode_lockpair(struct smbnode *np1, struct smbnode *np2, enum smbfslocktype);
void smbnode_unlock(struct smbnode *np);
void smbnode_unlockpair(struct smbnode *np1, struct smbnode *np2);
uint64_t smbfs_hash(struct smb_share *share, uint64_t ino,
                    const char *name, size_t nmlen);
void smb_vhashrem (struct smbnode *np);
void smb_vhashadd(struct smbnode *np, uint64_t hashval);
int smbfs_nget(struct smb_share *share, struct mount *mp,
               vnode_t dvp, const char *name, size_t nmlen,
               struct smbfattr *fap, vnode_t *vpp,
               uint32_t cnflags, uint32_t flags,
               vfs_context_t context);
vnode_t smbfs_find_vgetstrm(struct smbmount *smp, struct smbnode *np, const char *sname, 
							size_t maxfilenamelen);
int smbfs_vgetstrm(struct smb_share *share, struct smbmount *smp, vnode_t vp, 
				   vnode_t *svpp, struct smbfattr *fap, const char *sname);
int smb_get_rsrcfrk_size(struct smb_share *share, vnode_t vp, vfs_context_t context);
vnode_t smb_update_rsrc_and_getparent(vnode_t vp, int setsize);
int smb_check_posix_access(vfs_context_t context, struct smbnode * np, 
						   mode_t rq_mode);
Boolean node_isimmutable(struct smb_share *share, vnode_t vp, struct smbfattr *fap);
void smbfs_attr_cacheenter(struct smb_share *share, vnode_t vp, struct smbfattr *fap, 
						   int UpdateResourceParent, vfs_context_t context);
int smbfs_attr_cachelookup(struct smb_share *share, vnode_t vp, struct vnode_attr *va, 
						   vfs_context_t context, int useCacheDataOnly);
void smbfs_attr_touchdir(struct smbnode *dnp, int fatShare);

int smbfsIsCacheable(vnode_t vp);
void smbfs_setsize(vnode_t vp, off_t size);
int smbfs_update_size(struct smbnode *np, struct timespec * reqtime,
                      u_quad_t new_size, struct smbfattr *fap);
int smbfs_update_name_par(struct smb_share *share, vnode_t dvp, vnode_t vp,
                          struct timespec *reqtime,
                          const char *new_name, size_t name_len);

int FindByteRangeLockEntry(struct fileRefEntry *fndEntry, int64_t offset, 
						int64_t length, uint32_t lck_pid);
void AddRemoveByteRangeLockEntry(struct fileRefEntry *fndEntry, int64_t offset, 
							  int64_t length, int8_t unLock, uint32_t lck_pid);
void AddFileRef(vnode_t vp, struct proc *p, uint16_t accessMode, uint32_t rights,
                SMBFID fid, struct smb2_durable_handle *dur_handle, struct fileRefEntry **fndEntry);
void CloseDeferredFileRefs(vnode_t vp, const char *reason, uint32_t check_time, vfs_context_t context);
int32_t FindFileEntryByFID(vnode_t vp, SMBFID fid, struct fileRefEntry **fndEntry);
int32_t FindFileEntryByLease(vnode_t vp, struct smb_lease *leasep, struct fileRefEntry **fndEntry);
int32_t FindMappedFileRef(vnode_t vp, struct fileRefEntry **fndEntry, SMBFID *fid);
int32_t FindFileRef(vnode_t vp, proc_t p, uint16_t accessMode, int32_t flags,
                    int64_t offset, int64_t length, 
                    struct fileRefEntry **fndEntry, 
                    SMBFID *fid);
void RemoveFileRef(vnode_t vp, struct fileRefEntry *inEntry, int is_locked);
void smb_get_uid_gid_mode(struct smb_share *share, struct smbmount *smp,
                          struct smbfattr *fap, uint32_t flags,
                          uid_t *uid, gid_t *gid, mode_t *mode,
                          uint32_t max_access);

/* smbfs_io.c prototypes */
int smbfs_readvdir(vnode_t vp, uio_t uio, int flags, int32_t *numdirent,
                   vfs_context_t context);
int smbfs_0extend(struct smb_share *share, SMBFID fid, u_quad_t from,
                  u_quad_t to, int ioflag, vfs_context_t context);
int smbfs_doread(struct smb_share *share, off_t endOfFile, uio_t uiop,
                 SMBFID fid, vfs_context_t context);
int smbfs_dowrite(struct smb_share *share, off_t endOfFile, uio_t uiop, 
				  SMBFID fid, int ioflag, vfs_context_t context);
int smbfs_reconnect(struct smbmount *smp);
int32_t smbfs_IObusy(struct smbmount *smp);
void smbfs_ClearChildren(struct smbmount *smp, struct smbnode * parent);
void smbfs_CloseChildren(struct smb_share *share,
                         struct smbnode *parent,
                         u_int32_t need_lock,
						 struct rename_lock_list **rename_child_listp,
                         vfs_context_t context);
int16_t smbfs_get_epoch_delta(uint16_t server_epoch, uint16_t file_epoch);
int smbfs_handle_lease_break(struct lease_rq *lease_rqp, vfs_context_t context);
int smbfs_handle_dir_lease_break(struct lease_rq *lease_rqp);
#define smb_ubc_getsize(v) (vnode_vtype(v) == VREG ? ubc_getsize(v) : (off_t)0)

#endif /* _FS_SMBFS_NODE_H_ */
