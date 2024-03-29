/*
 * Copyright (c) 2006 - 2011 Apple Inc. All rights reserved.
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

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServicesPriv.h>
#include <netsmb/smb_lib.h>
#include <charsets.h>
#include <parse_url.h>
#include <netsmb/smb_conn.h>
#include <NetFS/NetFS.h>
#include <NetFS/NetFSUtilPrivate.h>
#include <netsmb/netbios.h>
#include <netsmb/nb_lib.h>

#define CIFS_SCHEME_LEN 5
#define SMB_SCHEME_LEN 4

static void LogCFString(CFStringRef theString, const char *debugstr, const char * func, int lineNum)
{
	char prntstr[1024];
	
	if (theString == NULL)
		return;
	CFStringGetCString(theString, prntstr, 1024, kCFStringEncodingUTF8);
	os_log_debug(OS_LOG_DEFAULT, "%s-line:%d %s = %s", func, lineNum, debugstr, prntstr);
}

#ifdef SMB_DEBUG
#define DebugLogCFString LogCFString
#else // SMB_DEBUG
#define DebugLogCFString(theString, debugstr, func, lineNum)
#endif // SMB_DEBUG

/*
 * Test to see if we have the same url string
 */
int isUrlStringEqual(CFURLRef url1, CFURLRef url2)
{
	if ((url1 == NULL) || (url2 == NULL)) {
		return FALSE;	/* Never match null URLs */
	}
	
	if (CFStringCompare(CFURLGetString(url1), CFURLGetString(url1), 
						kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
		return TRUE;
	}
	return FALSE;
}

/*
 * Allocate a buffer and then use CFStringGetCString to copy the c-style string 
 * into the buffer. The calling routine needs to free the buffer when done.
 * This routine needs a new name.
 */
char *CStringCreateWithCFString(CFStringRef inStr)
{
	CFIndex maxLen;
	char *str;
	
    if(inStr == NULL)
        return NULL;
	maxLen = CFStringGetMaximumSizeForEncoding(CFStringGetLength(inStr), 
											   kCFStringEncodingUTF8) + 1;
	str = malloc(maxLen);
	if (!str) {
		os_log_error(OS_LOG_DEFAULT, "%s malloc failed, syserr = %s",
					 __FUNCTION__,strerror(ENOMEM));
		return NULL;
	}
	CFStringGetCString(inStr, str, maxLen, kCFStringEncodingUTF8);
	return str;
}	

/*
 * See if this is a cifs or smb scheme
 *
 * RETURN VALUES:
 *	0	- No Scheme, could still be our scheme
 *	4	- SMB scheme, also the length of smb scheme field.
 *	5	- CIFS scheme, also the length of cifs scheme field.
 *	-1	- Unknown scheme, should be treated as an error.
 */
static int SMBSchemeLength(CFURLRef url)
{
	int len = 0;
	CFStringRef scheme = CFURLCopyScheme (url);

	if (scheme == NULL)
		return 0;
	
	if ( kCFCompareEqualTo == CFStringCompare (scheme, CFSTR("smb"), kCFCompareCaseInsensitive) ) 
		len = SMB_SCHEME_LEN;	/* Length of "smb:" */
	else if ( kCFCompareEqualTo == CFStringCompare (scheme, CFSTR("cifs"), kCFCompareCaseInsensitive) ) 
		len = CIFS_SCHEME_LEN;	/* Length of "cifs:" */
	else
		len = -1;
	CFRelease(scheme);
	return len;
}

/* 
 * This routine will percent escape out the input string by making a copy.
 * The CFURLCreateStringByReplacingPercentEscapes routine will either return a
 * copy or take a retain on the original string.
 *
 * NOTE: We always use kCFStringEncodingUTF8 and kCFAllocatorDefault.
 * NOTE: CFURLCreateStringByReplacingPercentEscapes can return NULL!
 */
static void CreateStringByReplacingPercentEscapesUTF8(CFStringRef *inOutStr, CFStringRef LeaveEscaped)
{
	CFStringRef inStr = (inOutStr) ? *inOutStr : NULL;
	
	if (!inStr)	/* Just a safety check */
		return;
	
	*inOutStr = CFURLCreateStringByReplacingPercentEscapes(NULL, inStr, LeaveEscaped);
	CFRelease(inStr);	/* We always want to release the inStr */
}

/* 
 * This routine will  percent escape the input string by making a copy. The CreateStringByAddingPercentEscapesUTF8
 * routine will either return a copy or take a retain on the original string. If doRelease is set then we do
 * a release on the inStr.
 *
 * NOTE:  We always use kCFStringEncodingUTF8 and kCFAllocatorDefault.
 */
static void CreateStringByAddingPercentEscapesUTF8(CFStringRef *inOutStr, CFStringRef leaveUnescaped, CFStringRef toBeEscaped, Boolean doRelease)
{
	CFStringRef inStr = (inOutStr) ? *inOutStr : NULL;
	
	if (!inStr)	/* Just a safety check */
		return;
		
    /* This is the only C option for escaping URL's, so we have to keep it. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	*inOutStr = CFURLCreateStringByAddingPercentEscapes(NULL, inStr, leaveUnescaped, toBeEscaped, kCFStringEncodingUTF8);
#pragma clang diagnostic pop
	if (doRelease)
		CFRelease(inStr);
}

static CFArrayRef CreateWrkgrpUserArrayFromCFStringRef(CFStringRef inString, CFStringRef separatorString)
{
	CFArrayRef	userArray = NULL;

	userArray = CFStringCreateArrayBySeparatingStrings(kCFAllocatorDefault, inString, separatorString);
	/* 
	 * If there are two array entries then we have a workgoup and username otherwise if we just have one item then its a 
	 * username. Any other number could be an error, but since we have no idea what they are trying to do we just treat
	 * it as a username.
	 */
	if (userArray && (CFArrayGetCount(userArray) != 2)) {
		CFRelease(userArray);
		userArray = NULL;
	}
	return userArray;
	
}

/* 
 * Check to see if there is a workgroup/domain in the URL. If yes then return an array
 * with the first element as the workgroup and the second element containing the
 * username and server name. If no workgroup just return NULL.
 */
static CFArrayRef CreateWrkgrpUserArray(CFURLRef url)
{
	CFStringRef netlocation = NULL;
	CFArrayRef	userArray = NULL;
	
	netlocation = CFURLCopyNetLocation(url);
	if (!netlocation)
		return NULL;
	
	userArray = CreateWrkgrpUserArrayFromCFStringRef(netlocation, CFSTR(";"));
	CFRelease(netlocation);
	return userArray;
}


/* 
 * Get the server name out of the URL. CFURLCopyHostName will escape out the 
 * server name for us. So just convert it to the correct code page encoding.
 *
 * Note: Currently we put the server name into a c-style string. In the future 
 * it would be nice to keep this as a CFString.
 */
static int SetServerFromURL(struct smb_ctx *ctx, CFURLRef url)
{
	CFIndex maxlen;
	CFStringRef serverNameRef = CFURLCopyHostName(url);
	char *ipV6Name = NULL;

	ipV6Name = CStringCreateWithCFString(serverNameRef);
	if (ipV6Name && isIPv6NumericName(ipV6Name)) {
        /* CFURLCopyHostName removed the [] so put them back in */
        CFMutableStringRef newServer = CFStringCreateMutableCopy(NULL, 1024, CFSTR("["));
        if (newServer) {
            CFStringAppend(newServer, serverNameRef);
            CFStringAppend(newServer, CFSTR("]"));
            CFRelease(serverNameRef);
            serverNameRef = newServer;
        }
    }

	/* Free up the buffer we allocated */
	if (ipV6Name) {
		free(ipV6Name);
    }
    
    /*
	 * Every time we parse the URL we end up replacing the server name. In the 
	 * future we should skip replacing the server name if we already have one and
	 * the one return CFURLCopyHostName matches it.
	 *
	 * Not going to make that big of a change in an update, so lets limit to the
	 * case were we are dealing with using a domain controller.
	 */
	if (serverNameRef && ctx->serverNameRef && ctx->serverName &&
		(ctx->serverIsDomainController) &&	
		(ctx->ct_flags & SMBCF_CONNECTED) && 
		(CFStringCompare(serverNameRef, ctx->serverNameRef, 0) == kCFCompareEqualTo)) {
		CFRelease(serverNameRef);
		return 0; /* Same name nothing to do here */
	}
	if (ctx->serverNameRef) {
		CFRelease(ctx->serverNameRef);
	}
		/* The serverNameRef should always contain the URL host name or the Bonjour Name */
	ctx->serverNameRef = serverNameRef;
	if (ctx->serverNameRef == NULL)
		return EINVAL;
	DebugLogCFString(ctx->serverNameRef, "Server", __FUNCTION__, __LINE__);
	
	maxlen = CFStringGetMaximumSizeForEncoding(CFStringGetLength(ctx->serverNameRef), kCFStringEncodingUTF8) + 1;
	if (ctx->serverName)
		free(ctx->serverName);
	ctx->serverName = malloc(maxlen);
	if (!ctx->serverName) {
		CFRelease(ctx->serverNameRef);
		ctx->serverNameRef = NULL;
		return ENOMEM;
	}
	CFStringGetCString(ctx->serverNameRef, ctx->serverName, maxlen, kCFStringEncodingUTF8);
	return 0;
}

/* 
 * Get the user and workgroup names and return them in CFStringRef. 
 * First get the CFURLCopyNetLocation because it will not escape out the string.
 */
static CFStringRef CopyUserAndWorkgroupFromURL(CFStringRef *outWorkGroup, CFURLRef url)
{
	CFURLRef net_url = NULL;
	CFArrayRef	userArray = NULL;
	CFStringRef userString = NULL;
	CFMutableStringRef urlString = NULL;
	CFStringRef wrkgrpString = NULL;
	
	*outWorkGroup = NULL;	/* Always start like we didn't get one. */
	/* This will return null if no workgroup in the URL */
	userArray = CreateWrkgrpUserArray(url);
	if (!userArray)	/* We just have a username name  */
		return(CFURLCopyUserName(url));	/* This will escape out the character for us. */
	
	/* Now for the hard part; netlocation contains one of the following:
	 *
	 * URL = "//workgroup;username:password@smb-win2003.apple.com"
	 * URL = "//workgroup;username:@smb-win2003.apple.com"
	 * URL = "//workgroup;username@smb-win2003.apple.com"
	 * URL = "//workgroup;@smb-win2003.apple.com"
	 */
	/* Get the username first */
	urlString = CFStringCreateMutableCopy(NULL, 1024, CFSTR("smb://"));
	if (!urlString) {
		CFRelease(userArray);
		return(CFURLCopyUserName(url));	/* This will escape out the character for us. */
	}
	CFStringAppend(urlString, (CFStringRef)CFArrayGetValueAtIndex(userArray, 1));
	net_url = CFURLCreateWithString(NULL, urlString, NULL);
	CFRelease(urlString);
	urlString = NULL;
	/* Not sure what to do if we fail here */
	if (!net_url) {
		CFRelease(userArray);
		return(CFURLCopyUserName(url));	/* This will escape out the character for us. */		
	}
	/* We now have a URL without the workgroup name, just copy out the username. */
	userString = CFURLCopyUserName(net_url);
	CFRelease(net_url);
	
	/* Now get the workgroup */
	wrkgrpString = CFStringCreateCopy(NULL, (CFStringRef)CFArrayGetValueAtIndex(userArray, 0));
    
    /*
     * Unescape the workgroup string.
     * Be careful, wrkgrpString could be returned as NULL
     */
	CreateStringByReplacingPercentEscapesUTF8(&wrkgrpString, CFSTR(""));
    if (wrkgrpString) {
        /* We have the workgroup return it to the calling routine */
		*outWorkGroup = wrkgrpString;
    }
	CFRelease(userArray);
	return(userString);
}

/* 
 * Get the workgroup and return a username CFStringRef if it exist.
 * First get the CFURLCopyNetLocation because it will not escape out the string.
 */
static CFStringRef SetWorkgroupFromURL(struct smb_ctx *ctx, CFURLRef url)
{
	CFStringRef userString = NULL;
	CFStringRef wrkgrpString = NULL;

	userString = CopyUserAndWorkgroupFromURL(&wrkgrpString, url);

	if (wrkgrpString) {
		LogCFString(wrkgrpString, "Workgroup", __FUNCTION__, __LINE__);
		if (CFStringGetLength(wrkgrpString) <= SMB_MAX_DOMAIN_NAMELEN) {
			str_upper(ctx->ct_setup.ioc_domain, sizeof(ctx->ct_setup.ioc_domain), wrkgrpString);
		}
		CFRelease(wrkgrpString);
	}
	return(userString);
}

/* 
 * Need to call SetWorkgroupFromURL just in case we have a workgroup name. CFURL does not handle
 * a CIFS style URL with a workgroup name.
 */
static int SetUserNameFromURL(struct smb_ctx *ctx, CFURLRef url)
{
	CFStringRef userNameRef = SetWorkgroupFromURL(ctx, url);
	char username[SMB_MAXUSERNAMELEN+1];
	int error;

	/* No user name in the URL */
	if (! userNameRef)
		return 0;
	LogCFString(userNameRef, "Username",__FUNCTION__, __LINE__);

	/* Conversion failed or the data doesn't fit in the buffer */
	if (CFStringGetCString(userNameRef, username, SMB_MAXUSERNAMELEN+1, kCFStringEncodingUTF8) == FALSE) {
		error = ENAMETOOLONG; /* Not sure what else to return. */
	} else {
		error = smb_ctx_setuser(ctx, username);		
	}
	CFRelease(userNameRef);
	return error;
}

/* 
 * The URL may contain no password, an empty password, or a password. An empty password is a passowrd
 * and should be treated the same as a password. This is need to make guest access work.
 *
 *	URL "smb://username:password@server/share" should set the password.
 *	URL "smb://username:@server/" should set the password.
 *	URL "smb://username@server/share" should not set the password.
 *	URL "smb://server/share/path" should not set the password.
 *
 */
static int SetPasswordFromURL(struct smb_ctx *ctx, CFURLRef url)
{
	CFStringRef passwd = CFURLCopyPassword(url);
		
	/*  URL =" //username@smb-win2003.apple.com" or URL =" //smb-win2003.apple.com" */
	if (! passwd)
		return 0;
	
	/* Password is too long return an error */	
	if (CFStringGetLength(passwd) >= SMB_MAXPASSWORDLEN) {
		CFRelease(passwd);
		return ENAMETOOLONG;
	}
	/* 
	 * Works for password and empty password
	 *
	 * URL = "//username:password@smb-win2003.apple.com"
	 * URL = "//username:@smb-win2003.apple.com"
	 */
	CFStringGetCString(passwd, ctx->ct_setup.ioc_password, SMB_MAXPASSWORDLEN, kCFStringEncodingUTF8);
	ctx->ct_flags |= SMBCF_EXPLICITPWD;
	CFRelease(passwd);
	return 0;
}

/* 
 * If URL contains a port then we should get it and set the correct flag.
 *
 *	URL "smb://username:password@server:445/share" set the port to 445.
 *
 */
static void SetPortNumberFromURL(struct smb_ctx *ctx, CFURLRef url)
{
	SInt32 port = CFURLGetPortNumber(url);
	
	/* No port defined in the URL */
	if (port == -1)
		return;
	/* They supplied a port number use it and only it */
	ctx->prefs.tcp_port = port;
	ctx->prefs.tryBothPorts = FALSE;
	os_log_debug(OS_LOG_DEFAULT, "Setting port number to %d", ctx->prefs.tcp_port);
}

/*
 * We need to separate the share name and any path component from the URL.
 *	URL "smb://username:password@server" no share name or path.
 *	URL "smb://username:password@server/"no share name or path.
 *	URL "smb://username:password@server/share" just a share name.
 *	URL "smb://username:password@server/share/path" share name and path.
 *
 * The Share name and Path name will not begin with a slash.
 *		smb://server/ntfs  share = ntfs path = NULL
 *		smb://ntfs/dir1/dir2  share = ntfs path = dir1/dir2
 *		smb://server/OPEN%2FSPACE/dir1   share = OPEN%2FSPACE path = dir1
 *
 * NOTES:
 * 1. This function returns a share and path that are already unescaped
 * 2. A "/" in the share name is handled specially and left unescaped as %2f
 * 3. A "/" in the path is not allowed. Just dont do it.
 *
 */
static int GetShareAndPathFromURL(CFURLRef url, CFStringRef *out_share, CFStringRef *out_path)
{
	Boolean isAbsolute = 0;
	CFArrayRef userArray = NULL;
	CFMutableArrayRef userArrayM = NULL;
	CFStringRef share = CFURLCopyStrictPath(url, &isAbsolute);
    CFMutableStringRef newshare = NULL;
	CFStringRef path = NULL;
    CFRange foundSlash;

    *out_share = NULL;
	*out_path = NULL;
	/* We have an empty share treat it like no share */
	if (share && (CFStringGetLength(share) == 0)) {
		CFRelease(share);	
		share = NULL;
	}
    
	/* Since there is no share name we have nothing left to do. */
    if (!share) {
		return 0;
    }

    /* Try to split it into a share name and path array components */
    userArray = CFStringCreateArrayBySeparatingStrings(NULL, share, CFSTR("/"));
    
    /* If we split it up, then create a mutable array we can work on */
    if (userArray && (CFArrayGetCount(userArray) > 1)) {
		userArrayM = CFArrayCreateMutableCopy(NULL, CFArrayGetCount(userArray),
                                              userArray);
    }
	
    if (userArray) {
		CFRelease(userArray);
        userArray = NULL;
    }

    if (userArrayM) {
        /*
         * We found a share name and path
         *
         * Create a copy of the share name to work on in. If something goes
         * wrong, we can just return the original share string.
         */
		newshare = CFStringCreateMutableCopy(NULL, 0, (CFStringRef)CFArrayGetValueAtIndex(userArrayM, 0));
		if (newshare) {
            /* Remove any trailing slashes out of the share name*/
			CFStringTrim(newshare, CFSTR("/"));
            
            /*
             * Unescape the share name
             * Be careful as newshare could be returned as NULL
             *
             * Have to handle the special case of "/" in the share name like
             * "Open%2FSpace". Because the share name is part of the path, we
             * cant just unescape it to "/" because that gives us a path of
             * Open/Space and we end up thinking the share name is Open instead
             * of "Open/Space". So we special case it and leave it unescaped
             * in the dictionary as "Open%2FSpace".
             */
			CreateStringByReplacingPercentEscapesUTF8((CFStringRef *) &newshare,
                                                      CFSTR("/"));
		}
        
        /* Remove the share out of the mutable array */
		CFArrayRemoveValueAtIndex(userArrayM, 0);
        
        /* Reassemble path with "/" between each path component */
		path = CFStringCreateByCombiningStrings(NULL, userArrayM, CFSTR("/"));

        /* Check to see if we actually have a path */
        if (path && (CFStringGetLength(path) == 0)) {
            /* Empty path so remove it */
			CFRelease(path);
			path = NULL;
		}
        
		if (path) {
            /* Create a mutable path that we can work on */
			CFMutableStringRef newpath = CFStringCreateMutableCopy(NULL, 0,
                                                                   path);
			if (newpath) {
                /* Remove any trailing slashes */
				CFStringTrim(newpath, CFSTR("/"));
                
                /* Replace path with the cleaned up path */
				CFRelease(path);
				path = newpath;							
			}
		}
        
		if (path) {
            /*
             * Unescape the path
             * Be careful as path could be returned as NULL
             *
             * We allow "/" in the share name, but do not allow it in the
             * path. You can always just mount the share and navigate to the
             * folder that has a "/" in it as a workaround.
             */
            foundSlash = CFStringFind (path, CFSTR("%2f"),
                                       kCFCompareCaseInsensitive);
            if (foundSlash.location != kCFNotFound) {
                /*
                 * Found a "%2f" in just path (no share in path), so error out
                 */
                os_log_error(OS_LOG_DEFAULT, "%s: '%%2f' not allowed in URL path",
                             __FUNCTION__);
                
                if (share) {
                    CFRelease(share);
                    share = NULL;
                }
                
                if (newshare) {
                    CFRelease(newshare);
                    newshare = NULL;
                }

                if (path) {
                    CFRelease(path);
                    path = NULL;
                }

                if (userArrayM) {
                    CFRelease(userArrayM);
                    userArrayM = NULL;
                }

                return(EINVAL);
            }
            else {
                CreateStringByReplacingPercentEscapesUTF8(&path, CFSTR("/"));
                LogCFString(path, "Path", __FUNCTION__, __LINE__);
            }
		}

        /* Done with the mutable array */
		CFRelease(userArrayM);
        userArrayM = NULL;
        
		/*
         * If something went wrong, just use the original value. Otherwise
         * use the new share name that we cleaned up.
         */
		if (newshare) {
			CFRelease(share);
			share = newshare;
		}
	}
    else {
        /*
         * Must only be a share name and no path
         *
         * Unescape the share name
         * Be careful as share could be returned as NULL
         *
         * Have to handle the special case of "/" in the share name like
         * "Open%2FSpace". Because the share name is part of the path, we
         * cant just unescape it to "/" because that gives us a path of
         * Open/Space and we end up thinking the share name is Open instead
         * of "Open/Space". So we special case it and leave it unescaped
         * in the dictionary as "Open%2FSpace".
         */
		CreateStringByReplacingPercentEscapesUTF8(&share, CFSTR("/"));
    }

    /*
	 * The above routines will not un-percent escape out slashes. We only allow
     * for the cases where the share name is a single slash. Slashes are treated
     * as delemiters in the path name. So if the share name has a single 0x2f
     * then make it a slash. This means you can't have a share name whose name
     * is literally "0x2f", not likely to happen.
	 */
	if (share && ( kCFCompareEqualTo == CFStringCompare (share, CFSTR("0x2f"), kCFCompareCaseInsensitive) )) {
		CFRelease(share);
		share = CFStringCreateCopy(NULL, CFSTR("/"));		
	}
    
    if (share && (CFStringGetLength(share) >= SMB_MAXSHARENAMELEN)) {
		CFRelease(share);
        share = NULL;
        
        if (path) {
			CFRelease(path);
            path = NULL;
        }
        
		return ENAMETOOLONG;
	}

    *out_share = share;
	*out_path = path;
	return 0;
}

/*
 * We need to separate the share name and any path component from the URL.
 *	URL "smb://username:password@server" no share name or path.
 *	URL "smb://username:password@server/"no share name or path.
 *	URL "smb://username:password@server/share" just a share name.
 *	URL "smb://username:password@server/share/path" share name and path.
 *
 * The Share name and Path name will not begin with a slash.
 *		smb://server/ntfs  share = ntfs path = NULL
 *		smb://ntfs/dir1/dir2  share = ntfs path = dir1/dir2
 */
static int SetShareAndPathFromURL(struct smb_ctx *ctx, CFURLRef url)
{
	CFStringRef share = NULL;
	CFStringRef path = NULL;
	CFIndex maxlen;
	int error;
    CFRange foundSlash;
    CFMutableStringRef newshare = NULL;

    /* Get the unescaped share and path */
    error = GetShareAndPathFromURL(url, &share, &path);
	if (error) {
		return error;
    }

	/* Since there is no share name we have nothing left to do. */
	if (share == NULL) {
        if (path != NULL) {
            CFRelease(path);
            path = NULL;
        }
        
		return 0;
    }
    
	DebugLogCFString(share, "Share", __FUNCTION__, __LINE__);

    /*
     * Have to handle the special case of / in the share name like
     * "Open%2FSpace". GetShareAndPathFromURL() returned the share name as
     * "Open%2FSpace" but we want to store it in the context as "Open/Space".
     */
    foundSlash = CFStringFind (share, CFSTR("%2f"), kCFCompareCaseInsensitive);
    if (foundSlash.location != kCFNotFound) {
        /*
         * Found a "%2f" in the path, so must be a share name with "/"
         * in it. Manually change it back to "/", so we can save the real
         * share name
         */
        newshare = CFStringCreateMutableCopy(NULL, 0, share);
        if (newshare) {
            /* Manually replace all "%2f" with "/" */
            CFStringFindAndReplace(newshare, CFSTR("%2f"), CFSTR("/"),
                                   CFRangeMake(0, CFStringGetLength(newshare)),
                                   kCFCompareCaseInsensitive);

            /* Replace share with the unescaped version */
            CFRelease(share);
            share = newshare;
            
            DebugLogCFString(share, "New Share", __FUNCTION__, __LINE__);
        }
    }

    /* If a share or mount path are already set in context, clear them */
	if (ctx->ct_origshare) {
		free(ctx->ct_origshare);
        ctx->ct_origshare = NULL;
    }
	
	if (ctx->mountPath) {
		CFRelease(ctx->mountPath);
    }
	ctx->mountPath = NULL;
	
    /* Save the new share into the context */
	maxlen = CFStringGetMaximumSizeForEncoding(CFStringGetLength(share), kCFStringEncodingUTF8) + 1;
	ctx->ct_origshare = malloc(maxlen);
	if (!ctx->ct_origshare) {
        /* malloc failed, so bail */
		CFRelease(share);
        share = NULL;
        
        if (path != NULL) {
            CFRelease(path);
            path = NULL;
        }
        
		return ENOMEM;
	}

    CFStringGetCString(share, ctx->ct_origshare, maxlen, kCFStringEncodingUTF8);
	str_upper(ctx->ct_sh.ioc_share, sizeof(ctx->ct_sh.ioc_share), share);
    
	CFRelease(share);
    share = NULL;
	
    /* Save the path into the context */
	ctx->mountPath = path;

	return 0;
}

/*
 * Here we expect something like
 *   "//[workgroup;][user[:password]@]host[/share[/path]]"
 * See http://ietf.org/internet-drafts/draft-crhertel-smb-url-07.txt
 */
int ParseSMBURL(struct smb_ctx *ctx)
{
	int error  = EINVAL;

    /* Make sure its a good URL, better be at this point */
	if ((!CFURLCanBeDecomposed(ctx->ct_url)) || (SMBSchemeLength(ctx->ct_url) < 0)) {
		os_log_error(OS_LOG_DEFAULT, "This is an invalid URL, syserr = %s",
					 strerror(error));
		return error;
	}

    error = SetServerFromURL(ctx, ctx->ct_url);
	if (error) {
		os_log_error(OS_LOG_DEFAULT, "The URL has a bad server name, syserr = %s",
					 strerror(error));
		return error;
	}

    error = SetUserNameFromURL(ctx, ctx->ct_url);
	if (error) {
		os_log_error(OS_LOG_DEFAULT, "The URL has a bad user name, syserr = %s",
					 strerror(error));
		return error;
	}
    
	error = SetPasswordFromURL(ctx, ctx->ct_url);
	if (error) {
		os_log_error(OS_LOG_DEFAULT, "The URL has a bad password, syserr = %s",
					 strerror(error));
		return error;
	}
    
	SetPortNumberFromURL(ctx, ctx->ct_url);
        
	error = SetShareAndPathFromURL(ctx, ctx->ct_url);
        
	/* CFURLCopyQueryString to get ?WINS=msfilsys.apple.com;NODETYPE=H info */
	return error;
}

/*
 * Given a Dfs Referral string create a CFURL. Remember referral have a file
 * syntax
 * 
 * Example: "/smb-win2003.apple.com/DfsRoot/DfsLink1"
 */
CFURLRef CreateURLFromReferral(CFStringRef inStr)
{
	CFURLRef ct_url = NULL;
	CFMutableStringRef urlString = CFStringCreateMutableCopy(NULL, 0, CFSTR("smb:/"));
	CFStringRef escapeStr = inStr;
	
    /* 
     * CreateStringByAddingPercentEscapesUTF8() will either create a new string
     * or return the original with ref count incremented. Either way we have to
     * call CFRelease on the returned string 
     */
	CreateStringByAddingPercentEscapesUTF8(&escapeStr, NULL, NULL, FALSE);

	if (urlString) {
		CFStringAppend(urlString, escapeStr);
		ct_url = CFURLCreateWithString(kCFAllocatorDefault, urlString, NULL);
		CFRelease(urlString);	/* We create it now release it */
	}
    
	if (!ct_url) {
		LogCFString(inStr, "creating url failed", __FUNCTION__, __LINE__);
	}
    
    if (escapeStr) {
        CFRelease(escapeStr);
    }
   
	return ct_url;
}

/*
 * Given a c-style string create a CFURL. We assume the c-style string is in  
 * URL or UNC format. Anything else will give unexpected behavior.
 * NOTE: The library code doesn't care if the scheme exist or not in the URL, 
 * but we attempt to create a URL with a scheme, just for correctness sake.
 *
 * Note: If its a URL, then do not escape it out again since it should already
 * be escaped out properly.
 */
CFURLRef CreateSMBURL(const char *url)
{
	CFURLRef ct_url = NULL;
	CFStringRef urlString = CFStringCreateWithCString(NULL, 
                                                      url, 
                                                      kCFStringEncodingUTF8);
    CFStringRef escapedUrlString;
    int UNCformat = 0;
    
    escapedUrlString = NULL;
	
	/* 
	 * We have a UNC path that we need to convert into a SMB URL. Currently we 
	 * just replace the backslashes with slashes
	 */
   if (urlString && (*url == '\\') && (*(url + 1) == '\\')) {
       CFArrayRef urlArray = CFStringCreateArrayBySeparatingStrings(NULL, 
                                                                    urlString, 
                                                                    CFSTR("\\"));

	   CFRelease(urlString);
	   urlString = NULL;
	   if (urlArray) {
		   urlString = CFStringCreateByCombiningStrings(NULL, 
                                                        urlArray, 
                                                        CFSTR("/"));
		   CFRelease(urlArray);
	   }
       UNCformat = 1;
    }
    
	/* Something failed just get out */
	if (!urlString) {
		return NULL;
    }
    
	DebugLogCFString(urlString, "urlString ", __FUNCTION__, __LINE__);

	/* 
	 * No scheme, add one if we can, but not required by the library code. 
	 * NOTE: If no scheme, then expect the string to start with double slashes.
	 */
	if ((!CFStringHasPrefix(urlString, CFSTR("smb://"))) && 
		(!CFStringHasPrefix(urlString, CFSTR("cifs://")))) {
		CFMutableStringRef urlStringM = CFStringCreateMutableCopy(NULL, 
                                                                  1024, 
                                                                  CFSTR("smb:"));

		if (urlStringM) {
			CFStringAppend(urlStringM, urlString);
			CFRelease(urlString);
			urlString = urlStringM;
		}
	}
    
    /* Something failed just get out */
    if (urlString == NULL) {
        return (NULL);
    }
   
    if (UNCformat == 1) {
        /* For UNC format strings, escape out any non-URL characters */
        /* This is the only C option for escaping URL's, so we have to keep it. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        escapedUrlString = CFURLCreateStringByAddingPercentEscapes(NULL, 
                                                                   urlString, 
                                                                   NULL, 
                                                                   NULL, 
                                                                   kCFStringEncodingUTF8);
#pragma clang diagnostic pop
        CFRelease(urlString);	/* Can release it now */
        
        /* Something failed just get out */
        if (escapedUrlString == NULL) {
            return (NULL);
        }

        /* now create the URL */
        ct_url = CFURLCreateWithString(kCFAllocatorDefault, 
                                       escapedUrlString, 
                                       NULL);
        
        CFRelease(escapedUrlString);	/* We create it now release it */
    }
    else {
        /* 
         * For URL format strings, it should already be escaped. 
         * Just create the URL.
         */
        ct_url = CFURLCreateWithString(kCFAllocatorDefault, 
                                       urlString, 
                                       NULL);
        
        CFRelease(urlString);	/* Can release it now */
    }
	
	return ct_url;
}

/* 
 * Given a url parse it and place the component in a dictionary we create.
 */
int smb_url_to_dictionary(CFURLRef url, CFDictionaryRef *dict)
{
	CFMutableDictionaryRef mutableDict = NULL;
	int error  = 0;
	CFStringRef Server = NULL;
	CFStringRef Username = NULL;
	CFStringRef DomainWrkgrp = NULL;
	CFStringRef Password = NULL;
	CFStringRef Share = NULL;
	CFStringRef Path = NULL;
	CFStringRef Port = NULL;
	
	/* Make sure its a good URL, better be at this point */
	if ((!CFURLCanBeDecomposed(url)) || (SMBSchemeLength(url) < 0)) {
		os_log_error(OS_LOG_DEFAULT, "%s: Invalid URL, syserr = %s",
					 __FUNCTION__, strerror(EINVAL));	
		goto ErrorOut;
	}
	
	/* create and return the server parameters dictionary */
	mutableDict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                            &kCFTypeDictionaryKeyCallBacks,
                                            &kCFTypeDictionaryValueCallBacks);
	if (mutableDict == NULL) {
        /* Failed to create the mutable dictionary, so bail */
		error = errno;
		os_log_error(OS_LOG_DEFAULT, "%s: CFDictionaryCreateMutable failed, syserr = %s",
					 __FUNCTION__, strerror(error));
		goto ErrorOut;
	}
	
	/*
	 * SMB can have two different scheme's cifs or smb. When we made
     * SMBSchemeLength call at the start of this routine it made sure we had
     * one or the other scheme. Always default here to the SMB scheme.
     *
     * Save the scheme in dictionary
	 */
	CFDictionarySetValue (mutableDict, kNetFSSchemeKey, CFSTR(SMB_SCHEME_STRING));

    /* Try to get the host string and port string */
    error = NetFSCopyHostAndPort(url, &Server, &Port);
	if ((Server == NULL) || (error != noErr)) {
        if (Port != NULL) {
            CFRelease(Port);
            Port = NULL;
        }
		goto ErrorOut; /* Server name is required */
    }
	
    /* Save the host string in dictionary */
	LogCFString(Server, "Server String", __FUNCTION__, __LINE__);
	CFDictionarySetValue (mutableDict, kNetFSHostKey, Server);
	CFRelease(Server);
	Server = NULL;
	
    /* If there is a port string, then save it in dictionary */
    if (Port != NULL) {
        CFDictionarySetValue (mutableDict, kNetFSAlternatePortKey, Port);
        CFRelease(Port);
        Port = NULL;
    }

    /* Try to get username and workgroup strings */
	Username = CopyUserAndWorkgroupFromURL(&DomainWrkgrp, url);
	LogCFString(Username, "Username String", __FUNCTION__, __LINE__);
	LogCFString(DomainWrkgrp, "DomainWrkgrp String", __FUNCTION__, __LINE__);
	error = 0;
    
    /* Check to see if username is too long */
    if ((Username) && (CFStringGetLength(Username) >= SMB_MAXUSERNAMELEN)) {
		error = ENAMETOOLONG;
    }

    /* Check if workgroup name is too long */
    if ((DomainWrkgrp) &&
        (CFStringGetLength(DomainWrkgrp) > SMB_MAX_DOMAIN_NAMELEN)) {
		error = ENAMETOOLONG;
    }
	
    /* Was either username or workgroup name too long? If so, bail */
	if (error) {
        if (Username) {
			CFRelease(Username);
            Username = NULL;
        }
        
        if (DomainWrkgrp) {
			CFRelease(DomainWrkgrp);
            DomainWrkgrp = NULL;
        }
        
		goto ErrorOut; /* Username or Domain name is too long */
	}
	
	/* 
	 * We have a domain name so combined it with the user name so we can it 
	 * display to the user. We now test to make sure we have a username. Having
	 * a domain without a username makes no sense, so don't return either.
	 */
	if (DomainWrkgrp && Username && CFStringGetLength(Username)) {
		CFMutableStringRef tempString = CFStringCreateMutableCopy(NULL, 0, DomainWrkgrp);
		
		if (tempString) {
			CFStringAppend(tempString, CFSTR("\\"));
			CFStringAppend(tempString, Username);
			CFRelease(Username);
			Username = tempString;
		}
	}

    /* If there is a username string, then save it in dictionary */
	if (Username) {
		CFDictionarySetValue (mutableDict, kNetFSUserNameKey, Username);
		CFRelease(Username);
        Username = NULL;
	}	

    if (DomainWrkgrp) {
		CFRelease(DomainWrkgrp);
        DomainWrkgrp = NULL;
    }
    
    /* Try to get the password */
	Password = CFURLCopyPassword(url);
	if (Password) {
		if (CFStringGetLength(Password) >= SMB_MAXPASSWORDLEN) {
			error = ENAMETOOLONG;
			CFRelease(Password);		
			goto ErrorOut; /* Password is too long */
		}
        
        /* Save password into dictionary */
		CFDictionarySetValue (mutableDict, kNetFSPasswordKey, Password);
		CFRelease(Password);
        Password = NULL;
	}
	
	/*
	 * We used to keep the share and path as two different elements in the dictionary. This was
	 * changed to satisfy NetFS and other plugins. We still need to check and make sure the
	 * share and path are correct. So now split them apart and then put them put them back together.
	 */
	error = GetShareAndPathFromURL(url, &Share, &Path);
    if (error) {
        /* Failed to get share or path */
		goto ErrorOut;
    }
	
	LogCFString(Share, "Share String", __FUNCTION__, __LINE__);
	LogCFString(Path, "Path String", __FUNCTION__, __LINE__);

	if (Share && Path) {
		/* 
		 * We have a share and path, but there is nothing in the 
		 * share, then return an error 
		 */
	    if (CFStringGetLength(Share) == 0) {
			CFRelease(Path);
			CFRelease(Share);
			Share = Path = NULL;
			error = EINVAL;
			os_log_error(OS_LOG_DEFAULT, "%s: No share name found, syserr = %s",
						 __FUNCTION__, strerror(error));
			goto ErrorOut;
		}
        
        /* Recombine the share and path strings together for the path */
		if (CFStringGetLength(Path)) {
			CFMutableStringRef tempString = CFStringCreateMutableCopy(NULL, 0, Share);
			if (tempString) {
				CFStringAppend(tempString, CFSTR("/"));
				CFStringAppend(tempString, Path);
                
                /* Save the path string into dictionary */
				CFDictionarySetValue (mutableDict, kNetFSPathKey, tempString);
                
				CFRelease(tempString);
                tempString = NULL;
                
				CFRelease(Share);		
				Share = NULL;
			} 
		}
	}
    
	/* Ignore any empty share at this point */
    if (Share && CFStringGetLength(Share)) {
        /* Save the share string into dictionary */
		CFDictionarySetValue (mutableDict, kNetFSPathKey, Share);
    }

    if (Share) {
		CFRelease(Share);
        Share = NULL;
    }
	
    if (Path) {
		CFRelease(Path);
        Path = NULL;
    }

    /* Return the filled in dictionary */
	*dict = mutableDict;
	return 0;
		
ErrorOut:
	*dict = NULL;
    if (mutableDict) {
		CFRelease(mutableDict);
        mutableDict = NULL;
    }
    
    if (!error)	{
        /* If no error, then set it to the default error */
		error = EINVAL;
    }
    
	return error;
	
}

/* 
 * Given a dictionary create a url string. We assume that the dictionary has
 * any characters that need to be escaped, NOT currently escaped out.
 */
static int smb_dictionary_to_urlstring(CFDictionaryRef dict, CFMutableStringRef *urlReturnString)
{
	int error = 0;
	CFMutableStringRef urlStringM = NULL;
	CFStringRef DomainWrkgrp = NULL;
	CFStringRef Username = NULL;
	CFStringRef Password = NULL;
	CFStringRef Server = NULL;
	CFStringRef PortNumber = NULL;
	CFStringRef ShareAndPath = NULL;
	Boolean	releaseUsername = FALSE;
	char *ipV6Name = NULL;
    CFRange foundSlash;
    CFArrayRef userArray = NULL;
    CFMutableArrayRef userArrayM = NULL;
    CFStringRef share = NULL;
    CFStringRef path = NULL;
    CFMutableStringRef newshare = NULL;

    urlStringM = CFStringCreateMutableCopy(NULL, 1024, CFSTR("smb://"));
	if (urlStringM == NULL) {
		error = errno;
		os_log_error(OS_LOG_DEFAULT, "%s: couldn't allocate the url string, syserr = %s",
					 __FUNCTION__, strerror(error));
		goto WeAreDone;
	}
		 
	/* Get the server name, required value */
	Server = CFDictionaryGetValue(dict, kNetFSHostKey);
	if (Server == NULL) {
		error = EINVAL;
		os_log_error(OS_LOG_DEFAULT, "%s: no server name, syserr = %s",
					 __FUNCTION__, strerror(error));
		goto WeAreDone;
	}

	/*
	 * So we have three basic server names to cover here. 
	 *
	 * 1. Bonjour Name requires these /@:,?=;&+$ extra characters to be percent 
	 *	  escape out.
	 *
	 * 2. NetBIOS Name requires these ~!@#$%^&;'.(){} extra characters to be 
	 *	  percent escape out.
	 *
	 * 3. DNS Names requires the DOT IPv6 Notification address to be enclosed in 
	 *	  square brackets and that the colon not to be escaped out.
	 *					 
	 * Note that CFURLCopyHostName will remove the square brackets from the DOT  
	 * IPv6 Notification address. smb_url_to_dictionary will put the brackets
     * back into the IPv6 address in the dictionary. We expect this behavior so 
     * anyone changing the dictionary server field will need to make sure
	 * it does have square brackets.
	 */ 
	ipV6Name = CStringCreateWithCFString(Server);
    
	/* Is this an IPv6 numeric name, then leave the square brackets around it */
	if (ipV6Name && isIPv6NumericName(ipV6Name)) {
        CreateStringByAddingPercentEscapesUTF8(&Server, CFSTR("[]"), NULL,
                                               FALSE);
    }
    else {
		/* Some other name make sure we percent escape all the characters needed */
		CreateStringByAddingPercentEscapesUTF8(&Server, NULL,
                                               CFSTR("~!'()/@:,?=;&+$"), FALSE);
	}
    
	/* Free up the buffer we allocated */
	if (ipV6Name) {
		free(ipV6Name);
    }
	
	if (Server == NULL) {
		error = EINVAL;
		os_log_error(OS_LOG_DEFAULT, "%s: still no server name, syserr = %s",
					 __FUNCTION__, strerror(error));	
		goto WeAreDone;
	}
    
	/* Now get all the other parts of the url. */
	Username = CFDictionaryGetValue(dict, kNetFSUserNameKey);
	/* We have a user name see if they entered a domain also. */
	if (Username) {
		CFArrayRef	userArray = NULL;
		/* 
		 * Remember that on windows a back slash is illegal, so if someone wants
         * to use one in the username they will need to escape percent it out.
		 */
		userArray = CreateWrkgrpUserArrayFromCFStringRef(Username, CFSTR("\\"));
		/*
         * If we have an array then we have a domain\username in the Username
         * String
         */
		if (userArray) {
			DomainWrkgrp = CFStringCreateCopy(NULL, (CFStringRef)CFArrayGetValueAtIndex(userArray, 0));
			Username = CFStringCreateCopy(NULL, (CFStringRef)CFArrayGetValueAtIndex(userArray, 1));
			CFRelease(userArray);
			releaseUsername = TRUE;
		}
	}

	Password = CFDictionaryGetValue(dict, kNetFSPasswordKey);
    ShareAndPath = CFDictionaryGetValue(dict, kNetFSPathKey);
	PortNumber = CFDictionaryGetValue(dict, kNetFSAlternatePortKey);

	/* 
	 * Percent escape out any URL special characters, for the username,
     * password, Domain/Workgroup, path, and port. Not sure the port is
     * required, but AFP does it so why not.
     *
     * The CreateStringByAddingPercentEscapesUTF8 will return either NULL or a
     * value that must be released.
	 */
    if (DomainWrkgrp) {
        CreateStringByAddingPercentEscapesUTF8(&DomainWrkgrp, NULL,
                                               CFSTR("@:;/?"), TRUE);
    }
    
    if (Username) {
        CreateStringByAddingPercentEscapesUTF8(&Username, NULL,
                                               CFSTR("@:;/?"), releaseUsername);
    }
    
    if (Password) {
        CreateStringByAddingPercentEscapesUTF8(&Password, NULL,
                                               CFSTR("@:;/?"), FALSE);
    }

    if (PortNumber) {
        CreateStringByAddingPercentEscapesUTF8(&PortNumber, NULL, NULL, FALSE);
    }

    if (ShareAndPath) {
        /* Try to split it into a share name and path array components */
        userArray = CFStringCreateArrayBySeparatingStrings(NULL, ShareAndPath,
                                                           CFSTR("/"));
        
        /* If we split it up, then create a mutable array we can work on */
        if (userArray && (CFArrayGetCount(userArray) > 1)) {
            userArrayM = CFArrayCreateMutableCopy(NULL,
                                                  CFArrayGetCount(userArray),
                                                  userArray);
        }
        
        if (userArray) {
            CFRelease(userArray);
            userArray = NULL;
        }

        if (userArrayM) {
            /*
             * We found a share name and path
             */
            share = CFStringCreateCopy(kCFAllocatorDefault, (CFStringRef)CFArrayGetValueAtIndex(userArrayM, 0));

            /* Remove the share out of the mutable array */
            CFArrayRemoveValueAtIndex(userArrayM, 0);
            
            /* Reassemble path with "/" between each path component */
            path = CFStringCreateByCombiningStrings(NULL, userArrayM, CFSTR("/"));

            /* Check to see if we actually have a path */
            if (path && (CFStringGetLength(path) == 0)) {
                /* Empty path so remove it */
                CFRelease(path);
                path = NULL;
            }

            if (path) {
                /* Escape out the path, but do not escape out the "/" */
                CreateStringByAddingPercentEscapesUTF8(&path, NULL,
                                                       CFSTR(" !#$%&'()*+,:;<=>?@[]|"),
                                                       FALSE);
            }

            /* Done with the mutable array */
            CFRelease(userArrayM);
            userArrayM = NULL;
        }
        else {
            /*
             * Must only be a share name and no path
             */
            share = CFStringCreateCopy(kCFAllocatorDefault, ShareAndPath);
        }

        if (share) {
            /*
             * Escape out the share
             *
             * Have to handle the special case of "/" in the share name like
             * "Open%2FSpace". Because the share name is part of the path, we
             * cant just unescape it to "/" because that gives us a path of
             * Open/Space and we end up thinking the share name is Open instead
             * of "Open/Space". So we special case it and leave it unescaped
             * in the dictionary as "Open%2FSpace".
             */
            foundSlash = CFStringFind (share, CFSTR("%2f"),
                                       kCFCompareCaseInsensitive);
            if (foundSlash.location != kCFNotFound) {
                /*
                 * Found a "%2f" in the path, so must be a share name with "/"
                 * in it. Manually change it back to "/", so we can escape
                 * out the share name
                 */
                newshare = CFStringCreateMutableCopy(NULL, 0, share);

                if (newshare) {
                    /* Manually replace all "%2f" with "/" */
                    CFStringFindAndReplace(newshare, CFSTR("%2f"), CFSTR("/"),
                                           CFRangeMake(0, CFStringGetLength(newshare)),
                                           kCFCompareCaseInsensitive);
                    
                    /* Replace share with the fixed up share name */
                    CFRelease(share);
                    share = newshare;
                }
           }

            /* Escape out any URL reserved chars including "/" */
            CreateStringByAddingPercentEscapesUTF8(&share, NULL,
                                                   CFSTR(" !#$%&'()*+,:;<=>?@[]|/"),
                                                   FALSE);
        }
    }

	LogCFString(Username, "Username String", __FUNCTION__, __LINE__);
	LogCFString(DomainWrkgrp, "Domain String", __FUNCTION__, __LINE__);
    LogCFString(share, "Share String", __FUNCTION__, __LINE__);
    LogCFString(path, "Path String", __FUNCTION__, __LINE__);
	LogCFString(PortNumber, "PortNumber String", __FUNCTION__, __LINE__);
	
	/* Add the Domain/Workgroup */
	if (DomainWrkgrp) {
		CFStringAppend(urlStringM, DomainWrkgrp);
		CFStringAppend(urlStringM, CFSTR(";"));
	}
    
	/* Add the username and password */
	if (Username || Password) {		
		if (Username)
			CFStringAppend(urlStringM, Username);
		if (Password) {
			CFStringAppend(urlStringM, CFSTR(":"));
			CFStringAppend(urlStringM, Password);			
		}
		CFStringAppend(urlStringM, CFSTR("@"));
	}
	
	/* Add the server */	
	CFStringAppend(urlStringM, Server);
	
	/* Add the port number */
	if (PortNumber) {
		CFStringAppend(urlStringM, CFSTR(":"));
		CFStringAppend(urlStringM, PortNumber);
	}
	
	/* Add the share and path */
    if (share) {
        CFStringAppend(urlStringM, CFSTR("/"));
        /* If the share name is a slash percent escape it out */
        if (kCFCompareEqualTo == CFStringCompare (share, CFSTR("/"),
                                                  kCFCompareCaseInsensitive)) {
            CFStringAppend(urlStringM, CFSTR("0x2f"));
        }
        else {
            CFStringAppend(urlStringM, share);
        }

        /* Can only add a path if there was a share */
        if (path) {
            CFStringAppend(urlStringM, CFSTR("/"));
            CFStringAppend(urlStringM, path);
        }
    }
    
    DebugLogCFString(urlStringM, "URL String", __FUNCTION__, __LINE__);

WeAreDone:
    if (Username) {
		CFRelease(Username);
    }
    if (Password) {
		CFRelease(Password);
    }
    if (DomainWrkgrp) {
		CFRelease(DomainWrkgrp);
    }
    /* Do not release ShareAndPath as we only did a Get on it */
    if (share) {
        CFRelease(share);
    }
    if (path) {
        CFRelease(path);
    }
    if (PortNumber) {
		CFRelease(PortNumber);
    }
    if (Server) {
		CFRelease(Server);
    }
	
    /* If no error, return the URL string */
    if (error == 0) {
		*urlReturnString = urlStringM;
    }
    else if (urlStringM) {
		CFRelease(urlStringM);
    }
		
	return error;
}


/* 
 * Given a dictionary create a url. We assume that the dictionary has any characters that need to
 * be escaped out escaped out.
 */
int smb_dictionary_to_url(CFDictionaryRef dict, CFURLRef *url)
{
	int error;
	CFMutableStringRef urlStringM = NULL;
	
	error = smb_dictionary_to_urlstring(dict, &urlStringM);
	/* Ok we have everything we need for the URL now create it. */
	if ((error == 0) && urlStringM) {
		*url = CFURLCreateWithString(NULL, urlStringM, NULL);
		if (*url == NULL)
			error = errno;
	}
	
	if (urlStringM)
		CFRelease(urlStringM);
	if (error)
		os_log_error(OS_LOG_DEFAULT, "%s: creating the url failed, syserr = %s",
					 __FUNCTION__, strerror(error));	
		
	return error;
	
}

CFStringRef CreateURLCFString(CFStringRef Domain, CFStringRef Username,
                              CFStringRef Password, CFStringRef ServerName,
                              CFStringRef Path, CFStringRef PortNumber)
{
	CFMutableDictionaryRef mutableDict = NULL;
	int error;
	CFMutableStringRef urlString = NULL;
    CFRange foundBonjourName;
    CFRange foundEscapedPeriods;
    CFMutableStringRef newServerNameRef = NULL;

	mutableDict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, 
											&kCFTypeDictionaryValueCallBacks);
	if (mutableDict == NULL) {
		os_log_error(OS_LOG_DEFAULT, "%s: CFDictionaryCreateMutable failed, syserr = %s",
					 __FUNCTION__, strerror(errno));
		return NULL;
	}
	
	if (Domain && Username) {
		CFMutableStringRef tempString = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, Domain);
		
		if (tempString) {
			CFStringAppend(tempString, CFSTR("\\"));
			CFStringAppend(tempString, Username);
			Username = tempString;
		}
        else {
			CFRetain(Username);
		}
	} else if (Username) {
		CFRetain(Username);
	}
    
	if (Username) {
		CFDictionarySetValue(mutableDict, kNetFSUserNameKey, Username);
		CFRelease(Username);
	}
    
	if (Password) {
		CFDictionarySetValue(mutableDict, kNetFSPasswordKey, Password);
	}
    
	if (ServerName) {
        /* Check to see if its a Bonjour server name */
        foundBonjourName = CFStringFind (ServerName, CFSTR("._smb._tcp.local"), 0);
        if (foundBonjourName.location != kCFNotFound) {
            /*
             * <38807912> If its a Bonjour name, then any "." has been
             * escaped with "\134.". Need to replace that with "%5c%2e"
             * because DCERPC will strip any "\134."
             *
             * Later we reverse this and put the "\134." back in.
             */
            foundEscapedPeriods = CFStringFind (ServerName, CFSTR("%5c%2e"), 0);
            if (foundEscapedPeriods.location != kCFNotFound) {
                newServerNameRef = CFStringCreateMutableCopy(NULL, 0, ServerName);
                if (newServerNameRef) {
                    /* Replace "%5c%2e" with "\134." */
                    CFStringFindAndReplace(newServerNameRef, CFSTR("%5c%2e"), CFSTR("\134."),
                                           CFRangeMake(0, CFStringGetLength(newServerNameRef)),
                                           kCFCompareCaseInsensitive);
               }
            }
        }

        if (newServerNameRef) {
            CFDictionarySetValue(mutableDict, kNetFSHostKey, newServerNameRef);
            CFRelease(newServerNameRef);
        }
        else {
            CFDictionarySetValue(mutableDict, kNetFSHostKey, ServerName);
        }
	}
    
	if (Path) {
		CFDictionarySetValue (mutableDict, kNetFSPathKey, Path);
	}
    
	if (PortNumber) {
		CFDictionarySetValue (mutableDict, kNetFSAlternatePortKey, PortNumber);
	}
    
	error = smb_dictionary_to_urlstring(mutableDict, &urlString);
	CFRelease(mutableDict);
    
	if (error) {
		errno = error;
	}
    
	return urlString;
}

/*
 * Check to make sure we have the correct user name and share. If we already have
 * both a username and share name then we are done, otherwise get the latest stuff.
 */
static void UpdateDictionaryWithUserAndShare(struct smb_ctx *ctx, CFMutableDictionaryRef mutableDict)
{
	CFStringRef DomainWrkgrp = NULL;
	CFStringRef Username = NULL;
	CFStringRef share = NULL;
	CFMutableStringRef path = NULL;
	
	Username = CFDictionaryGetValue(mutableDict, kNetFSUserNameKey);
	share = CFDictionaryGetValue(mutableDict, kNetFSPathKey);
	
	/* Everything we need is in the dictionary */
	if (share && Username) 
		return;
	
	/* Add the user name, if we have one */
	if (ctx->ct_setup.ioc_user[0]) {
		Username = CFStringCreateWithCString(NULL, ctx->ct_setup.ioc_user, kCFStringEncodingUTF8);
		if (ctx->ct_setup.ioc_domain[0])	/* They gave us a domain add it */
			DomainWrkgrp = CFStringCreateWithCString(NULL, ctx->ct_setup.ioc_domain, kCFStringEncodingUTF8);

		/* 
		 * We have a domain name so combined it with the user name. We now test 
		 * to make sure we have a username. Having a domain without a username 
		 * makes no sense, so don't return either.
		 */
		if (DomainWrkgrp && Username && CFStringGetLength(Username)) {
			CFMutableStringRef tempString = CFStringCreateMutableCopy(NULL, 0, DomainWrkgrp);
			
			if (tempString) {
				CFStringAppend(tempString, CFSTR("\\"));
				CFStringAppend(tempString, Username);
				CFRelease(Username);
				Username = tempString;
			}
		}
		if (DomainWrkgrp) {
			CFRelease(DomainWrkgrp);
		}
		if (Username)
		{
			CFDictionarySetValue (mutableDict, kNetFSUserNameKey, Username);
			CFRelease(Username);				
		}	
	}
	/* if we have a share then we are done */
	if (share || !ctx->ct_origshare)
		return;
	
	path = CFStringCreateMutable(NULL, 1024);
	/* Should never happen, but just to be safe */
	if (!path)
		return;
	
	CFStringAppendCString(path, ctx->ct_origshare, kCFStringEncodingUTF8);
	/* Add the path if we have one */
	if (ctx->mountPath) {
		CFStringAppend(path, CFSTR("/"));
		CFStringAppend(path, ctx->mountPath);		
	}
	CFDictionarySetValue (mutableDict, kNetFSPathKey, path);
	CFRelease(path);
}

/*
 * We need to create the from name. The from name is just a URL without the scheme. We 
 * never put the password in the from name, but if they have an empty password then we
 * need to make sure that it's included.
 *
 * Examples: 
 *	URL "smb://username:@server/share" - Empty password, just remove the scheme.
 *	URL "smb://username:password@server/share" - Need to remove the password and the scheme.
 *	URL "smb://username@server" - Need to add the share and remove the scheme.
 *	URL "smb://server" - Need to add the username and share and remove the scheme.
 *	URL "smb://server/share/path" - Need to add the usernameand remove the scheme.
 */
void CreateSMBFromName(struct smb_ctx *ctx, char *fromname, int maxlen)
{
	CFMutableStringRef urlStringM = NULL;
	CFMutableStringRef newUrlStringM = NULL;
	CFMutableDictionaryRef mutableDict = NULL;
	CFStringRef Password = NULL;
	int SchemeLength = 0;
	int error = 0;;
	
	/* Always start with the original url and a cleaned out the from name */
	bzero(fromname, maxlen);
	SchemeLength = SMBSchemeLength(ctx->ct_url);
	urlStringM = CFStringCreateMutableCopy(NULL, 0, CFURLGetString(ctx->ct_url));
	if (urlStringM == NULL) {
		os_log_error(OS_LOG_DEFAULT, "Failed creating URL string, syserr = %s", strerror(errno));
		return;
	}
	
	error = smb_url_to_dictionary(ctx->ct_url, (CFDictionaryRef *)&mutableDict);
	if (error || (mutableDict == NULL)) {
		os_log_debug(OS_LOG_DEFAULT, "Failed parsing URL, syserr = %s", strerror(error));
		goto WeAreDone;
	}
	UpdateDictionaryWithUserAndShare(ctx, mutableDict);
	
	Password = CFDictionaryGetValue(mutableDict, kNetFSPasswordKey);
	/* 
	 * If there is a password and its not an empty password then remove it. Never
	 * show the password in the mount from name.
	 */
	if (Password && (CFStringGetLength(Password) > 0)) {
		CFDictionaryRemoveValue(mutableDict, kNetFSPasswordKey);
	}
	/* Guest access has an empty password. */
	if (ctx->ct_setup.ioc_userflags & SMBV_GUEST_ACCESS)
		CFDictionarySetValue (mutableDict, kNetFSPasswordKey, CFSTR(""));

	/* 
	 * Recreate the URL from our new dictionary. The old code would not escape
	 * out the share/path, which can cause us issue. Now the from name can look
	 * pretty goofy, but we will always work with alias. Now this was originally
	 * done because carbon would use the last part of the mount from name as the
	 * volume name. We now return the volume name, so this is no longer an issue.
	 */
	error = smb_dictionary_to_urlstring(mutableDict, &newUrlStringM);
	if (error || (newUrlStringM == NULL)) {
		os_log_debug(OS_LOG_DEFAULT, "Failed parsing dictionary, syserr = %s",
					 strerror(error));
		goto WeAreDone;	
	}
	if (urlStringM)
		CFRelease(urlStringM);
	urlStringM = newUrlStringM;
	newUrlStringM = NULL;
	/* smb_dictionary_to_urlstring always uses the SMB scheme */
	SchemeLength = SMB_SCHEME_LEN;
	if (CFStringGetLength(urlStringM) < (maxlen+SchemeLength))
		goto WeAreDone;
	
	/* 
	 * At this point the URL is too big to fit in the mount from name. See if
	 * removing the username will make it fit. 
	 */
	CFDictionaryRemoveValue(mutableDict, kNetFSUserNameKey);
	CFDictionaryRemoveValue(mutableDict, kNetFSPasswordKey);
	
	
	/* 
	 * Recreate the URL from our new dictionary. The old code would not escape
	 * out the share/path, which can cause us issue. Now the from name can look
	 * pretty goofy, but we will always work with alias. Now this was originally
	 * done because carbon would use the last part of the mount from name as the
	 * volume name. We now return the volume name, so this is no longer an issue.
	 */
	error = smb_dictionary_to_urlstring(mutableDict, &newUrlStringM);
	if (error || (newUrlStringM == NULL)) {
		os_log_debug(OS_LOG_DEFAULT, "Removing username failed parsing dictionary, syserr = %s",
					 strerror(error));
		goto WeAreDone;
	}
	if (urlStringM)
		CFRelease(urlStringM);
	urlStringM = newUrlStringM;
	newUrlStringM = NULL;
	
WeAreDone:
	if (urlStringM && (SchemeLength > 0)) {
		/* Remove the scheme, start at the begining */
		CFRange range1 = CFRangeMake(0, SchemeLength);
		CFStringDelete(urlStringM, range1);		
	}
	if (urlStringM)
		CFStringGetCString(urlStringM, fromname, maxlen, kCFStringEncodingUTF8);
	if (error)
		os_log_error(OS_LOG_DEFAULT, "Mount from name is %s, syserr = %s",
					 fromname, strerror(error));
	else
		os_log_debug(OS_LOG_DEFAULT, "Mount from name is %s", fromname);
	
	if (urlStringM)
		CFRelease(urlStringM);
	if (mutableDict)
		CFRelease(mutableDict);	
}

/*
 * See if this is a BTMM address
 */
int isBTMMAddress(CFStringRef serverNameRef)
{
    boolean_t	foundBTMM;
    CFStringRef btmmRef;
    CFStringRef serverNameQual;
    CFIndex serverNameLen, btmmCount, serverCount, btmmIndex, srvIndex;
    CFArrayRef btmmArrRef, serverArrRef;
    CFStringRef btmmTmpRef, serverTmpRef;
    CFComparisonResult res;
    
    foundBTMM = TRUE;
    btmmRef = NULL;
    serverNameQual = NULL;
    btmmArrRef = NULL;
    serverArrRef = NULL;
	
	if (serverNameRef == NULL) {
		os_log_debug(OS_LOG_DEFAULT, "%s: serverNameRef is NULL!", __FUNCTION__);
		foundBTMM = FALSE;
        goto out;
	}
	
    serverNameLen = CFStringGetLength(serverNameRef);
	if (serverNameLen == 0) {
		os_log_debug(OS_LOG_DEFAULT, "%s: serverNameRef len is 0!", __FUNCTION__);
		foundBTMM = FALSE;
        goto out;
	}
    
    /*  Create a copy of the server name, add a trailing '.' */
    /*  if it doesn't already have one. */
    if (CFStringGetCharacterAtIndex(serverNameRef, serverNameLen - 1) == (UniChar)'.') {
        serverNameQual = CFStringCreateCopy(kCFAllocatorDefault, serverNameRef);
    } else {
        serverNameQual = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%@."), serverNameRef);
    }
    
    if (serverNameQual == NULL) {
		foundBTMM = FALSE;
        goto out;
    }
    
    /* Fetch BTMM domain from DynamicStore */
    btmmRef = _CSBackToMyMacCopyDomain();
    if (btmmRef == NULL) {
		foundBTMM = FALSE;
        goto out;
    }
    
    /* Split them into component string arrays */
    btmmArrRef = CFStringCreateArrayBySeparatingStrings(kCFAllocatorDefault, btmmRef, CFSTR("."));
    btmmCount = CFArrayGetCount(btmmArrRef);
    
    serverArrRef = CFStringCreateArrayBySeparatingStrings(kCFAllocatorDefault, serverNameQual, CFSTR("."));
    serverCount = CFArrayGetCount(serverArrRef);
    
    if (btmmCount == 0 || serverCount == 0) {
		foundBTMM = FALSE;
        goto out;
    }
    
    if (btmmCount > serverCount) {
        /* Not a BTMM domain */
		foundBTMM = FALSE;
        goto out;
    }
    
    for (btmmIndex = btmmCount - 1, srvIndex = serverCount - 1; btmmIndex >= 0; btmmIndex--, srvIndex--) {
        btmmTmpRef = CFArrayGetValueAtIndex(btmmArrRef, btmmIndex);
        serverTmpRef = CFArrayGetValueAtIndex(serverArrRef, srvIndex);
        
        res = CFStringCompare(btmmTmpRef, serverTmpRef, kCFCompareCaseInsensitive);
        if (res != kCFCompareEqualTo) {
            /* Not a BTMM domain */
            foundBTMM = FALSE;
            break;
        }
    }
        
	if (foundBTMM == TRUE) {
        os_log_debug(OS_LOG_DEFAULT, "%s: found a btmm address", __FUNCTION__);
	}

out:
    
    // Clean up mem
    if (btmmArrRef != NULL) {
        CFRelease(btmmArrRef);
    }
    if (serverArrRef != NULL) {
        CFRelease(serverArrRef);
    }
    if (btmmRef != NULL) {
        CFRelease(btmmRef);
    }
    if (serverNameQual !=NULL) {
        CFRelease(serverNameQual);
    }
    
	return (foundBTMM);
}
