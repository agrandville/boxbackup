// --------------------------------------------------------------------------
//
// File
//		Name:    BackupClientFileAttributes.cpp
//		Purpose: Storage of file attributes
//		Created: 2003/10/07
//
// --------------------------------------------------------------------------

#ifdef WIN32
// we need features from Windows 2000 and above for ACL support
#define WINVER 0x0500
#define _WIN32_WINNT 0x0500
#endif

#include "Box.h"

#ifdef HAVE_UNISTD_H
	#include <unistd.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <limits.h>
#include <algorithm>
#include <new>
#include <vector>
#ifdef HAVE_SYS_XATTR_H
#include <cerrno>
#include <sys/xattr.h>
#endif

#ifdef WIN32
#include <windows.h>
#include <AccCtrl.h>
#include <Aclapi.h>
#include <Sddl.h>
#endif

#include "BackupClientFileAttributes.h"
#include "CommonException.h"
#include "FileModificationTime.h"
#include "BoxTimeToUnix.h"
#include "BackupStoreException.h"
#include "CipherContext.h"
#include "CipherBlowfish.h"
#include "MD5Digest.h"
#include "CollectInBufferStream.h"
#include "Archive.h"

#include "MemLeakFindOn.h"

#define ATTRIBUTE_ENCODING_BLOWFISH	2

#define MAX_ATTRIBUTE_HASH_SECRET_LENGTH	256

// Hide private static variables from the rest of the world
// -- don't put them as static class variables to avoid openssl/evp.h being
// included all over the project.
namespace
{
	CipherContext sBlowfishEncrypt;
	CipherContext sBlowfishDecrypt;
	uint8_t sAttributeHashSecret[MAX_ATTRIBUTE_HASH_SECRET_LENGTH];
	int sAttributeHashSecretLength = 0;
}



// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::BackupClientFileAttributes()
//		Purpose: Default constructor
//		Created: 2003/10/07
//
// --------------------------------------------------------------------------
BackupClientFileAttributes::BackupClientFileAttributes()
	: mpClearAttributes(0)
{
	ASSERT(sizeof(u_int64_t) == sizeof(box_time_t));
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::BackupClientFileAttributes(const BackupClientFileAttributes &)
//		Purpose: Copy constructor
//		Created: 2003/10/07
//
// --------------------------------------------------------------------------
BackupClientFileAttributes::BackupClientFileAttributes(const BackupClientFileAttributes &rToCopy)
	: StreamableMemBlock(rToCopy), // base class does the hard work
	  mpClearAttributes(0)
{
}
BackupClientFileAttributes::BackupClientFileAttributes(const StreamableMemBlock &rToCopy)
	: StreamableMemBlock(rToCopy), // base class does the hard work
	  mpClearAttributes(0)
{
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::~BackupClientFileAttributes()
//		Purpose: Destructor
//		Created: 2003/10/07
//
// --------------------------------------------------------------------------
BackupClientFileAttributes::~BackupClientFileAttributes()
{
	if(mpClearAttributes)
	{
		delete mpClearAttributes;
		mpClearAttributes = 0;
	}
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes &operator=(const BackupClientFileAttributes &)
//		Purpose: Assignment operator
//		Created: 2003/10/07
//
// --------------------------------------------------------------------------
BackupClientFileAttributes &BackupClientFileAttributes::operator=(const BackupClientFileAttributes &rAttr)
{
	StreamableMemBlock::Set(rAttr);
	RemoveClear();	// make sure no decrypted version held
	return *this;
}
// Assume users play nice
BackupClientFileAttributes &BackupClientFileAttributes::operator=(const StreamableMemBlock &rAttr)
{
	StreamableMemBlock::Set(rAttr);
	RemoveClear();	// make sure no decrypted version held
	return *this;
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::operator==(const BackupClientFileAttributes &)
//		Purpose: Comparison operator
//		Created: 2003/10/09
//
// --------------------------------------------------------------------------
bool BackupClientFileAttributes::operator==(const BackupClientFileAttributes &rAttr) const
{
	EnsureClearAvailable();
	rAttr.EnsureClearAvailable();

	return mpClearAttributes->operator==(*rAttr.mpClearAttributes);
}
// Too dangerous to allow -- put the two names the wrong way round, and it compares encrypted data.
/*bool BackupClientFileAttributes::operator==(const StreamableMemBlock &rAttr) const
{
	StreamableMemBlock *pDecoded = 0;

	try
	{
		EnsureClearAvailable();
		StreamableMemBlock *pDecoded = MakeClear(rAttr);

		// Compare using clear version
		bool compared = mpClearAttributes->operator==(rAttr);
		
		// Delete temporary
		delete pDecoded;
	
		return compared;
	}
	catch(...)
	{
		delete pDecoded;
		throw;
	}
}*/


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::Compare(const BackupClientFileAttributes &, bool)
//		Purpose: Compare, optionally ignoring the attribute modification time and/or modification time, and some data which is
//				 irrelevant in practise (eg file generation number)
//		Created: 10/12/03
//
// --------------------------------------------------------------------------
bool BackupClientFileAttributes::Compare(const BackupClientFileAttributes &rAttr, bool IgnoreAttrModTime, bool IgnoreModTime) const
{
	EnsureClearAvailable();
	rAttr.EnsureClearAvailable();

	// Check sizes are the same, as a first check
	if(mpClearAttributes->GetSize() != rAttr.mpClearAttributes->GetSize())
	{
		return false;
	}
	
	// Then check the elements of the two things
	// Bytes are checked in network order, but this doesn't matter as we're only checking for equality.
	attr_StreamFormat_Generic *a1 = (attr_StreamFormat_Generic*)
		mpClearAttributes->GetBuffer();
	attr_StreamFormat_Generic *a2 = (attr_StreamFormat_Generic*)
		rAttr.mpClearAttributes->GetBuffer();
	
	if(a1->AttributeType != a2->AttributeType)
	{
		return false;
	}

	if (ntohl(a1->AttributeType) == ATTRIBUTETYPE_GENERIC_UNIX)
	{
		attr_StreamFormat_Unix* ua1 = (attr_StreamFormat_Unix*)a1;
		attr_StreamFormat_Unix* ua2 = (attr_StreamFormat_Unix*)a2;
		if (ua1->UID != ua2->UID
			|| ua1->GID != ua2->GID
			|| ua1->UserDefinedFlags != ua2->UserDefinedFlags
			|| ua1->Mode != ua2->Mode)
		{
			return false;
		}
		
		if(!IgnoreModTime)
		{
			if(ua1->ModificationTime != ua2->ModificationTime)
			{
				return false;
			}
		}

		if(!IgnoreAttrModTime)
		{
			if(ua1->AttrModificationTime != ua2->AttrModificationTime)
			{
				return false;
			}
		}
		
		// Check symlink string?
		unsigned int size = mpClearAttributes->GetSize();
		if(size == sizeof(attr_StreamFormat_Unix))
		{
			return true;
		}

		// Check whether symlink strings and xattrs match
		if(::memcmp(ua1 + 1, ua2 + 1, 
			size - sizeof(attr_StreamFormat_Unix)) != 0)
		{
			return false;
		}

		return true;
	}
	else if (ntohl(a1->AttributeType) == ATTRIBUTETYPE_GENERIC_WINDOWS)
	{
		attr_StreamFormat_Windows* wa1 = (attr_StreamFormat_Windows*)a1;
		attr_StreamFormat_Windows* wa2 = (attr_StreamFormat_Windows*)a2;

		if(wa1->Attributes != wa2->Attributes)
		{
			return false;
		}

		if(wa1->CreationTime != wa2->CreationTime)
		{
			return false;
		}
		
		if(!IgnoreModTime)
		{
			if(wa1->LastWriteTime != wa2->LastWriteTime)
			{
				return false;
			}
		}

		unsigned int size = mpClearAttributes->GetSize();
		if(size == sizeof(attr_StreamFormat_Windows))
		{
			return true;
		}

		// The rest of the record is the Windows ACL data, 
		// which we can compare byte-for-byte for now.

		if(::memcmp(wa1 + 1, wa2 + 1, 
			size - sizeof(attr_StreamFormat_Windows)) != 0)
		{
			return false;
		}

		return true;
	}
	else
	{
		// unknown attribute type
		return false;
	}

	// Passes all test, must be OK
	return true;
}




// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::ReadAttributes(const char *)
//		Purpose: Read the attributes of the file, and store them ready for streaming.
//				 Optionally retrieve the modification time and attribute modification time.
//		Created: 2003/10/07
//
// --------------------------------------------------------------------------
void BackupClientFileAttributes::ReadAttributes(const char *Filename, bool ZeroModificationTimes, box_time_t *pModTime,
	box_time_t *pAttrModTime, int64_t *pFileSize, InodeRefType *pInodeNumber, bool *pHasMultipleLinks)
{
	StreamableMemBlock *pAttrBlock = 0;
	try
	{
		struct stat st;
		if(::lstat(Filename, &st) != 0)
		{
			THROW_EXCEPTION(CommonException, OSFileError)
		}
		
		// Modification times etc
		if(pModTime) {*pModTime = FileModificationTime(st);}
		if(pAttrModTime) {*pAttrModTime = FileAttrModificationTime(st);}
		if(pFileSize) {*pFileSize = st.st_size;}
		if(pInodeNumber) {*pInodeNumber = st.st_ino;}
		if(pHasMultipleLinks) {*pHasMultipleLinks = (st.st_nlink > 1);}

		pAttrBlock = new StreamableMemBlock;

#ifdef WIN32
		FillAttributesWindows(*pAttrBlock, Filename, st, 
			ZeroModificationTimes);
#else // !WIN32
		FillAttributes(*pAttrBlock, Filename, st, 
			ZeroModificationTimes);

		// Is it a link?
		if((st.st_mode & S_IFMT) == S_IFLNK)
		{
			FillAttributesLink(*pAttrBlock, Filename, st);
		}

		FillExtendedAttr(*pAttrBlock, Filename);
#endif // WIN32

		// Attributes ready. Encrypt into this block
		EncryptAttr(*pAttrBlock);
		
		// Store the new attributes
		RemoveClear();
		mpClearAttributes = pAttrBlock;
		pAttrBlock = 0;
	}
	catch(...)
	{
		// clean up
		if (pAttrBlock)
		{
			delete pAttrBlock;
			pAttrBlock = 0;
		}
		throw;
	}
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::ReadAttributesLink()
//		Purpose: Private function, handles standard attributes for all objects
//		Created: 2003/10/07
//
// --------------------------------------------------------------------------
void BackupClientFileAttributes::FillAttributes(StreamableMemBlock &outputBlock, const char *Filename, struct stat &st, bool ZeroModificationTimes)
{
	outputBlock.ResizeBlock(sizeof(attr_StreamFormat_Unix));
	attr_StreamFormat_Unix *pattr = (attr_StreamFormat_Unix*)
		outputBlock.GetBuffer();
	ASSERT(pattr != 0);

	// Fill in the entries
	pattr->AttributeType = htonl(ATTRIBUTETYPE_GENERIC_UNIX);
	pattr->UID = htonl(st.st_uid);
	pattr->GID = htonl(st.st_gid);
	if(ZeroModificationTimes)
	{
		pattr->ModificationTime = 0;
		pattr->AttrModificationTime = 0;
	}
	else
	{
		pattr->ModificationTime = box_hton64(FileModificationTime(st));
		pattr->AttrModificationTime = box_hton64(FileAttrModificationTime(st));
	}
	pattr->Mode = htons(st.st_mode);

#ifndef HAVE_STRUCT_STAT_ST_FLAGS
	pattr->UserDefinedFlags = 0;
	pattr->FileGenerationNumber = 0;
#else
	pattr->UserDefinedFlags = htonl(st.st_flags);
	pattr->FileGenerationNumber = htonl(st.st_gen);
#endif
}


#ifdef WIN32
void WriteSid(Archive& rArchiver, PSID pSID)
{
	CHAR* pSidString;
	if (!ConvertSidToStringSid(pSID, &pSidString))
	{
		::syslog(LOG_WARNING, "Failed to convert Security Identifier "
			"to a string: error %d", GetLastError());
		THROW_EXCEPTION(BackupStoreException, LookupAccountFailed)
	}

	std::string sid(pSidString);
	LocalFree((HLOCAL)pSidString);

	char namebuf[1024];
	char domainbuf[1024];
	SID_NAME_USE nametype;
	DWORD namelen = sizeof(namebuf);
	DWORD domainlen = sizeof(domainbuf);

	if(!LookupAccountSid(NULL, pSID, namebuf, &namelen,
		domainbuf, &domainlen, &nametype))
	{
		::syslog(LOG_WARNING, "Failed to find account details for "
			"'%s': error %d", sid.c_str(), GetLastError());
		THROW_EXCEPTION(BackupStoreException, LookupAccountFailed)
	}

	std::string domain(domainbuf);
	std::string name(namebuf);

	rArchiver.Write((int)nametype);
	rArchiver.Write(sid);
	rArchiver.Write(domain);
	rArchiver.Write(name);
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::FillAttributesWindows()
//		Purpose: Private function, handles Windows attributes (ACL)
//		Created: 2006/02/27
//
// --------------------------------------------------------------------------
void BackupClientFileAttributes::FillAttributesWindows
(
	StreamableMemBlock &rOutputBlock, const char *pFilename, 
	struct stat &rStat, bool ZeroLastWriteTime
)
{
	rOutputBlock.ResizeBlock(sizeof(attr_StreamFormat_Windows));
	attr_StreamFormat_Windows *pAttr = (attr_StreamFormat_Windows*)
		rOutputBlock.GetBuffer();
	ASSERT(pAttr != 0);

	// Fill in the entries
	pAttr->AttributeType = htonl(ATTRIBUTETYPE_GENERIC_WINDOWS);
	pAttr->CreationTime = box_hton64(rStat.st_ctime * MICRO_SEC_IN_SEC_LL);

	if(ZeroLastWriteTime)
	{
		pAttr->LastWriteTime = 0;
	}
	else
	{
		pAttr->LastWriteTime = box_hton64(FileModificationTime(rStat));
	}

	// not very efficient to stat the file again, but emu_fstat
	// doesn't return the information that we need.

	HANDLE handle = OpenFileByNameUtf8(pFilename);

	if (handle == INVALID_HANDLE_VALUE)
	{
		::syslog(LOG_WARNING, "Failed to open file to read "
			"Windows attributes: '%s': error %d", 
			pFilename, GetLastError());
		THROW_EXCEPTION(CommonException, OSFileError)
	}

	BY_HANDLE_FILE_INFORMATION fi;
	bool result = GetFileInformationByHandle(handle, &fi);

	if (!result)
	{
		CloseHandle(handle);
		::syslog(LOG_WARNING, "Failed to read Windows "
			"file attributes for '%s': error %d", 
			pFilename, GetLastError());
		THROW_EXCEPTION(CommonException, OSFileError)
	}

	if (INVALID_FILE_ATTRIBUTES == fi.dwFileAttributes)
	{
		CloseHandle(handle);
		::syslog(LOG_WARNING, "Failed to read valid Windows "
			"file attributes for '%s': error %d", 
			pFilename, GetLastError());
		THROW_EXCEPTION(CommonException, OSFileError)
	}

	pAttr->Attributes = fi.dwFileAttributes;

	// Now start writing our custom data, with the magic and version
	CollectInBufferStream buffer;
	Archive archiver(buffer, 0);

	archiver.Write((int) ACL_FORMAT_MAGIC);
	archiver.Write((int) ACL_FORMAT_VERSION);

	PSID psidOwner = NULL;
	PSID psidGroup = NULL;
	PACL pDacl = NULL;
	PSECURITY_DESCRIPTOR pSecurityDesc = NULL;
	PEXPLICIT_ACCESS pEntries = NULL;

	try
	{
		DWORD result = GetSecurityInfo(
			handle, // object handle
			SE_FILE_OBJECT, // ObjectType
			DACL_SECURITY_INFORMATION  | // SecurityInfo
			GROUP_SECURITY_INFORMATION |
			OWNER_SECURITY_INFORMATION,
			&psidOwner, // ppsidOwner,
			&psidGroup, // ppsidGroup,
			&pDacl,     // ppDacl,
			NULL,       // ppSacl,
			&pSecurityDesc // ppSecurityDescriptor
		);

		CloseHandle(handle);

		if (result != ERROR_SUCCESS)
		{
			::syslog(LOG_WARNING, "Failed to get Windows "
				"security info for '%s': error %d",
				pFilename, result);
			THROW_EXCEPTION(CommonException, OSFileError)
		}

		WriteSid(archiver, psidOwner);
		WriteSid(archiver, psidGroup);

		ULONG numEntries;
		result = GetExplicitEntriesFromAcl
		(
			pDacl,       // pAcl
			&numEntries, // pcCountOfExplicitEntries,
			&pEntries    // pListOfExplicitEntries
		);

		if(result != ERROR_SUCCESS)
		{
			::syslog(LOG_WARNING, "Failed to get Windows "
				"access control list entries for '%s': "
				"error %d", pFilename, result);
			THROW_EXCEPTION(CommonException, OSFileError)
		}

		archiver.Write((int)numEntries);

		for (ULONG i = 0; i < numEntries; i++)
		{
			EXPLICIT_ACCESS* pEntry = &(pEntries[i]);

			archiver.Write((int)pEntry->grfAccessPermissions);
			archiver.Write((int)pEntry->grfAccessMode);
			archiver.Write((int)pEntry->grfInheritance);
			archiver.Write((int)pEntry->Trustee.TrusteeForm);
			archiver.Write((int)pEntry->Trustee.TrusteeType);

			if(pEntry->Trustee.pMultipleTrustee != NULL)
			{
				::syslog(LOG_WARNING, "Failed to process "
					"access control entry for '%s': "
					"multiple trustees not supported",
					pFilename);
				THROW_EXCEPTION(CommonException, OSFileError);
			}

			if(pEntry->Trustee.MultipleTrusteeOperation != 
				NO_MULTIPLE_TRUSTEE)
			{
				::syslog(LOG_WARNING, "Failed to process "
					"access control entry for '%s': "
					"multiple trustees not supported (2)",
					pFilename);
				THROW_EXCEPTION(CommonException, OSFileError);
			}

			switch(pEntry->Trustee.TrusteeForm)
			{
			case TRUSTEE_IS_SID:
				{
					PSID trusteeSid = (PSID)(
						pEntry->Trustee.ptstrName);
					WriteSid(archiver, trusteeSid);
				}
				break;
			default:
				::syslog(LOG_WARNING, "Failed to process "
					"access control entry for '%s': "
					"trustee form %d not supported",
					pFilename, 
					pEntry->Trustee.TrusteeForm);
				THROW_EXCEPTION(CommonException, OSFileError);
			}

		}
	}
	catch(...)
	{
		if (pEntries)      LocalFree((HLOCAL)pEntries);
		if (pSecurityDesc) LocalFree((HLOCAL)pSecurityDesc);
		throw;
	}

	LocalFree((HLOCAL)pEntries);
	LocalFree((HLOCAL)pSecurityDesc);

	// Copy the serialised data into the output block
	buffer.SetForReading();
	rOutputBlock.ResizeBlock(rOutputBlock.GetSize() + buffer.GetSize());

	// block might have moved after resize
	pAttr = (attr_StreamFormat_Windows *)(rOutputBlock.GetBuffer());
	memcpy(pAttr + 1, buffer.GetBuffer(), buffer.GetSize());
}
#endif // WIN32


#ifndef WIN32
// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::ReadAttributesLink()
//		Purpose: Private function, handles the case where a symbolic link is needed
//		Created: 2003/10/07
//
// --------------------------------------------------------------------------
void BackupClientFileAttributes::FillAttributesLink(StreamableMemBlock &outputBlock, const char *Filename, struct stat &st)
{
	// Make sure we're only called for symbolic links
	ASSERT((st.st_mode & S_IFMT) == S_IFLNK);

	// Get the filename the link is linked to
	char linkedTo[PATH_MAX+4];
	int linkedToSize = ::readlink(Filename, linkedTo, PATH_MAX);
	if(linkedToSize == -1)
	{
		THROW_EXCEPTION(CommonException, OSFileError);
	}

	int oldSize = outputBlock.GetSize();
	outputBlock.ResizeBlock(oldSize+linkedToSize+1);
	char* buffer = static_cast<char*>(outputBlock.GetBuffer());

	// Add the path name for the symbolic link, and add 0 termination
	std::memcpy(buffer+oldSize, linkedTo, linkedToSize);
	buffer[oldSize+linkedToSize] = '\0';
}
#endif

// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::ReadExtendedAttr(const char *, unsigned char**)
//		Purpose: Private function, read the extended attributes of the file into the block
//		Created: 2005/06/12
//
// --------------------------------------------------------------------------
void BackupClientFileAttributes::FillExtendedAttr(StreamableMemBlock &outputBlock, const char *Filename)
{
#ifdef HAVE_SYS_XATTR_H
	int listBufferSize = 1000;
	char* list = new char[listBufferSize];

	try
	{
		// This returns an unordered list of attribute names, each 0 terminated,
		// concatenated together
		int listSize = ::llistxattr(Filename, list, listBufferSize);

		if(listSize>listBufferSize)
		{
			delete[] list, list = NULL;
			list = new char[listSize];
			listSize = ::llistxattr(Filename, list, listSize);
		}

		if(listSize>0)
		{
			// Extract list of attribute names so we can sort them
			std::vector<std::string> attrKeys;
			for(int i = 0; i<listSize; ++i)
			{
				std::string attrKey(list+i);
				i += attrKey.size();
				attrKeys.push_back(attrKey);
			}
			sort(attrKeys.begin(), attrKeys.end());

			// Make initial space in block
			int xattrSize = outputBlock.GetSize();
			int xattrBufferSize = (xattrSize+listSize)>500 ? (xattrSize+listSize)*2 : 1000;
			outputBlock.ResizeBlock(xattrBufferSize);
			unsigned char* buffer = static_cast<unsigned char*>(outputBlock.GetBuffer());

			// Leave space for attr block size later
			int xattrBlockSizeOffset = xattrSize;
			xattrSize += sizeof(u_int32_t);

			// Loop for each attribute
			for(std::vector<std::string>::const_iterator attrKeyI = attrKeys.begin(); attrKeyI!=attrKeys.end(); ++attrKeyI)
			{
				std::string attrKey(*attrKeyI);

				if(xattrSize+sizeof(u_int16_t)+attrKey.size()+1+sizeof(u_int32_t)>static_cast<unsigned int>(xattrBufferSize))
				{
					xattrBufferSize = (xattrBufferSize+sizeof(u_int16_t)+attrKey.size()+1+sizeof(u_int32_t))*2;
					outputBlock.ResizeBlock(xattrBufferSize);
					buffer = static_cast<unsigned char*>(outputBlock.GetBuffer());
				}

				// Store length and text for attibute name
				u_int16_t keyLength = htons(attrKey.size()+1);
				std::memcpy(buffer+xattrSize, &keyLength, sizeof(u_int16_t));
				xattrSize += sizeof(u_int16_t);
				std::memcpy(buffer+xattrSize, attrKey.c_str(), attrKey.size()+1);
				xattrSize += attrKey.size()+1;

				// Leave space for value size
				int valueSizeOffset = xattrSize;
				xattrSize += sizeof(u_int32_t);

				// Find size of attribute (must call with buffer and length 0 on some platforms,
				// as -1 is returned if the data doesn't fit.)
				int valueSize = ::lgetxattr(Filename, attrKey.c_str(), 0, 0);
				if(valueSize<0)
				{
					THROW_EXCEPTION(CommonException, OSFileError);
				}

				// Resize block, if needed
				if(xattrSize+valueSize>xattrBufferSize)
				{
					xattrBufferSize = (xattrBufferSize+valueSize)*2;
					outputBlock.ResizeBlock(xattrBufferSize);
					buffer = static_cast<unsigned char*>(outputBlock.GetBuffer());
				}

				// This gets the attribute value (may be text or binary), no termination
				valueSize = ::lgetxattr(Filename, attrKey.c_str(), buffer+xattrSize, xattrBufferSize-xattrSize);
				if(valueSize<0)
				{
					THROW_EXCEPTION(CommonException, OSFileError);
				}
				xattrSize += valueSize;

				// Fill in value size
				u_int32_t valueLength = htonl(valueSize);
				std::memcpy(buffer+valueSizeOffset, &valueLength, sizeof(u_int32_t));
			}

			// Fill in attribute block size
			u_int32_t xattrBlockLength = htonl(xattrSize-xattrBlockSizeOffset-sizeof(u_int32_t));
			std::memcpy(buffer+xattrBlockSizeOffset, &xattrBlockLength, sizeof(u_int32_t));

			outputBlock.ResizeBlock(xattrSize);
		}
		else if(listSize<0 && errno!=EOPNOTSUPP && errno!=EACCES)
		{
			THROW_EXCEPTION(CommonException, OSFileError);
		}
	}
	catch(...)
	{
		delete[] list;
		throw;
	}
	delete[] list;
#endif
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::WriteAttributes(const char *)
//		Purpose: Apply the stored attributes to the file
//		Created: 2003/10/07
//
// --------------------------------------------------------------------------
void BackupClientFileAttributes::WriteAttributes(const char *Filename) const
{
	// Got something loaded
	if(GetSize() <= 0)
	{
		THROW_EXCEPTION(BackupStoreException, AttributesNotLoaded);
	}
	
	// Make sure there are clear attributes to use
	EnsureClearAvailable();
	ASSERT(mpClearAttributes != 0);

	// Check if the decrypted attributes are small enough, and the type of attributes stored
	if(mpClearAttributes->GetSize() < (int)sizeof(int32_t))
	{
		THROW_EXCEPTION(BackupStoreException, AttributesNotUnderstood);
	}
	int32_t *type = (int32_t*)mpClearAttributes->GetBuffer();
	ASSERT(type != 0);

#ifdef WIN32
	if(ntohl(*type) == ATTRIBUTETYPE_GENERIC_WINDOWS)
	{
		WriteAttributesWindows(Filename);
		return;
	}
#endif

	if(ntohl(*type) != ATTRIBUTETYPE_GENERIC_UNIX)
	{
		// Don't know what to do with these
		THROW_EXCEPTION(BackupStoreException, AttributesNotUnderstood);
	}
	
	// Check there is enough space for an attributes block
	if(mpClearAttributes->GetSize() < (int)sizeof(attr_StreamFormat_Unix))
	{
		// Too small
		THROW_EXCEPTION(BackupStoreException, AttributesNotLoaded);
	}

	// Get pointer to structure
	attr_StreamFormat_Unix *pattr = (attr_StreamFormat_Unix*)
		mpClearAttributes->GetBuffer();
	int xattrOffset = sizeof(attr_StreamFormat_Unix);

	// is it a symlink?
	int16_t mode = ntohs(pattr->Mode);
	if((mode & S_IFMT) == S_IFLNK)
	{
		// Check things are sensible
		if(mpClearAttributes->GetSize() < 
			(int)sizeof(attr_StreamFormat_Unix) + 1)
		{
			// Too small
			THROW_EXCEPTION(BackupStoreException, AttributesNotLoaded);
		}
	
#ifdef WIN32
		::syslog(LOG_WARNING, 
			"Cannot create symbolic links on Windows: %s", 
			Filename);
#else
		// Make a symlink, first deleting anything in the way
		::unlink(Filename);
		if(::symlink((char*)(pattr + 1), Filename) != 0)
		{
			THROW_EXCEPTION(CommonException, OSFileError)
		}
#endif

		xattrOffset += std::strlen(reinterpret_cast<char*>(pattr+1))+1;
	}
	
	// If working as root, set user IDs
	#ifndef WIN32
	if(::geteuid() == 0)
	{
		#ifndef HAVE_LCHOWN
			// only if not a link, can't set their owner on this platform
			if((mode & S_IFMT) != S_IFLNK)
			{
				// Not a link, use normal chown
				if(::chown(Filename, ntohl(pattr->UID), ntohl(pattr->GID)) != 0)
				{
					THROW_EXCEPTION(CommonException, OSFileError)
				}
			}
		#else
			if(::lchown(Filename, ntohl(pattr->UID), ntohl(pattr->GID)) != 0)	// use the version which sets things on symlinks
			{
				THROW_EXCEPTION(CommonException, OSFileError)
			}
		#endif
	}
	#endif

	if(static_cast<int>(xattrOffset+sizeof(u_int32_t))<=mpClearAttributes->GetSize())
	{
		WriteExtendedAttr(Filename, xattrOffset);
	}

	// Stop now if symlink, because otherwise it'll just be applied to the target
	if((mode & S_IFMT) == S_IFLNK)
	{
		return;
	}

	// Set modification time?
	box_time_t modtime = box_ntoh64(pattr->ModificationTime);
	if(modtime != 0)
	{
		// Work out times as timevals
		struct timeval times[2];

		#ifdef WIN32
		BoxTimeToTimeval(box_ntoh64(pattr->ModificationTime), 
			times[1]);
		BoxTimeToTimeval(box_ntoh64(pattr->AttrModificationTime), 
			times[0]);
		// Because stat() returns the creation time in the ctime
		// field under Windows, and this gets saved in the 
		// AttrModificationTime field of the serialised attributes,
		// we subvert the first parameter of emu_utimes() to allow
		// it to be reset to the right value on the restored file.
		#else
		BoxTimeToTimeval(modtime, times[1]);
		// Copy access time as well, why not, got to set it to something
		times[0] = times[1];
		// Attr modification time will be changed anyway, 
		// nothing that can be done about it
		#endif
		
		// Try to apply
		if(::utimes(Filename, times) != 0)
		{
			THROW_EXCEPTION(CommonException, OSFileError)
		}
	}
	
	// Apply everything else... (allowable mode flags only)
	if(::chmod(Filename, mode & (S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX)) != 0)	// mode must be done last (think setuid)
	{
		THROW_EXCEPTION(CommonException, OSFileError)
	}
}


#ifdef WIN32
bool ReadSid(Archive& rArchive, PSID* pPSID)
{
	int nametype;
	rArchive.Read(nametype);

	std::string sid;
	rArchive.Read(sid);

	std::string domain;
	rArchive.Read(domain);

	std::string name;
	rArchive.Read(name);

	name = domain + '\\' + name;

	DWORD sidBufSize = 0;
	DWORD domainBufSize = 0;
	SID_NAME_USE use;

	// TODO: if we don't find the account name, 
	// use the stored SID instead

	if (!LookupAccountName(NULL, name.c_str(), NULL, &sidBufSize,
		NULL, &domainBufSize, &use) && 
		GetLastError() != ERROR_INSUFFICIENT_BUFFER)
	{
		::syslog(LOG_ERR, "Failed to lookup account details for "
			"'%s': error %d", name.c_str(), GetLastError());
		return false;
	}

	*pPSID = malloc(sidBufSize);
	TCHAR* domainstr = (TCHAR *)malloc(domainBufSize);

	BOOL result = LookupAccountName(NULL, name.c_str(),
		*pPSID, &sidBufSize, domainstr, &domainBufSize, &use);
	free(domainstr);

	if (!result)
	{
		::syslog(LOG_ERR, "Failed to lookup account details for "
			"'%s': error %d", name.c_str(), GetLastError());
		free(*pPSID);
		*pPSID = NULL;
		return false;
	}

	return true;
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::WriteAttributesWindows(
//			 const char *)
//		Purpose: Apply the stored Windows attributes to the file
//		Created: 2006/03/12
//
// --------------------------------------------------------------------------
void BackupClientFileAttributes::WriteAttributesWindows(const char *Filename) 
const
{
	// Check there is enough space for an attributes block
	if(mpClearAttributes->GetSize() < 
		(int)sizeof(attr_StreamFormat_Windows))
	{
		// Too small
		THROW_EXCEPTION(BackupStoreException, AttributesNotLoaded);
	}

	// Get pointer to structure
	attr_StreamFormat_Windows *pAttr = (attr_StreamFormat_Windows*)
		mpClearAttributes->GetBuffer();

	std::wstring wideName;
	if (!GetFileNameWide(Filename, wideName))
	{
		THROW_EXCEPTION(CommonException, OSFileError);
	}

	DWORD attributes = pAttr->Attributes;
	attributes &= ~FILE_ATTRIBUTE_OFFLINE;
	attributes &= ~FILE_ATTRIBUTE_REPARSE_POINT;

	if (!SetFileAttributesW(wideName.c_str(), attributes))
	{
		::syslog(LOG_ERR, "Failed to restore attributes of '%s': "
			"error %d", Filename, GetLastError());
		return;
	}

	CollectInBufferStream buffer;
	buffer.Write(pAttr + 1, mpClearAttributes->GetSize() -
		sizeof(attr_StreamFormat_Windows));
	buffer.SetForReading();

	Archive archive(buffer, 0);
	int data;

	archive.Read(data);
	if (data != ACL_FORMAT_MAGIC)
	{
		::syslog(LOG_ERR, "Failed to restore ACL of '%s': "
			"wrong magic in attribute block", Filename);
		THROW_EXCEPTION(BackupStoreException, AttributesNotLoaded);
	}

	archive.Read(data);
	if (data != ACL_FORMAT_VERSION)
	{
		::syslog(LOG_ERR, "Failed to restore ACL of '%s': "
			"wrong version in attribute block", Filename);
		THROW_EXCEPTION(BackupStoreException, AttributesNotLoaded);
	}

	PACL pNewAcl = NULL;
	PSID pSidOwner = NULL, pSidGroup = NULL;

	if (!ReadSid(archive, &pSidOwner))
	{
		::syslog(LOG_ERR, "Failed to restore ACL of '%s': "
			"failed to decode file owner", Filename);
		THROW_EXCEPTION(BackupStoreException, AttributesNotLoaded);
	}

	if (!ReadSid(archive, &pSidGroup))
	{
		::syslog(LOG_ERR, "Failed to restore ACL of '%s': "
			"failed to decode file group", Filename);
		THROW_EXCEPTION(BackupStoreException, AttributesNotLoaded);
	}

	int numEntries;
	archive.Read(numEntries);

	EXPLICIT_ACCESS* pEntries = (EXPLICIT_ACCESS*)calloc(numEntries, 
		sizeof(EXPLICIT_ACCESS));

	if (!pEntries)
	{
		::syslog(LOG_ERR, "Failed to restore ACL of '%s': "
			"out of memory", Filename);
		if (pSidOwner) free(pSidOwner);
		if (pSidGroup) free(pSidGroup);
		THROW_EXCEPTION(BackupStoreException, AttributesNotLoaded);
	}
	
	for (int i = 0; i < numEntries; i++)
	{
		PEXPLICIT_ACCESS pEntry = &(pEntries[i]);

		int data;
		archive.Read(data); pEntry->grfAccessPermissions = data;
		archive.Read(data); pEntry->grfAccessMode = (ACCESS_MODE)data;
		archive.Read(data); pEntry->grfInheritance = data;
		archive.Read(data); pEntry->Trustee.TrusteeForm = 
			(TRUSTEE_FORM)data;
		archive.Read(data); pEntry->Trustee.TrusteeType = 
			(TRUSTEE_TYPE)data;

		if (pEntry->Trustee.TrusteeForm != TRUSTEE_IS_SID)
		{
			::syslog(LOG_ERR, "Failed to restore ACL of '%s': "
				"only SID trustees are supported", Filename);
			if (pSidOwner) free(pSidOwner);
			if (pSidGroup) free(pSidGroup);
			THROW_EXCEPTION(BackupStoreException, 
				AttributesNotLoaded);
		}

		LPSTR* pPSIDstr = &pEntry->Trustee.ptstrName;
		if (!ReadSid(archive, (PSID*)pPSIDstr))
		{
			::syslog(LOG_ERR, "Failed to restore ACL of '%s': "
				"failed to read ACL entry %d SID", Filename, i);
			if (pSidOwner) free(pSidOwner);
			if (pSidGroup) free(pSidGroup);
			THROW_EXCEPTION(BackupStoreException, 
				AttributesNotLoaded);
		}
	}
		
	DWORD result = SetEntriesInAcl(numEntries, pEntries, NULL, &pNewAcl);

	for (int i = 0; i < numEntries; i++)
	{
		PEXPLICIT_ACCESS pEntry = &(pEntries[i]);
		PSID pSid = (PSID)(pEntry->Trustee.ptstrName);
		free(pSid);
	}

	free(pEntries);

	if (result != ERROR_SUCCESS)
	{
		::syslog(LOG_ERR, "Failed to create access control list "
			"for '%s': error %d", Filename, result);
		if (pSidOwner) free(pSidOwner);
		if (pSidGroup) free(pSidGroup);
		THROW_EXCEPTION(BackupStoreException, AttributesNotLoaded);
	}

	WCHAR* pNameBuffer = new WCHAR [wideName.size() + 1];
	if (!pNameBuffer)
	{
		::syslog(LOG_ERR, "Failed to create access control list "
			"for '%s': out of memory", Filename);
		if (pSidOwner) free(pSidOwner);
		if (pSidGroup) free(pSidGroup);
		THROW_EXCEPTION(BackupStoreException, AttributesNotLoaded);
	}

	memcpy(pNameBuffer, wideName.c_str(), wideName.size() * sizeof(WCHAR));

	result = SetNamedSecurityInfoW(pNameBuffer, SE_FILE_OBJECT,
		DACL_SECURITY_INFORMATION | 
		OWNER_SECURITY_INFORMATION |
		GROUP_SECURITY_INFORMATION
		/* | UNPROTECTED_DACL_SECURITY_INFORMATION */,
		pSidOwner, pSidGroup, pNewAcl, NULL);

	delete [] pNameBuffer;
	LocalFree((HLOCAL)pNewAcl);

	if (result != ERROR_SUCCESS)
	{
		::syslog(LOG_ERR, "Failed to apply access control list "
			"to '%s': error %d", Filename, result);
		if (pSidOwner) free(pSidOwner);
		if (pSidGroup) free(pSidGroup);
		THROW_EXCEPTION(BackupStoreException, AttributesNotLoaded);
	}
}
#endif


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::IsSymLink()
//		Purpose: Do these attributes represent a symbolic link?
//		Created: 2003/10/07
//
// --------------------------------------------------------------------------
bool BackupClientFileAttributes::IsSymLink() const
{
	EnsureClearAvailable();

	// Got the right kind of thing?
	if(mpClearAttributes->GetSize() < (int)sizeof(int32_t))
	{
		THROW_EXCEPTION(BackupStoreException, AttributesNotLoaded);
	}
	
	// Get the type of attributes stored
	int32_t *type = (int32_t*)mpClearAttributes->GetBuffer();
	ASSERT(type != 0);
	if(ntohl(*type) == ATTRIBUTETYPE_GENERIC_UNIX && 
		mpClearAttributes->GetSize() > (int)sizeof(attr_StreamFormat_Unix))
	{
		// Check link
		attr_StreamFormat_Unix *pattr = (attr_StreamFormat_Unix*)
			mpClearAttributes->GetBuffer();
		return ((ntohs(pattr->Mode)) & S_IFMT) == S_IFLNK;
	}
	
	return false;
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::RemoveClear()
//		Purpose: Private. Deletes any clear version of the attributes that may be held
//		Created: 3/12/03
//
// --------------------------------------------------------------------------
void BackupClientFileAttributes::RemoveClear() const
{
	if(mpClearAttributes)
	{
		delete mpClearAttributes;
	}
	mpClearAttributes = 0;
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::EnsureClearAvailable()
//		Purpose: Private. Makes sure the clear version is available
//		Created: 3/12/03
//
// --------------------------------------------------------------------------
void BackupClientFileAttributes::EnsureClearAvailable() const
{
	if(mpClearAttributes == 0)
	{
		mpClearAttributes = MakeClear(*this);
	}
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::WriteExtendedAttr(const char *Filename, int xattrOffset)
//		Purpose: Private function, apply the stored extended attributes to the file
//		Created: 2005/06/13
//
// --------------------------------------------------------------------------
void BackupClientFileAttributes::WriteExtendedAttr(const char *Filename, int xattrOffset) const
{
#ifdef HAVE_SYS_XATTR_H
	const char* buffer = static_cast<char*>(mpClearAttributes->GetBuffer());

	u_int32_t xattrBlockLength = 0;
	std::memcpy(&xattrBlockLength, buffer+xattrOffset, sizeof(u_int32_t));
	int xattrBlockSize = ntohl(xattrBlockLength);
	xattrOffset += sizeof(u_int32_t);

	int xattrEnd = xattrOffset+xattrBlockSize;
	if(xattrEnd>mpClearAttributes->GetSize())
	{
		// Too small
		THROW_EXCEPTION(BackupStoreException, AttributesNotLoaded);
	}

	while(xattrOffset<xattrEnd)
	{
		u_int16_t keyLength = 0;
		std::memcpy(&keyLength, buffer+xattrOffset, sizeof(u_int16_t));
		int keySize = ntohs(keyLength);
		xattrOffset += sizeof(u_int16_t);

		const char* key = buffer+xattrOffset;
		xattrOffset += keySize;

		u_int32_t valueLength = 0;
		std::memcpy(&valueLength, buffer+xattrOffset, sizeof(u_int32_t));
		int valueSize = ntohl(valueLength);
		xattrOffset += sizeof(u_int32_t);

		// FIXME: Warn on EOPNOTSUPP
		if(::lsetxattr(Filename, key, buffer+xattrOffset, valueSize, 0)!=0 && errno!=EOPNOTSUPP)
		{
			THROW_EXCEPTION(CommonException, OSFileError);
		}

		xattrOffset += valueSize;
	}

	ASSERT(xattrOffset==xattrEnd);
#endif
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::MakeClear(const StreamableMemBlock &)
//		Purpose: Static. Decrypts stored attributes.
//		Created: 3/12/03
//
// --------------------------------------------------------------------------
StreamableMemBlock *BackupClientFileAttributes::MakeClear(const StreamableMemBlock &rEncrypted)
{
	// New block
	StreamableMemBlock *pdecrypted = 0;

	try
	{
		// Check the block is big enough for IV and header
		int ivSize = sBlowfishEncrypt.GetIVLength();
		if(rEncrypted.GetSize() <= (ivSize + 1))
		{
			THROW_EXCEPTION(BackupStoreException, BadEncryptedAttributes);
		}
		
		// How much space is needed for the output?
		int maxDecryptedSize = sBlowfishDecrypt.MaxOutSizeForInBufferSize(rEncrypted.GetSize() - ivSize);
		
		// Allocate it
		pdecrypted = new StreamableMemBlock(maxDecryptedSize);
	
		// ptr to block	
		uint8_t *encBlock = (uint8_t*)rEncrypted.GetBuffer();

		// Check that the header has right type
		if(encBlock[0] != ATTRIBUTE_ENCODING_BLOWFISH)
		{
			THROW_EXCEPTION(BackupStoreException, EncryptedAttributesHaveUnknownEncoding);
		}

		// Set IV
		sBlowfishDecrypt.SetIV(encBlock + 1);
		
		// Decrypt
		int decryptedSize = sBlowfishDecrypt.TransformBlock(pdecrypted->GetBuffer(), maxDecryptedSize, encBlock + 1 + ivSize, rEncrypted.GetSize() - (ivSize + 1));

		// Resize block to fit
		pdecrypted->ResizeBlock(decryptedSize);
	}
	catch(...)
	{
		delete pdecrypted;
		pdecrypted = 0;
	}

	return pdecrypted;
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::SetBlowfishKey(const void *, int)
//		Purpose: Static. Sets the key to use for encryption and decryption.
//		Created: 3/12/03
//
// --------------------------------------------------------------------------
void BackupClientFileAttributes::SetBlowfishKey(const void *pKey, int KeyLength)
{
	// IVs set later
	sBlowfishEncrypt.Reset();
	sBlowfishEncrypt.Init(CipherContext::Encrypt, CipherBlowfish(CipherDescription::Mode_CBC, pKey, KeyLength));
	sBlowfishDecrypt.Reset();
	sBlowfishDecrypt.Init(CipherContext::Decrypt, CipherBlowfish(CipherDescription::Mode_CBC, pKey, KeyLength));
}



// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::EncryptAttr(const StreamableMemBlock &)
//		Purpose: Private. Encrypt the given attributes into this block. 
//		Created: 3/12/03
//
// --------------------------------------------------------------------------
void BackupClientFileAttributes::EncryptAttr(const StreamableMemBlock &rToEncrypt)
{
	// Free any existing block
	FreeBlock();
	
	// Work out the maximum amount of space we need
	int maxEncryptedSize = sBlowfishEncrypt.MaxOutSizeForInBufferSize(rToEncrypt.GetSize());
	// And the size of the IV
	int ivSize = sBlowfishEncrypt.GetIVLength();
	
	// Allocate this space
	AllocateBlock(maxEncryptedSize + ivSize + 1);
	
	// Store the encoding byte
	uint8_t *block = (uint8_t*)GetBuffer();
	block[0] = ATTRIBUTE_ENCODING_BLOWFISH;
	
	// Generate and store an IV for this attribute block
	int ivSize2 = 0;
	const void *iv = sBlowfishEncrypt.SetRandomIV(ivSize2);
	ASSERT(ivSize == ivSize2);
	
	// Copy into the encrypted block
	::memcpy(block + 1, iv, ivSize);
	
	// Do the transform
	int encrytedSize = sBlowfishEncrypt.TransformBlock(block + 1 + ivSize, maxEncryptedSize, rToEncrypt.GetBuffer(), rToEncrypt.GetSize());

	// Resize this block
	ResizeBlock(encrytedSize + ivSize + 1);
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::SetAttributeHashSecret(const void *, int)
//		Purpose: Set the secret for the filename attribute hash
//		Created: 25/4/04
//
// --------------------------------------------------------------------------
void BackupClientFileAttributes::SetAttributeHashSecret(const void *pSecret, int SecretLength)
{
	if(SecretLength > (int)sizeof(sAttributeHashSecret))
	{
		SecretLength = sizeof(sAttributeHashSecret);
	}
	if(SecretLength < 0)
	{
		THROW_EXCEPTION(BackupStoreException, Internal)
	}
	
	// Copy
	::memcpy(sAttributeHashSecret, pSecret, SecretLength);
	sAttributeHashSecretLength = SecretLength;
}


// --------------------------------------------------------------------------
//
// Function
//		Name:    BackupClientFileAttributes::GenerateAttributeHash(struct stat &, const std::string &, const std::string &)
//		Purpose: Generate a 64 bit hash from the attributes, used to detect changes.
//				 Include filename in the hash, so that it changes from one file to another,
//				 so don't reveal identical attributes.
//		Created: 25/4/04
//
// --------------------------------------------------------------------------
uint64_t BackupClientFileAttributes::GenerateAttributeHash(struct stat &st, const std::string &filename, const std::string &leafname)
{
	if(sAttributeHashSecretLength == 0)
	{
		THROW_EXCEPTION(BackupStoreException, AttributeHashSecretNotSet)
	}
	
	// Assemble stuff we're interested in
	attributeHashData hashData;
	memset(&hashData, 0, sizeof(hashData));
	// Use network byte order and large sizes to be cross platform
	hashData.uid = htonl(st.st_uid);
	hashData.gid = htonl(st.st_gid);
	hashData.mode = htonl(st.st_mode);

	StreamableMemBlock xattr;
	FillExtendedAttr(xattr, filename.c_str());

	// Create a MD5 hash of the data, filename, and secret
	MD5Digest digest;
	digest.Add(&hashData, sizeof(hashData));
	digest.Add(xattr.GetBuffer(), xattr.GetSize());
	digest.Add(leafname.c_str(), leafname.size());
	digest.Add(sAttributeHashSecret, sAttributeHashSecretLength);
	digest.Finish();
	
	// Return the first 64 bits of the hash
	uint64_t result = *((uint64_t *)(digest.DigestAsData()));
	return result;
}
