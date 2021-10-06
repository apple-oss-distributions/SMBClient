/*
 * Copyright (c) 2010-2017 Apple Inc. All rights reserved.
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
#ifndef _SMBLIB_PREFERENCES_H_
#define _SMBLIB_PREFERENCES_H_

#include <netsmb/smb_conn.h>

#define DefaultNetBIOSResolverTimeout	1

/* Shouldn't this be handle by gss */
enum smb_min_auth {
	SMB_MINAUTH = 0,			/* minimum auth level for connection */
	SMB_MINAUTH_LM = 1,			/* No plaintext passwords */
	SMB_MINAUTH_NTLM = 2,		/* don't send LM reply? */
	SMB_MINAUTH_NTLMV2 = 3,		/* don't fall back to NTLMv1 */
	SMB_MINAUTH_KERBEROS = 4	/* don't do NTLMv1 or NTLMv2 */
};

struct smb_prefs {
    CFStringRef         LocalNetBIOSName;
    CFArrayRef          WINSAddresses;
    int32_t             NetBIOSResolverTimeout;
    CFStringEncoding    WinCodePage;
    uint32_t            tryBothPorts;
    uint16_t            tcp_port;
    int32_t             KernelLogLevel;
    enum smb_min_auth   minAuthAllowed;
    int32_t             altflags;
    CFStringRef         NetBIOSDNSName;
    int32_t             protocol_version_map;
    uint32_t            lanman_on;
    uint32_t            signing_required;
    int32_t             signing_req_versions;
    int32_t             max_resp_timeout;
    int32_t             ip_QoS;

    int32_t             dir_cache_async_cnt;
    int32_t             dir_cache_max;
    int32_t             dir_cache_min;
    int32_t             max_dirs_cached;
    int32_t             max_dir_entries_cached;

    uint32_t            no_DNS_match;
    uint32_t            try_netBIOS_before_DNS;

    int32_t             max_read_size;
    int32_t             max_write_size;

    int32_t             mc_max_channels;
    int32_t             mc_max_rss_channels;
    uint32_t            mc_client_if_blacklist[kClientIfBlacklistMaxLen];
    uint32_t            mc_client_if_blacklist_len;
};

void getDefaultPreferences(struct smb_prefs *prefs);
void setWINSAddress(struct smb_prefs *prefs, const char *winsAddress, int count);
void releasePreferenceInfo(struct smb_prefs *prefs);
void readPreferences(struct smb_prefs *prefs, char *serverName, char *shareName, 
					 int noUserPrefs, int resetPrefs);
CFStringEncoding getPrefsCodePage( void );

#endif // _SMBLIB_PREFERENCES_H_
