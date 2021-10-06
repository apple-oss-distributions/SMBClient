/*
 * Copyright (c) 2001 - 2020 Apple Inc. All rights reserved.
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
#ifndef __COMMON_H__
#define __COMMON_H__

#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFArray.h>

#ifdef __cplusplus
extern "C" {
#endif
	
#define iprintf(ident,args...)	do { printf("%-" # ident "s", ""); \
				printf(args);}while(0)

extern int verbose;

enum OutputFormat { None = 0, Json = 1 };

int  cmd_lookup(int argc, char *argv[]);
int  cmd_status(int argc, char *argv[]);
int  cmd_view(int argc, char *argv[]);
int  cmd_dfs(int argc, char *argv[]);
int  cmd_identity(int argc, char *argv[]);
int  cmd_statshares(int argc, char *argv[]);
int  cmd_multichannel(int argc, char *argv[]);
int  cmd_snapshot(int argc, char *argv[]);
void lookup_usage(void);
void status_usage(void);
void view_usage(void);
void dfs_usage(void);
void identity_usage(void);
void ntstatus_to_err(NTSTATUS status);
void statshares_usage(void);
void multichannel_usage(void);
void snapshot_usage(void);
struct statfs *smb_getfsstat(int *fs_cnt);
CFArrayRef createShareArrayFromShareDictionary(CFDictionaryRef shareDict);
char * get_share_name(const char *name);
/*
 * Allocate a buffer and then use CFStringGetCString to copy the c-style string
 * into the buffer. The calling routine needs to free the buffer when done.
 */
char *CStringCreateWithCFString(CFStringRef inStr);
	
#ifdef __cplusplus
} // extern "C"
#endif

#endif // __COMMON_H__
