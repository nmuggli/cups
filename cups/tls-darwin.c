/*
 * "$Id$"
 *
 * TLS support code for CUPS on OS X.
 *
 * Copyright 2007-2013 by Apple Inc.
 * Copyright 1997-2007 by Easy Software Products, all rights reserved.
 *
 * These coded instructions, statements, and computer programs are the
 * property of Apple Inc. and are protected by Federal copyright
 * law.  Distribution and use rights are outlined in the file "LICENSE.txt"
 * which should have been included with this file.  If this file is
 * file is missing or damaged, see the license at "http://www.cups.org/".
 *
 * This file is subject to the Apple OS-Developed Software exception.
 */

/**** This file is included from http.c ****/

/*
 * Include necessary headers...
 */

#include <spawn.h>

extern char **environ;


/*
 * Local globals...
 */

static int		tls_auto_create = 0;
					/* Auto-create self-signed certs? */
static char		*tls_common_name = NULL;
					/* Default common name */
static SecKeychainRef	tls_keychain = NULL;
					/* Server cert keychain */
static char		*tls_keypath = NULL;
					/* Server cert keychain path */
static _cups_mutex_t	tls_mutex = _CUPS_MUTEX_INITIALIZER;
					/* Mutex for keychain/certs */


/*
 * Local functions...
 */

static CFArrayRef	http_cdsa_copy_server(const char *common_name);
static OSStatus		http_cdsa_read(SSLConnectionRef connection, void *data, size_t *dataLength);
static OSStatus		http_cdsa_write(SSLConnectionRef connection, const void *data, size_t *dataLength);


/*
 * 'cupsMakeServerCredentials()' - Make a self-signed certificate and private key pair.
 *
 * @since CUPS 2.0@
 */

int					/* O - 1 on success, 0 on failure */
cupsMakeServerCredentials(
    const char *path,			/* I - Path to keychain/directory */
    const char *common_name,		/* I - Common name */
    int        num_alt_names,		/* I - Number of subject alternate names */
    const char **alt_names,		/* I - Subject Alternate Names */
    time_t     expiration_date)		/* I - Expiration date */
{
#ifdef HAVE_SECGENERATESELFSIGNEDCERTIFICATE
  int			status = 0;	/* Return status */
  OSStatus		err;		/* Error code (if any) */
  CFStringRef		cfcommon_name = NULL;
					/* CF string for server name */
  SecIdentityRef	ident = NULL;	/* Identity */
  SecKeyRef		publicKey = NULL,
					/* Public key */
			privateKey = NULL;
					/* Private key */
  CFMutableDictionaryRef keyParams = NULL;
					/* Key generation parameters */


  (void)num_alt_names;
  (void)alt_names;
  (void)expiration_date;

  cfcommon_name = CFStringCreateWithCString(kCFAllocatorDefault, common_name,
                                           kCFStringEncodingUTF8);
  if (!cfcommon_name)
    goto cleanup;

 /*
  * Create a public/private key pair...
  */

  keyParams = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
  if (!keyParams)
    goto cleanup;

  CFDictionaryAddValue(keyParams, kSecAttrKeyType, kSecAttrKeyTypeRSA);
  CFDictionaryAddValue(keyParams, kSecAttrKeySizeInBits, CFSTR("2048"));
  CFDictionaryAddValue(keyParams, kSecAttrLabel,
                       CFSTR("CUPS Self-Signed Certificate"));

  err = SecKeyGeneratePair(keyParams, &publicKey, &privateKey);
  if (err != noErr)
    goto cleanup;

 /*
  * Create a self-signed certificate using the public/private key pair...
  */

  CFIndex	usageInt = kSecKeyUsageAll;
  CFNumberRef	usage = CFNumberCreate(alloc, kCFNumberCFIndexType, &usageInt);
  CFDictionaryRef certParams = CFDictionaryCreateMutable(kCFAllocatorDefault,
kSecCSRBasicContraintsPathLen, CFINT(0), kSecSubjectAltName, cfcommon_name, kSecCertificateKeyUsage, usage, NULL, NULL);
  CFRelease(usage);

  const void	*ca_o[] = { kSecOidOrganization, CFSTR("") };
  const void	*ca_cn[] = { kSecOidCommonName, cfcommon_name };
  CFArrayRef	ca_o_dn = CFArrayCreate(kCFAllocatorDefault, ca_o, 2, NULL);
  CFArrayRef	ca_cn_dn = CFArrayCreate(kCFAllocatorDefault, ca_cn, 2, NULL);
  const void	*ca_dn_array[2];

  ca_dn_array[0] = CFArrayCreate(kCFAllocatorDefault, (const void **)&ca_o_dn, 1, NULL);
  ca_dn_array[1] = CFArrayCreate(kCFAllocatorDefault, (const void **)&ca_cn_dn, 1, NULL);

  CFArrayRef	subject = CFArrayCreate(kCFAllocatorDefault, ca_dn_array, 2, NULL);
  SecCertificateRef cert = SecGenerateSelfSignedCertificate(subject, certParams, publicKey, privateKey);
  CFRelease(subject);
  CFRelease(certParams);

  if (!cert)
    goto cleanup;

  ident = SecIdentityCreate(kCFAllocatorDefault, cert, privateKey);

  if (ident)
    status = 1;

  /*
   * Cleanup and return...
   */

cleanup:

  if (cfcommon_name)
    CFRelease(cfcommon_name);

  if (keyParams)
    CFRelease(keyParams);

  if (ident)
    CFRelease(ident);

  if (cert)
    CFRelease(cert);

  if (publicKey)
    CFRelease(publicKey);

  if (privateKey)
    CFRelease(publicKey);

  return (status);

#else /* !HAVE_SECGENERATESELFSIGNEDCERTIFICATE */
  int		pid,			/* Process ID of command */
		status;			/* Status of command */
  char		command[1024],		/* Command */
		*argv[4],		/* Command-line arguments */
		keychain[1024],		/* Keychain argument */
		infofile[1024];		/* Type-in information for cert */
  cups_file_t	*fp;			/* Seed/info file */


  (void)num_alt_names;
  (void)alt_names;
  (void)expiration_date;

 /*
  * Run the "certtool" command to generate a self-signed certificate...
  */

  if (!cupsFileFind("certtool", getenv("PATH"), 1, command, sizeof(command)))
    return (-1);

 /*
  * Create a file with the certificate information fields...
  *
  * Note: This assumes that the default questions are asked by the certtool
  * command...
  */

 if ((fp = cupsTempFile2(infofile, sizeof(infofile))) == NULL)
    return (-1);

  cupsFilePrintf(fp,
                 "CUPS Self-Signed Certificate\n"
		 			/* Enter key and certificate label */
                 "r\n"			/* Generate RSA key pair */
                 "2048\n"		/* Key size in bits */
                 "y\n"			/* OK (y = yes) */
                 "b\n"			/* Usage (b=signing/encryption) */
                 "s\n"			/* Sign with SHA1 */
                 "y\n"			/* OK (y = yes) */
                 "%s\n"			/* Common name */
                 "\n"			/* Country (default) */
                 "\n"			/* Organization (default) */
                 "\n"			/* Organizational unit (default) */
                 "\n"			/* State/Province (default) */
                 "\n"			/* Email address */
                 "y\n",			/* OK (y = yes) */
        	 common_name);
  cupsFileClose(fp);

  snprintf(keychain, sizeof(keychain), "k=%s", path);

  argv[0] = "certtool";
  argv[1] = "c";
  argv[2] = keychain;
  argv[3] = NULL;

  posix_spawn_file_actions_t actions;	/* File actions */

  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addclose(&actions, 0);
  posix_spawn_file_actions_addopen(&actions, 0, infofile, O_RDONLY, 0);

  if (posix_spawn(&pid, command, &actions, NULL, argv, environ))
  {
    unlink(infofile);
    return (-1);
  }

  posix_spawn_file_actions_destroy(&actions);

  unlink(infofile);

  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR)
    {
      status = -1;
      break;
    }

  return (!status);
#endif /* HAVE_SECGENERATESELFSIGNEDCERTIFICATE */
}


/*
 * 'cupsSetServerCredentials()' - Set the default server credentials.
 *
 * Note: The server credentials are used by all threads in the running process.
 * This function is threadsafe.
 *
 * @since CUPS 2.0@
 */

int					/* O - 1 on success, 0 on failure */
cupsSetServerCredentials(
    const char *path,			/* I - Path to keychain/directory */
    const char *common_name,		/* I - Default common name for server */
    int        auto_create)		/* I - 1 = automatically create self-signed certificates */
{
  SecKeychainRef	keychain = NULL;/* Temporary keychain */


  if (SecKeychainOpen(path, &keychain) != noErr)
  {
    /* TODO: Set cups last error string */
    return (0);
  }

  _cupsMutexLock(&tls_mutex);

 /*
  * Close any keychain that is currently open...
  */

  if (tls_keychain)
    CFRelease(tls_keychain);

  if (tls_keypath)
    _cupsStrFree(tls_keypath);

  if (tls_common_name)
    _cupsStrFree(tls_common_name);

 /*
  * Save the new keychain...
  */

  tls_keychain    = keychain;
  tls_keypath     = _cupsStrAlloc(path);
  tls_auto_create = auto_create;
  tls_common_name = _cupsStrAlloc(common_name);

  _cupsMutexUnlock(&tls_mutex);

  return (1);
}


/*
 * 'httpCopyCredentials()' - Copy the credentials associated with the peer in
 *                           an encrypted connection.
 *
 * @since CUPS 1.5/OS X 10.7@
 */

int					/* O - Status of call (0 = success) */
httpCopyCredentials(
    http_t	 *http,			/* I - Connection to server */
    cups_array_t **credentials)		/* O - Array of credentials */
{
  OSStatus		error;		/* Error code */
  SecTrustRef		peerTrust;	/* Peer trust reference */
  CFIndex		count;		/* Number of credentials */
  SecCertificateRef	secCert;	/* Certificate reference */
  CFDataRef		data;		/* Certificate data */
  int			i;		/* Looping var */


  if (credentials)
    *credentials = NULL;

  if (!http || !http->tls || !credentials)
    return (-1);

  if (!(error = SSLCopyPeerTrust(http->tls, &peerTrust)) && peerTrust)
  {
    if ((*credentials = cupsArrayNew(NULL, NULL)) != NULL)
    {
      count = SecTrustGetCertificateCount(peerTrust);

      for (i = 0; i < count; i ++)
      {
	secCert = SecTrustGetCertificateAtIndex(peerTrust, i);
	if ((data = SecCertificateCopyData(secCert)))
	{
	  httpAddCredential(*credentials, CFDataGetBytePtr(data),
	                    CFDataGetLength(data));
	  CFRelease(data);
	}
      }
    }

    CFRelease(peerTrust);
  }

  return (error);
}


/*
 * '_httpCreateCredentials()' - Create credentials in the internal format.
 */

http_tls_credentials_t			/* O - Internal credentials */
_httpCreateCredentials(
    cups_array_t *credentials)		/* I - Array of credentials */
{
  CFMutableArrayRef	peerCerts;	/* Peer credentials reference */
  SecCertificateRef	secCert;	/* Certificate reference */
  CFDataRef		data;		/* Credential data reference */
  http_credential_t	*credential;	/* Credential data */


  if (!credentials)
    return (NULL);

  if ((peerCerts = CFArrayCreateMutable(kCFAllocatorDefault,
				        cupsArrayCount(credentials),
				        &kCFTypeArrayCallBacks)) == NULL)
    return (NULL);

  for (credential = (http_credential_t *)cupsArrayFirst(credentials);
       credential;
       credential = (http_credential_t *)cupsArrayNext(credentials))
  {
    if ((data = CFDataCreate(kCFAllocatorDefault, credential->data,
			     credential->datalen)))
    {
      if ((secCert = SecCertificateCreateWithData(kCFAllocatorDefault, data))
              != NULL)
      {
	CFArrayAppendValue(peerCerts, secCert);
	CFRelease(secCert);
      }

      CFRelease(data);
    }
  }

  return (peerCerts);
}


/*
 * 'httpCredentialsAreTrusted()' - Return whether the credentials are trusted.
 *
 * @since CUPS 2.0@
 */

int					/* O - 1 if trusted, 0 if not/unknown */
httpCredentialsAreTrusted(
    cups_array_t *credentials)		/* I - Credentials */
{
  (void)credentials;

  return (0);
}


/*
 * 'httpCredentialsGetExpiration()' - Return the expiration date of the credentials.
 *
 * @since CUPS 2.0@
 */

time_t					/* O - Expiration date of credentials */
httpCredentialsGetExpiration(
    cups_array_t *credentials)		/* I - Credentials */
{
  (void)credentials;

  return (0);
}


/*
 * 'httpCredentialsIsValidName()' - Return whether the credentials are valid for the given name.
 *
 * @since CUPS 2.0@
 */

int					/* O - 1 if valid, 0 otherwise */
httpCredentialsIsValidName(
    cups_array_t *credentials,		/* I - Credentials */
    const char   *common_name)		/* I - Name to check */
{
  (void)credentials;
  (void)common_name;

  return (0);
}


/*
 * 'httpCredentialsString()' - Return a string representing the credentials.
 *
 * @since CUPS 2.0@
 */

size_t					/* O - Total size of credentials string */
httpCredentialsString(
    cups_array_t *credentials,		/* I - Credentials */
    char         *buffer,		/* I - Buffer or @code NULL@ */
    size_t       bufsize)		/* I - Size of buffer */
{
  (void)credentials;

  if (buffer && bufsize > 0)
    *buffer = '\0';

  return (1);
}


/*
 * '_httpFreeCredentials()' - Free internal credentials.
 */

void
_httpFreeCredentials(
    http_tls_credentials_t credentials)	/* I - Internal credentials */
{
  if (!credentials)
    return;

  CFRelease(credentials);
}


/*
 * 'httpLoadCredentials()' - Load X.509 credentials from a keychain file.
 *
 * @since CUPS 2.0@
 */

int					/* O - 0 on success, -1 on error */
httpLoadCredentials(
    const char   *path,			/* I  - Keychain/PKCS#12 path */
    cups_array_t **credentials,		/* IO - Credentials */
    const char   *common_name)		/* I  - Common name for credentials */
{
  (void)path;
  (void)credentials;
  (void)common_name;

  return (-1);

#if 0
  OSStatus		err;		/* Error info */
  SecKeychainRef	keychain = NULL;/* Keychain reference */
  SecIdentitySearchRef	search = NULL;	/* Search reference */
  SecIdentityRef	identity = NULL;/* Identity */
  CFArrayRef		certificates = NULL;
					/* Certificate array */
  SecPolicyRef		policy = NULL;	/* Policy ref */
  CFStringRef		cfcommon_name = NULL;
					/* Server name */
  CFMutableDictionaryRef query = NULL;	/* Query qualifiers */
  CFArrayRef		list = NULL;	/* Keychain list */


  if ((err = SecKeychainOpen(path, &keychain)))
    goto cleanup;

  cfcommon_name = CFStringCreateWithCString(kCFAllocatorDefault, common_name, kCFStringEncodingUTF8);

  policy = SecPolicyCreateSSL(1, cfcommon_name);

  if (cfcommon_name)
    CFRelease(cfcommon_name);

  if (!policy)
    goto cleanup;

  if (!(query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)))
    goto cleanup;

  list = CFArrayCreate(kCFAllocatorDefault, (const void **)&keychain, 1,
                       &kCFTypeArrayCallBacks);

  CFDictionaryAddValue(query, kSecClass, kSecClassIdentity);
  CFDictionaryAddValue(query, kSecMatchPolicy, policy);
  CFDictionaryAddValue(query, kSecReturnRef, kCFBooleanTrue);
  CFDictionaryAddValue(query, kSecMatchLimit, kSecMatchLimitOne);
  CFDictionaryAddValue(query, kSecMatchSearchList, list);

  CFRelease(list);

  err = SecItemCopyMatching(query, (CFTypeRef *)&identity);

  if (err)
    goto cleanup;

  if (CFGetTypeID(identity) != SecIdentityGetTypeID())
    goto cleanup;

  if ((certificates = CFArrayCreate(NULL, (const void **)&identity, 1, &kCFTypeArrayCallBacks)) == NULL)
    goto cleanup;

  cleanup :

  if (keychain)
    CFRelease(keychain);
  if (search)
    CFRelease(search);
  if (identity)
    CFRelease(identity);

  if (policy)
    CFRelease(policy);
  if (query)
    CFRelease(query);

  return (certificates);
#endif /* 0 */
}


#if 0
/*
 * 'cupsMakeCredentials()' - Create self-signed credentials for the given
 *                           name.
 *
 * @since CUPS 2.0@
 */

int					/* O - 0 on success, -1 on error */
cupsMakeCredentials(
    const char   *path,			/* I - Keychain/PKCS#12 path */
    cups_array_t **credentials,		/* O - Credentials */
    const char   *common_name)		/* I - Common name for X.509 cert */
{
#    ifdef HAVE_SECGENERATESELFSIGNEDCERTIFICATE
  int			status = -1;	/* Return status */
  OSStatus		err;		/* Error code (if any) */
  CFStringRef		cfcommon_name = NULL;
					/* CF string for server name */
  SecIdentityRef	ident = NULL;	/* Identity */
  SecKeyRef		publicKey = NULL,
					/* Public key */
			privateKey = NULL;
					/* Private key */
  CFMutableDictionaryRef keyParams = NULL;
					/* Key generation parameters */


  if (credentials)
    *credentials = NULL;

  cfcommon_name = CFStringCreateWithCString(kCFAllocatorDefault, servername,
                                           kCFStringEncodingUTF8);
  if (!cfcommon_name)
    goto cleanup;

 /*
  * Create a public/private key pair...
  */

  keyParams = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
  if (!keyParams)
    goto cleanup;

  CFDictionaryAddValue(keyParams, kSecAttrKeyType, kSecAttrKeyTypeRSA);
  CFDictionaryAddValue(keyParams, kSecAttrKeySizeInBits, CFSTR("2048"));
  CFDictionaryAddValue(keyParams, kSecAttrLabel,
                       CFSTR("CUPS Self-Signed Certificate"));

  err = SecKeyGeneratePair(keyParams, &publicKey, &privateKey);
  if (err != noErr)
    goto cleanup;

 /*
  * Create a self-signed certificate using the public/private key pair...
  */

  CFIndex	usageInt = kSecKeyUsageAll;
  CFNumberRef	usage = CFNumberCreate(alloc, kCFNumberCFIndexType, &usageInt);
  CFDictionaryRef certParams = CFDictionaryCreateMutable(kCFAllocatorDefault,
kSecCSRBasicContraintsPathLen, CFINT(0), kSecSubjectAltName, cfcommon_name, kSecCertificateKeyUsage, usage, NULL, NULL);
  CFRelease(usage);

  const void	*ca_o[] = { kSecOidOrganization, CFSTR("") };
  const void	*ca_cn[] = { kSecOidCommonName, cfcommon_name };
  CFArrayRef	ca_o_dn = CFArrayCreate(kCFAllocatorDefault, ca_o, 2, NULL);
  CFArrayRef	ca_cn_dn = CFArrayCreate(kCFAllocatorDefault, ca_cn, 2, NULL);
  const void	*ca_dn_array[2];

  ca_dn_array[0] = CFArrayCreate(kCFAllocatorDefault, (const void **)&ca_o_dn, 1, NULL);
  ca_dn_array[1] = CFArrayCreate(kCFAllocatorDefault, (const void **)&ca_cn_dn, 1, NULL);

  CFArrayRef	subject = CFArrayCreate(kCFAllocatorDefault, ca_dn_array, 2, NULL);
  SecCertificateRef cert = SecGenerateSelfSignedCertificate(subject, certParams, publicKey, privateKey);
  CFRelease(subject);
  CFRelease(certParams);

  if (!cert)
    goto cleanup;

  ident = SecIdentityCreate(kCFAllocatorDefault, cert, privateKey);

  if (ident)
    status = 0;

  /*
   * Cleanup and return...
   */

cleanup:

  if (cfcommon_name)
    CFRelease(cfcommon_name);

  if (keyParams)
    CFRelease(keyParams);

  if (ident)
    CFRelease(ident);

  if (cert)
    CFRelease(cert);

  if (publicKey)
    CFRelease(publicKey);

  if (privateKey)
    CFRelease(publicKey);

  return (status);

#    else /* !HAVE_SECGENERATESELFSIGNEDCERTIFICATE */
  int		pid,			/* Process ID of command */
		status;			/* Status of command */
  char		command[1024],		/* Command */
		*argv[4],		/* Command-line arguments */
		keychain[1024],		/* Keychain argument */
		infofile[1024];		/* Type-in information for cert */
  cups_file_t	*fp;			/* Seed/info file */


 /*
  * Run the "certtool" command to generate a self-signed certificate...
  */

  if (!cupsFileFind("certtool", getenv("PATH"), 1, command, sizeof(command)))
    return (-1);

 /*
  * Create a file with the certificate information fields...
  *
  * Note: This assumes that the default questions are asked by the certtool
  * command...
  */

 if ((fp = cupsTempFile2(infofile, sizeof(infofile))) == NULL)
    return (-1);

  cupsFilePrintf(fp,
                 "%s\n"			/* Enter key and certificate label */
                 "r\n"			/* Generate RSA key pair */
                 "2048\n"		/* Key size in bits */
                 "y\n"			/* OK (y = yes) */
                 "b\n"			/* Usage (b=signing/encryption) */
                 "s\n"			/* Sign with SHA1 */
                 "y\n"			/* OK (y = yes) */
                 "%s\n"			/* Common name */
                 "\n"			/* Country (default) */
                 "\n"			/* Organization (default) */
                 "\n"			/* Organizational unit (default) */
                 "\n"			/* State/Province (default) */
                 "%s\n"			/* Email address */
                 "y\n",			/* OK (y = yes) */
        	 common_name, common_name, "");
  cupsFileClose(fp);

  snprintf(keychain, sizeof(keychain), "k=%s", path);

  argv[0] = "certtool";
  argv[1] = "c";
  argv[2] = keychain;
  argv[3] = NULL;

  posix_spawn_file_actions_t actions;	/* File actions */

  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addclose(&actions, 0);
  posix_spawn_file_actions_addopen(&actions, 0, infofile, O_RDONLY, 0);

  if (posix_spawn(&pid, command, &actions, NULL, argv, environ))
  {
    unlink(infofile);
    return (-1);
  }

  posix_spawn_file_actions_destroy(&actions);

  unlink(infofile);

  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR)
    {
      status = -1;
      break;
    }

  if (status)
    return (-1);

#    endif /* HAVE_SECGENERATESELFSIGNEDCERTIFICATE */

  return (httpLoadCredentials(path, credentials, common_name));
}
#endif /* 0 */


/*
 * 'httpSaveCredentials()' - Save X.509 credentials to a keychain file.
 *
 * @since CUPS 2.0@
 */

int					/* O - -1 on error, 0 on success */
httpSaveCredentials(
    const char   *path,			/* I - Keychain/PKCS#12 path */
    cups_array_t *credentials,		/* I - Credentials */
    const char   *common_name)		/* I - Common name for credentials */
{
  (void)path;
  (void)credentials;
  (void)common_name;

  return (-1);
}


/*
 * 'http_cdsa_copy_server()' - Find and copy server credentials from the keychain.
 */

static CFArrayRef			/* O - Array of certificates or NULL */
http_cdsa_copy_server(
    const char *common_name)		/* I - Server's hostname */
{
  OSStatus		err;		/* Error info */
  SecIdentitySearchRef	search = NULL;	/* Search reference */
  SecIdentityRef	identity = NULL;/* Identity */
  CFArrayRef		certificates = NULL;
					/* Certificate array */
  SecPolicyRef		policy = NULL;	/* Policy ref */
  CFStringRef		cfcommon_name = NULL;
					/* Server name */
  CFMutableDictionaryRef query = NULL;	/* Query qualifiers */
  CFArrayRef		list = NULL;	/* Keychain list */


  cfcommon_name = CFStringCreateWithCString(kCFAllocatorDefault, common_name, kCFStringEncodingUTF8);

  policy = SecPolicyCreateSSL(1, cfcommon_name);

  if (cfcommon_name)
    CFRelease(cfcommon_name);

  if (!policy)
    goto cleanup;

  if (!(query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)))
    goto cleanup;

  list = CFArrayCreate(kCFAllocatorDefault, (const void **)&tls_keychain, 1, &kCFTypeArrayCallBacks);

  CFDictionaryAddValue(query, kSecClass, kSecClassIdentity);
  CFDictionaryAddValue(query, kSecMatchPolicy, policy);
  CFDictionaryAddValue(query, kSecReturnRef, kCFBooleanTrue);
  CFDictionaryAddValue(query, kSecMatchLimit, kSecMatchLimitOne);
  CFDictionaryAddValue(query, kSecMatchSearchList, list);

  CFRelease(list);

  err = SecItemCopyMatching(query, (CFTypeRef *)&identity);

  if (err)
    goto cleanup;

  if (CFGetTypeID(identity) != SecIdentityGetTypeID())
    goto cleanup;

  if ((certificates = CFArrayCreate(NULL, (const void **)&identity, 1, &kCFTypeArrayCallBacks)) == NULL)
    goto cleanup;

  cleanup :

  if (search)
    CFRelease(search);
  if (identity)
    CFRelease(identity);

  if (policy)
    CFRelease(policy);
  if (query)
    CFRelease(query);

  return (certificates);
}


/*
 * 'http_cdsa_read()' - Read function for the CDSA library.
 */

static OSStatus				/* O  - -1 on error, 0 on success */
http_cdsa_read(
    SSLConnectionRef connection,	/* I  - SSL/TLS connection */
    void             *data,		/* I  - Data buffer */
    size_t           *dataLength)	/* IO - Number of bytes */
{
  OSStatus	result;			/* Return value */
  ssize_t	bytes;			/* Number of bytes read */
  http_t	*http;			/* HTTP connection */


  http = (http_t *)connection;

  if (!http->blocking)
  {
   /*
    * Make sure we have data before we read...
    */

    while (!_httpWait(http, http->wait_value, 0))
    {
      if (http->timeout_cb && (*http->timeout_cb)(http, http->timeout_data))
	continue;

      http->error = ETIMEDOUT;
      return (-1);
    }
  }

  do
  {
    bytes = recv(http->fd, data, *dataLength, 0);
  }
  while (bytes == -1 && (errno == EINTR || errno == EAGAIN));

  if (bytes == *dataLength)
  {
    result = 0;
  }
  else if (bytes > 0)
  {
    *dataLength = bytes;
    result = errSSLWouldBlock;
  }
  else
  {
    *dataLength = 0;

    if (bytes == 0)
      result = errSSLClosedGraceful;
    else if (errno == EAGAIN)
      result = errSSLWouldBlock;
    else
      result = errSSLClosedAbort;
  }

  return (result);
}


/*
 * 'http_cdsa_write()' - Write function for the CDSA library.
 */

static OSStatus				/* O  - -1 on error, 0 on success */
http_cdsa_write(
    SSLConnectionRef connection,	/* I  - SSL/TLS connection */
    const void       *data,		/* I  - Data buffer */
    size_t           *dataLength)	/* IO - Number of bytes */
{
  OSStatus	result;			/* Return value */
  ssize_t	bytes;			/* Number of bytes read */
  http_t	*http;			/* HTTP connection */


  http = (http_t *)connection;

  do
  {
    bytes = write(http->fd, data, *dataLength);
  }
  while (bytes == -1 && (errno == EINTR || errno == EAGAIN));

  if (bytes == *dataLength)
  {
    result = 0;
  }
  else if (bytes >= 0)
  {
    *dataLength = bytes;
    result = errSSLWouldBlock;
  }
  else
  {
    *dataLength = 0;

    if (errno == EAGAIN)
      result = errSSLWouldBlock;
    else
      result = errSSLClosedAbort;
  }

  return (result);
}


/*
 * 'http_tls_initialize()' - Initialize the TLS stack.
 */

static void
http_tls_initialize(void)
{
 /*
  * Nothing to do...
  */
}


/*
 * 'http_tls_pending()' - Return the number of pending TLS-encrypted bytes.
 */

static size_t
http_tls_pending(http_t *http)		/* I - HTTP connection */
{
  size_t bytes;				/* Bytes that are available */


  if (!SSLGetBufferedReadSize(http->tls, &bytes))
    return (bytes);

  return (0);
}


/*
 * 'http_tls_read()' - Read from a SSL/TLS connection.
 */

static int				/* O - Bytes read */
http_tls_read(http_t *http,		/* I - HTTP connection */
	      char   *buf,		/* I - Buffer to store data */
	      int    len)		/* I - Length of buffer */
{
  int		result;			/* Return value */
  OSStatus	error;			/* Error info */
  size_t	processed;		/* Number of bytes processed */


  error = SSLRead(http->tls, buf, len, &processed);
  DEBUG_printf(("6http_tls_read: error=%d, processed=%d", (int)error,
                (int)processed));
  switch (error)
  {
    case 0 :
	result = (int)processed;
	break;

    case errSSLWouldBlock :
	if (processed)
	  result = (int)processed;
	else
	{
	  result = -1;
	  errno  = EINTR;
	}
	break;

    case errSSLClosedGraceful :
    default :
	if (processed)
	  result = (int)processed;
	else
	{
	  result = -1;
	  errno  = EPIPE;
	}
	break;
  }

  return (result);
}


/*
 * 'http_tls_set_credentials()' - Set the TLS credentials.
 */

static int				/* O - Status of connection */
http_tls_set_credentials(http_t *http)	/* I - HTTP connection */
{
  _cups_globals_t *cg = _cupsGlobals();	/* Pointer to library globals */
  OSStatus		error = 0;	/* Error code */
  http_tls_credentials_t credentials = NULL;
					/* TLS credentials */


  DEBUG_printf(("7http_tls_set_credentials(%p)", http));

 /*
  * Prefer connection specific credentials...
  */

  if ((credentials = http->tls_credentials) == NULL)
    credentials = cg->tls_credentials;

  if (credentials)
  {
    error = SSLSetCertificate(http->tls, credentials);
    DEBUG_printf(("4http_tls_set_credentials: SSLSetCertificate, error=%d",
		  (int)error));
  }
  else
    DEBUG_puts("4http_tls_set_credentials: No credentials to set.");

  return (error);
}


/*
 * 'http_tls_start()' - Set up SSL/TLS support on a connection.
 */

static int				/* O - 0 on success, -1 on failure */
http_tls_start(http_t *http)		/* I - HTTP connection */
{
  char			hostname[256],	/* Hostname */
			*hostptr;	/* Pointer into hostname */
  _cups_globals_t	*cg = _cupsGlobals();
					/* Pointer to library globals */
  OSStatus		error;		/* Error code */
  const char		*message = NULL;/* Error message */
  cups_array_t		*credentials;	/* Credentials array */
  cups_array_t		*names;		/* CUPS distinguished names */
  CFArrayRef		dn_array;	/* CF distinguished names array */
  CFIndex		count;		/* Number of credentials */
  CFDataRef		data;		/* Certificate data */
  int			i;		/* Looping var */
  http_credential_t	*credential;	/* Credential data */


  DEBUG_printf(("7http_tls_start(http=%p)", http));

  if (http->mode == _HTTP_MODE_SERVER && !tls_keychain)
  {
    DEBUG_puts("4http_tls_start: cupsSetServerCredentials not called.");
    http->error  = errno = EINVAL;
    http->status = HTTP_STATUS_ERROR;
    _cupsSetError(IPP_STATUS_ERROR_INTERNAL, _("Server credentials not set."), 1);

    return (-1);
  }

  if ((http->tls = SSLCreateContext(kCFAllocatorDefault, http->mode == _HTTP_MODE_CLIENT ? kSSLClientSide : kSSLServerSide, kSSLStreamType)) == NULL)
  {
    DEBUG_puts("4http_tls_start: SSLCreateContext failed.");
    http->error  = errno = ENOMEM;
    http->status = HTTP_STATUS_ERROR;
    _cupsSetHTTPError(HTTP_STATUS_ERROR);

    return (-1);
  }

  error = SSLSetConnection(http->tls, http);
  DEBUG_printf(("4http_tls_start: SSLSetConnection, error=%d", (int)error));

  if (!error)
  {
    error = SSLSetIOFuncs(http->tls, http_cdsa_read, http_cdsa_write);
    DEBUG_printf(("4http_tls_start: SSLSetIOFuncs, error=%d", (int)error));
  }

  if (!error)
  {
    error = SSLSetSessionOption(http->tls, kSSLSessionOptionBreakOnServerAuth,
                                true);
    DEBUG_printf(("4http_tls_start: SSLSetSessionOption, error=%d",
                  (int)error));
  }

  if (!error && http->mode == _HTTP_MODE_CLIENT)
  {
   /*
    * Client: set client-side credentials, if any...
    */

    if (cg->client_cert_cb)
    {
      error = SSLSetSessionOption(http->tls,
				  kSSLSessionOptionBreakOnCertRequested, true);
      DEBUG_printf(("4http_tls_start: kSSLSessionOptionBreakOnCertRequested, "
                    "error=%d", (int)error));
    }
    else
    {
      error = http_tls_set_credentials(http);
      DEBUG_printf(("4http_tls_start: http_tls_set_credentials, error=%d",
                    (int)error));
    }
  }
  else if (!error)
  {
   /*
    * Server: find/create a certificate for TLS...
    */

    if (http->fields[HTTP_FIELD_HOST][0])
    {
     /*
      * Use hostname for TLS upgrade...
      */

      strlcpy(hostname, http->fields[HTTP_FIELD_HOST], sizeof(hostname));
    }
    else
    {
     /*
      * Resolve hostname from connection address...
      */

      http_addr_t	addr;		/* Connection address */
      socklen_t		addrlen;	/* Length of address */

      addrlen = sizeof(addr);
      if (getsockname(http->fd, (struct sockaddr *)&addr, &addrlen))
      {
	DEBUG_printf(("4http_tls_start: Unable to get socket address: %s", strerror(errno)));
	hostname[0] = '\0';
      }
      else if (httpAddrLocalhost(&addr))
	hostname[0] = '\0';
      else
	httpAddrString(&addr, hostname, sizeof(hostname));
    }

    if (hostname[0])
      http->tls_credentials = http_cdsa_copy_server(hostname);
    else if (tls_common_name)
      http->tls_credentials = http_cdsa_copy_server(tls_common_name);

    if (!http->tls_credentials && tls_auto_create && (hostname[0] || tls_common_name))
    {
      DEBUG_printf(("4http_tls_start: Auto-create credentials for \"%s\".", hostname[0] ? hostname : tls_common_name));

      if (!cupsMakeServerCredentials(tls_keypath, hostname[0] ? hostname : tls_common_name, 0, NULL, time(NULL) + 365 * 86400))
      {
	DEBUG_puts("4http_tls_start: cupsMakeServerCredentials failed.");
	http->error  = errno = EINVAL;
	http->status = HTTP_STATUS_ERROR;
	_cupsSetError(IPP_STATUS_ERROR_INTERNAL, _("Unable to create server credentials."), 1);

	return (-1);
      }

      http->tls_credentials = http_cdsa_copy_server(hostname[0] ? hostname : tls_common_name);
    }

    if (!http->tls_credentials)
    {
      DEBUG_puts("4http_tls_start: Unable to find server credentials.");
      http->error  = errno = EINVAL;
      http->status = HTTP_STATUS_ERROR;
      _cupsSetError(IPP_STATUS_ERROR_INTERNAL, _("Unable to find server credentials."), 1);

      return (-1);
    }

    error = SSLSetCertificate(http->tls, http->tls_credentials);

    DEBUG_printf(("4http_tls_start: SSLSetCertificate, error=%d", (int)error));
  }

  DEBUG_printf(("4http_tls_start: tls_credentials=%p", http->tls_credentials));

 /*
  * Let the server know which hostname/domain we are trying to connect to
  * in case it wants to serve up a certificate with a matching common name.
  */

  if (!error && http->mode == _HTTP_MODE_CLIENT)
  {
   /*
    * Client: get the hostname to use for TLS...
    */

    if (httpAddrLocalhost(http->hostaddr))
    {
      strlcpy(hostname, "localhost", sizeof(hostname));
    }
    else
    {
     /*
      * Otherwise make sure the hostname we have does not end in a trailing dot.
      */

      strlcpy(hostname, http->hostname, sizeof(hostname));
      if ((hostptr = hostname + strlen(hostname) - 1) >= hostname &&
	  *hostptr == '.')
	*hostptr = '\0';
    }

    error = SSLSetPeerDomainName(http->tls, hostname, strlen(hostname));

    DEBUG_printf(("4http_tls_start: SSLSetPeerDomainName, error=%d", (int)error));
  }

  if (!error)
  {
    int done = 0;			/* Are we done yet? */

    while (!error && !done)
    {
      error = SSLHandshake(http->tls);

      DEBUG_printf(("4http_tls_start: SSLHandshake returned %d.", (int)error));

      switch (error)
      {
	case noErr :
	    done = 1;
	    break;

	case errSSLWouldBlock :
	    error = noErr;		/* Force a retry */
	    usleep(1000);		/* in 1 millisecond */
	    break;

	case errSSLServerAuthCompleted :
	    error = 0;
	    if (cg->server_cert_cb)
	    {
	      error = httpCopyCredentials(http, &credentials);
	      if (!error)
	      {
		error = (cg->server_cert_cb)(http, http->tls, credentials,
					     cg->server_cert_data);
		httpFreeCredentials(credentials);
	      }

	      DEBUG_printf(("4http_tls_start: Server certificate callback "
	                    "returned %d.", (int)error));
	    }
	    break;

	case errSSLClientCertRequested :
	    error = 0;

	    if (cg->client_cert_cb)
	    {
	      names = NULL;
	      if (!(error = SSLCopyDistinguishedNames(http->tls, &dn_array)) &&
		  dn_array)
	      {
		if ((names = cupsArrayNew(NULL, NULL)) != NULL)
		{
		  for (i = 0, count = CFArrayGetCount(dn_array); i < count; i++)
		  {
		    data = (CFDataRef)CFArrayGetValueAtIndex(dn_array, i);

		    if ((credential = malloc(sizeof(*credential))) != NULL)
		    {
		      credential->datalen = CFDataGetLength(data);
		      if ((credential->data = malloc(credential->datalen)))
		      {
			memcpy((void *)credential->data, CFDataGetBytePtr(data),
			       credential->datalen);
			cupsArrayAdd(names, credential);
		      }
		      else
		        free(credential);
		    }
		  }
		}

		CFRelease(dn_array);
	      }

	      if (!error)
	      {
		error = (cg->client_cert_cb)(http, http->tls, names,
					     cg->client_cert_data);

		DEBUG_printf(("4http_tls_start: Client certificate callback "
		              "returned %d.", (int)error));
	      }

	      httpFreeCredentials(names);
	    }
	    break;

	case errSSLUnknownRootCert :
	    message = _("Unable to establish a secure connection to host "
	                "(untrusted certificate).");
	    break;

	case errSSLNoRootCert :
	    message = _("Unable to establish a secure connection to host "
	                "(self-signed certificate).");
	    break;

	case errSSLCertExpired :
	    message = _("Unable to establish a secure connection to host "
	                "(expired certificate).");
	    break;

	case errSSLCertNotYetValid :
	    message = _("Unable to establish a secure connection to host "
	                "(certificate not yet valid).");
	    break;

	case errSSLHostNameMismatch :
	    message = _("Unable to establish a secure connection to host "
	                "(host name mismatch).");
	    break;

	case errSSLXCertChainInvalid :
	    message = _("Unable to establish a secure connection to host "
	                "(certificate chain invalid).");
	    break;

	case errSSLConnectionRefused :
	    message = _("Unable to establish a secure connection to host "
	                "(peer dropped connection before responding).");
	    break;

 	default :
	    break;
      }
    }
  }

  if (error)
  {
    http->error  = error;
    http->status = HTTP_STATUS_ERROR;
    errno        = ECONNREFUSED;

    CFRelease(http->tls);
    http->tls = NULL;

   /*
    * If an error string wasn't set by the callbacks use a generic one...
    */

    if (!message)
#ifdef HAVE_CSSMERRORSTRING
      message = cssmErrorString(error);
#else
      message = _("Unable to establish a secure connection to host.");
#endif /* HAVE_CSSMERRORSTRING */

    _cupsSetError(IPP_STATUS_ERROR_CUPS_PKI, message, 1);

    return (-1);
  }

  return (0);
}


/*
 * 'http_tls_stop()' - Shut down SSL/TLS on a connection.
 */

static void
http_tls_stop(http_t *http)		/* I - HTTP connection */
{
  while (SSLClose(http->tls) == errSSLWouldBlock)
    usleep(1000);

  CFRelease(http->tls);

  if (http->tls_credentials)
    CFRelease(http->tls_credentials);

  http->tls             = NULL;
  http->tls_credentials = NULL;
}


/*
 * 'http_tls_write()' - Write to a SSL/TLS connection.
 */

static int				/* O - Bytes written */
http_tls_write(http_t     *http,	/* I - HTTP connection */
	       const char *buf,		/* I - Buffer holding data */
	       int        len)		/* I - Length of buffer */
{
  ssize_t	result;			/* Return value */
  OSStatus	error;			/* Error info */
  size_t	processed;		/* Number of bytes processed */


  DEBUG_printf(("2http_tls_write(http=%p, buf=%p, len=%d)", http, buf, len));

  error = SSLWrite(http->tls, buf, len, &processed);

  switch (error)
  {
    case 0 :
	result = (int)processed;
	break;

    case errSSLWouldBlock :
	if (processed)
	{
	  result = (int)processed;
	}
	else
	{
	  result = -1;
	  errno  = EINTR;
	}
	break;

    case errSSLClosedGraceful :
    default :
	if (processed)
	{
	  result = (int)processed;
	}
	else
	{
	  result = -1;
	  errno  = EPIPE;
	}
	break;
  }

  DEBUG_printf(("3http_tls_write: Returning %d.", (int)result));

  return ((int)result);
}


#if 0
/*
 * 'cupsdEndTLS()' - Shutdown a secure session with the client.
 */

int					/* O - 1 on success, 0 on error */
cupsdEndTLS(cupsd_client_t *con)	/* I - Client connection */
{
  while (SSLClose(con->http.tls) == errSSLWouldBlock)
    usleep(1000);

  CFRelease(con->http.tls);
  con->http.tls = NULL;

  if (con->http.tls_credentials)
    CFRelease(con->http.tls_credentials);

  return (1);
}


/*
 * 'cupsdStartTLS()' - Start a secure session with the client.
 */

int					/* O - 1 on success, 0 on error */
cupsdStartTLS(cupsd_client_t *con)	/* I - Client connection */
{
  OSStatus	error = 0;		/* Error code */
  SecTrustRef	peerTrust;		/* Peer certificates */


  cupsdLogMessage(CUPSD_LOG_DEBUG, "[Client %d] Encrypting connection.",
                  con->http.fd);

  con->http.tls_credentials = copy_cdsa_certificate(con);

  if (!con->http.tls_credentials)
  {
   /*
    * No keychain (yet), make a self-signed certificate...
    */

    if (make_certificate(con))
      con->http.tls_credentials = copy_cdsa_certificate(con);
  }

  if (!con->http.tls_credentials)
  {
    cupsdLogMessage(CUPSD_LOG_ERROR,
        	    "Could not find signing key in keychain \"%s\"",
		    ServerCertificate);
    error = errSSLBadConfiguration;
  }

  if (!error)
    con->http.tls = SSLCreateContext(kCFAllocatorDefault, kSSLServerSide,
                                     kSSLStreamType);

  if (!error)
    error = SSLSetIOFuncs(con->http.tls, http_cdsa_read, http_cdsa_write);

  if (!error)
    error = SSLSetConnection(con->http.tls, HTTP(con));

  if (!error)
    error = SSLSetCertificate(con->http.tls, con->http.tls_credentials);

  if (!error)
  {
   /*
    * Perform SSL/TLS handshake
    */

    while ((error = SSLHandshake(con->http.tls)) == errSSLWouldBlock)
      usleep(1000);
  }

  if (error)
  {
    cupsdLogMessage(CUPSD_LOG_ERROR,
                    "Unable to encrypt connection from %s - %s (%d)",
                    con->http.hostname, cssmErrorString(error), (int)error);

    con->http.error  = error;
    con->http.status = HTTP_ERROR;

    if (con->http.tls)
    {
      CFRelease(con->http.tls);
      con->http.tls = NULL;
    }

    if (con->http.tls_credentials)
    {
      CFRelease(con->http.tls_credentials);
      con->http.tls_credentials = NULL;
    }

    return (0);
  }

  cupsdLogMessage(CUPSD_LOG_DEBUG, "Connection from %s now encrypted.",
                  con->http.hostname);

  if (!SSLCopyPeerTrust(con->http.tls, &peerTrust) && peerTrust)
  {
    cupsdLogMessage(CUPSD_LOG_DEBUG, "Received %d peer certificates.",
		    (int)SecTrustGetCertificateCount(peerTrust));
    CFRelease(peerTrust);
  }
  else
    cupsdLogMessage(CUPSD_LOG_DEBUG, "Received NO peer certificates.");

  return (1);
}
#endif /* 0 */


/*
 * End of "$Id$".
 */
