/*
 * Copyright 2005 - 2016 Zarafa and its licensors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <kopano/platform.h>
#include <kopano/ECInterfaceDefs.h>
#include <kopano/mapi_ptr.h>
#include "kcore.hpp"
#include "ics.h"
#include "pcutil.hpp"
#include "ECMessage.h"
#include "ECMAPIFolder.h"
#include "ECMAPITable.h"
#include "ECExchangeModifyTable.h"
#include "ECExchangeImportHierarchyChanges.h"
#include "ECExchangeImportContentsChanges.h"
#include "ECExchangeExportChanges.h"
#include "WSTransport.h"
#include "WSMessageStreamExporter.h"
#include "WSMessageStreamImporter.h"

#include "Mem.h"
#include <kopano/ECGuid.h>
#include <edkguid.h>
#include <kopano/Util.h>
#include "ClientUtil.h"

#include <kopano/ECDebug.h>
#include <kopano/mapi_ptr.h>

#include <edkmdb.h>
#include <kopano/mapiext.h>
#include <mapiutil.h>
#include <cstdio>

#include <kopano/stringutil.h>

#include <kopano/charset/convstring.h>

static LONG __stdcall AdviseECFolderCallback(void *lpContext, ULONG cNotif,
    LPNOTIFICATION lpNotif)
{
	if (lpContext == NULL)
		return S_OK;

	ECMAPIFolder *lpFolder = (ECMAPIFolder*)lpContext;

	lpFolder->m_bReload = TRUE;

	return S_OK;
}

ECMAPIFolder::ECMAPIFolder(ECMsgStore *lpMsgStore, BOOL fModify,
    WSMAPIFolderOps *lpFolderOps, const char *szClassName) :
	ECMAPIContainer(lpMsgStore, MAPI_FOLDER, fModify, szClassName)
{
	// Folder counters
	HrAddPropHandlers(PR_ASSOC_CONTENT_COUNT,		GetPropHandler,	DefaultSetPropComputed, (void *)this);
	HrAddPropHandlers(PR_CONTENT_COUNT,				GetPropHandler,	DefaultSetPropComputed, (void *)this);
	HrAddPropHandlers(PR_CONTENT_UNREAD,			GetPropHandler,	DefaultSetPropComputed,	(void *)this);
	HrAddPropHandlers(PR_SUBFOLDERS,				GetPropHandler,	DefaultSetPropComputed,	(void *)this);
	HrAddPropHandlers(PR_FOLDER_CHILD_COUNT,		GetPropHandler,	DefaultSetPropComputed,	(void *)this);
	HrAddPropHandlers(PR_DELETED_MSG_COUNT,			GetPropHandler,	DefaultSetPropComputed, (void *)this);
	HrAddPropHandlers(PR_DELETED_FOLDER_COUNT,		GetPropHandler,	DefaultSetPropComputed, (void *)this);
	HrAddPropHandlers(PR_DELETED_ASSOC_MSG_COUNT,	GetPropHandler,	DefaultSetPropComputed, (void *)this);

	HrAddPropHandlers(PR_CONTAINER_CONTENTS,		GetPropHandler,	DefaultSetPropIgnore, (void *)this, FALSE, FALSE);
	HrAddPropHandlers(PR_FOLDER_ASSOCIATED_CONTENTS,GetPropHandler,	DefaultSetPropIgnore, (void *)this, FALSE, FALSE);
	HrAddPropHandlers(PR_CONTAINER_HIERARCHY,		GetPropHandler,	DefaultSetPropIgnore, (void *)this, FALSE, FALSE);

	HrAddPropHandlers(PR_ACCESS,			GetPropHandler,			DefaultSetPropComputed, (void *)this);
	HrAddPropHandlers(PR_RIGHTS,			DefaultMAPIGetProp,		DefaultSetPropComputed, (void*) this);
	HrAddPropHandlers(PR_MESSAGE_SIZE,		GetPropHandler,			DefaultSetPropComputed,	(void*) this, FALSE, FALSE);
	
	HrAddPropHandlers(PR_FOLDER_TYPE,		DefaultMAPIGetProp,		DefaultSetPropComputed, (void*) this);

	// ACLs are only offline
	HrAddPropHandlers(PR_ACL_DATA,			GetPropHandler,			SetPropHandler,			(void*)this);

	this->lpFolderOps = lpFolderOps;
	if (lpFolderOps)
		lpFolderOps->AddRef();

	this->isTransactedObject = FALSE;

	m_lpFolderAdviseSink = NULL;
	m_ulConnection = 0;
}

ECMAPIFolder::~ECMAPIFolder()
{
	if(lpFolderOps)
		lpFolderOps->Release();

	if (m_ulConnection > 0)
		GetMsgStore()->m_lpNotifyClient->UnRegisterAdvise(m_ulConnection);

	if (m_lpFolderAdviseSink)
		m_lpFolderAdviseSink->Release();

}

HRESULT ECMAPIFolder::Create(ECMsgStore *lpMsgStore, BOOL fModify, WSMAPIFolderOps *lpFolderOps, ECMAPIFolder **lppECMAPIFolder)
{
	HRESULT hr = hrSuccess;
	ECMAPIFolder *lpMAPIFolder = NULL;

	lpMAPIFolder = new ECMAPIFolder(lpMsgStore, fModify, lpFolderOps, "IMAPIFolder");

	hr = lpMAPIFolder->QueryInterface(IID_ECMAPIFolder, (void **)lppECMAPIFolder);

	if(hr != hrSuccess)
		delete lpMAPIFolder;

	return hr;
}

HRESULT ECMAPIFolder::GetPropHandler(ULONG ulPropTag, void* lpProvider, ULONG ulFlags, LPSPropValue lpsPropValue, void *lpParam, void *lpBase)
{
	HRESULT hr = hrSuccess;
	ECMAPIFolder *lpFolder = (ECMAPIFolder *)lpParam;

	switch(ulPropTag) {
	case PR_CONTENT_COUNT:
	case PR_CONTENT_UNREAD:
	case PR_DELETED_MSG_COUNT:
	case PR_DELETED_FOLDER_COUNT:
	case PR_DELETED_ASSOC_MSG_COUNT:
	case PR_ASSOC_CONTENT_COUNT:
	case PR_FOLDER_CHILD_COUNT:
		if(lpFolder->HrGetRealProp(ulPropTag, ulFlags, lpBase, lpsPropValue) != hrSuccess)
		{
			// Don't return an error here: outlook is relying on PR_CONTENT_COUNT, etc being available at all times. Especially the
			// exit routine (which checks to see how many items are left in the outbox) will crash if PR_CONTENT_COUNT is MAPI_E_NOT_FOUND
			lpsPropValue->ulPropTag = ulPropTag;
			lpsPropValue->Value.ul = 0;
		}
		break;
	case PR_SUBFOLDERS:
		if(lpFolder->HrGetRealProp(ulPropTag, ulFlags, lpBase, lpsPropValue) != hrSuccess)
		{
			lpsPropValue->ulPropTag = PR_SUBFOLDERS;
			lpsPropValue->Value.b = FALSE;
		}
		break;
	case PR_ACCESS:
		if(lpFolder->HrGetRealProp(PR_ACCESS, ulFlags, lpBase, lpsPropValue) != hrSuccess)
		{
			lpsPropValue->ulPropTag = PR_ACCESS;
			lpsPropValue->Value.l = 0; // FIXME: tijdelijk voor test
		}
		break;
	case PR_CONTAINER_CONTENTS:
	case PR_FOLDER_ASSOCIATED_CONTENTS:
	case PR_CONTAINER_HIERARCHY:
		lpsPropValue->ulPropTag = ulPropTag;
		lpsPropValue->Value.x = 1;
		break;
	case PR_ACL_DATA:
		hr = lpFolder->GetSerializedACLData(lpBase, lpsPropValue);
		if (hr == hrSuccess)
			lpsPropValue->ulPropTag = PR_ACL_DATA;
		else {
			lpsPropValue->ulPropTag = CHANGE_PROP_TYPE(PR_ACL_DATA, PT_ERROR);
			lpsPropValue->Value.err = hr;
		}
		break;
	default:
		hr = MAPI_E_NOT_FOUND;
		break;
	}

	return hr;
}

HRESULT	ECMAPIFolder::SetPropHandler(ULONG ulPropTag, void* lpProvider, LPSPropValue lpsPropValue, void *lpParam)
{
	HRESULT hr = hrSuccess;
	ECMAPIFolder *lpFolder = (ECMAPIFolder *)lpParam;

	switch(ulPropTag) {
	case PR_ACL_DATA:
		hr = lpFolder->SetSerializedACLData(lpsPropValue);
		break;
	default:
		hr = MAPI_E_NOT_FOUND;
		break;
	}

	return hr;
}

// This is similar to GetPropHandler, but works is a static function, and therefore cannot access
// an open ECMAPIFolder object. (The folder is most probably also not open, so ...
HRESULT ECMAPIFolder::TableRowGetProp(void* lpProvider, struct propVal *lpsPropValSrc, LPSPropValue lpsPropValDst, void **lpBase, ULONG ulType) {
	HRESULT hr = hrSuccess;

	switch(lpsPropValSrc->ulPropTag) {

	case PROP_TAG(PT_ERROR,PROP_ID(PR_DISPLAY_TYPE)):
		lpsPropValDst->Value.l = DT_FOLDER;
		lpsPropValDst->ulPropTag = PR_DISPLAY_TYPE;
		break;
	
	default:
		hr = MAPI_E_NOT_FOUND;
	}

	return hr;
}

HRESULT	ECMAPIFolder::QueryInterface(REFIID refiid, void **lppInterface) 
{
	REGISTER_INTERFACE2(ECMAPIFolder, this);
	REGISTER_INTERFACE2(ECMAPIContainer, this);
	REGISTER_INTERFACE2(ECMAPIProp, this);
	REGISTER_INTERFACE2(ECUnknown, this);
	REGISTER_INTERFACE2(IMAPIFolder, &this->m_xMAPIFolder);
	REGISTER_INTERFACE2(IMAPIContainer, &this->m_xMAPIFolder);
	REGISTER_INTERFACE2(IMAPIProp, &this->m_xMAPIFolder);
	REGISTER_INTERFACE2(IUnknown, &this->m_xMAPIFolder);
	REGISTER_INTERFACE2(IFolderSupport, &this->m_xFolderSupport);
	REGISTER_INTERFACE2(IECSecurity, &this->m_xECSecurity);
	REGISTER_INTERFACE3(ISelectUnicode, IUnknown, &this->m_xUnknown);
	return MAPI_E_INTERFACE_NOT_SUPPORTED;
}

HRESULT ECMAPIFolder::HrSetPropStorage(IECPropStorage *lpStorage, BOOL fLoadProps)
{
	HRESULT hr = hrSuccess;
	ULONG ulEventMask = fnevObjectModified  | fnevObjectDeleted | fnevObjectMoved | fnevObjectCreated;
	WSMAPIPropStorage *lpMAPIPropStorage = NULL;
	ULONG cbEntryId;
	LPENTRYID lpEntryId = NULL;

	hr = HrAllocAdviseSink(AdviseECFolderCallback, this, &m_lpFolderAdviseSink);	
	if (hr != hrSuccess)
		goto exit;

	hr = lpStorage->QueryInterface(IID_WSMAPIPropStorage, (void**)&lpMAPIPropStorage);
	if (hr != hrSuccess)
		goto exit;

	hr = lpMAPIPropStorage->GetEntryIDByRef(&cbEntryId, &lpEntryId);
	if (hr != hrSuccess)
		goto exit;

	hr = GetMsgStore()->InternalAdvise(cbEntryId, lpEntryId, ulEventMask, m_lpFolderAdviseSink, &m_ulConnection);
	if (hr == MAPI_E_NO_SUPPORT)
		hr = hrSuccess;			// there is no spoon
	else if (hr != hrSuccess)
		goto exit;
	else
		lpMAPIPropStorage->RegisterAdvise(ulEventMask, m_ulConnection);
	
	hr = ECGenericProp::HrSetPropStorage(lpStorage, fLoadProps);

exit:
	if(lpMAPIPropStorage)
		lpMAPIPropStorage->Release();

	return hr;
}

HRESULT ECMAPIFolder::SetEntryId(ULONG cbEntryId, LPENTRYID lpEntryId)
{
	return ECGenericProp::SetEntryId(cbEntryId, lpEntryId);
}

HRESULT ECMAPIFolder::OpenProperty(ULONG ulPropTag, LPCIID lpiid, ULONG ulInterfaceOptions, ULONG ulFlags, LPUNKNOWN *lppUnk)
{
	HRESULT hr = MAPI_E_INTERFACE_NOT_SUPPORTED;
	SPropValuePtr ptrSK, ptrDisplay;
	
	if (lpiid == NULL)
		return MAPI_E_INVALID_PARAMETER;

	if(ulPropTag == PR_CONTAINER_CONTENTS) {
		if (*lpiid == IID_IMAPITable)
			hr = GetContentsTable(ulInterfaceOptions, (LPMAPITABLE*)lppUnk);
	} else if(ulPropTag == PR_FOLDER_ASSOCIATED_CONTENTS) {
		if (*lpiid == IID_IMAPITable)
			hr = GetContentsTable( (ulInterfaceOptions|MAPI_ASSOCIATED), (LPMAPITABLE*)lppUnk);
	} else if(ulPropTag == PR_CONTAINER_HIERARCHY) {
		if(*lpiid == IID_IMAPITable)
			hr = GetHierarchyTable(ulInterfaceOptions, (LPMAPITABLE*)lppUnk);
	} else if(ulPropTag == PR_RULES_TABLE) {
		if(*lpiid == IID_IExchangeModifyTable)
			hr = ECExchangeModifyTable::CreateRulesTable(this, ulInterfaceOptions, (LPEXCHANGEMODIFYTABLE*)lppUnk);
	} else if(ulPropTag == PR_ACL_TABLE) {
		if(*lpiid == IID_IExchangeModifyTable)
			hr = ECExchangeModifyTable::CreateACLTable(this, ulInterfaceOptions, (LPEXCHANGEMODIFYTABLE*)lppUnk);
	} else if(ulPropTag == PR_COLLECTOR) {
		if(*lpiid == IID_IExchangeImportHierarchyChanges)
			hr = ECExchangeImportHierarchyChanges::Create(this, (LPEXCHANGEIMPORTHIERARCHYCHANGES*)lppUnk);
		else if(*lpiid == IID_IExchangeImportContentsChanges)
			hr = ECExchangeImportContentsChanges::Create(this, (LPEXCHANGEIMPORTCONTENTSCHANGES*)lppUnk);
	} else if(ulPropTag == PR_HIERARCHY_SYNCHRONIZER) {
		hr = HrGetOneProp(&m_xMAPIProp, PR_SOURCE_KEY, &~ptrSK);
		if(hr != hrSuccess)
			return hr;
		HrGetOneProp(&m_xMAPIProp, PR_DISPLAY_NAME_W, &~ptrDisplay); // ignore error
		hr = ECExchangeExportChanges::Create(this->GetMsgStore(), *lpiid, std::string((const char*)ptrSK->Value.bin.lpb, ptrSK->Value.bin.cb), !ptrDisplay ? L"" : ptrDisplay->Value.lpszW, ICS_SYNC_HIERARCHY, (LPEXCHANGEEXPORTCHANGES*) lppUnk);
	} else if(ulPropTag == PR_CONTENTS_SYNCHRONIZER) {
		hr = HrGetOneProp(&m_xMAPIProp, PR_SOURCE_KEY, &~ptrSK);
		if(hr != hrSuccess)
			return hr;
		HrGetOneProp(&m_xMAPIProp, PR_DISPLAY_NAME, &~ptrDisplay); // ignore error
		hr = ECExchangeExportChanges::Create(this->GetMsgStore(), *lpiid, std::string((const char*)ptrSK->Value.bin.lpb, ptrSK->Value.bin.cb), !ptrDisplay ? L"" : ptrDisplay->Value.lpszW, ICS_SYNC_CONTENTS, (LPEXCHANGEEXPORTCHANGES*) lppUnk);
	} else {
		hr = ECMAPIProp::OpenProperty(ulPropTag, lpiid, ulInterfaceOptions, ulFlags, lppUnk);
	}
	return hr;
}

HRESULT ECMAPIFolder::CopyTo(ULONG ciidExclude, LPCIID rgiidExclude, LPSPropTagArray lpExcludeProps, ULONG ulUIParam, LPMAPIPROGRESS lpProgress, LPCIID lpInterface, LPVOID lpDestObj, ULONG ulFlags, LPSPropProblemArray *lppProblems)
{
	return Util::DoCopyTo(&IID_IMAPIFolder, &this->m_xMAPIFolder, ciidExclude, rgiidExclude, lpExcludeProps, ulUIParam, lpProgress, lpInterface, lpDestObj, ulFlags, lppProblems);
}

HRESULT ECMAPIFolder::CopyProps(LPSPropTagArray lpIncludeProps, ULONG ulUIParam, LPMAPIPROGRESS lpProgress, LPCIID lpInterface, LPVOID lpDestObj, ULONG ulFlags, LPSPropProblemArray *lppProblems)
{
	return Util::DoCopyProps(&IID_IMAPIFolder, &this->m_xMAPIFolder, lpIncludeProps, ulUIParam, lpProgress, lpInterface, lpDestObj, ulFlags, lppProblems);
}

HRESULT ECMAPIFolder::SetProps(ULONG cValues, LPSPropValue lpPropArray, LPSPropProblemArray *lppProblems)
{
	HRESULT hr;

	hr = ECMAPIContainer::SetProps(cValues, lpPropArray, lppProblems);
	if (hr != hrSuccess)
		return hr;

	return ECMAPIContainer::SaveChanges(KEEP_OPEN_READWRITE);
}

HRESULT ECMAPIFolder::DeleteProps(LPSPropTagArray lpPropTagArray, LPSPropProblemArray *lppProblems)
{
	HRESULT hr;

	hr = ECMAPIContainer::DeleteProps(lpPropTagArray, lppProblems);
	if (hr != hrSuccess)
		return hr;

	return ECMAPIContainer::SaveChanges(KEEP_OPEN_READWRITE);
}

HRESULT ECMAPIFolder::SaveChanges(ULONG ulFlags)
{
	return hrSuccess;
}

HRESULT ECMAPIFolder::SetSearchCriteria(LPSRestriction lpRestriction, LPENTRYLIST lpContainerList, ULONG ulSearchFlags)
{
	if (lpFolderOps == NULL)
		return MAPI_E_NO_SUPPORT;
	return lpFolderOps->HrSetSearchCriteria(lpContainerList, lpRestriction, ulSearchFlags);
}

HRESULT ECMAPIFolder::GetSearchCriteria(ULONG ulFlags, LPSRestriction *lppRestriction, LPENTRYLIST *lppContainerList, ULONG *lpulSearchState)
{
	if (lpFolderOps == NULL)
		return MAPI_E_NO_SUPPORT;
	// FIXME ulFlags ignore
	return lpFolderOps->HrGetSearchCriteria(lppContainerList, lppRestriction, lpulSearchState);
}

HRESULT ECMAPIFolder::CreateMessage(LPCIID lpInterface, ULONG ulFlags, LPMESSAGE *lppMessage)
{
    return CreateMessageWithEntryID(lpInterface, ulFlags, 0, NULL, lppMessage);
}

HRESULT ECMAPIFolder::CreateMessageWithEntryID(LPCIID lpInterface, ULONG ulFlags, ULONG cbEntryID, LPENTRYID lpEntryID, LPMESSAGE *lppMessage)
{
	HRESULT		hr = hrSuccess;
	ECMessage	*lpMessage = NULL;	
	LPMAPIUID	lpMapiUID = NULL;
	ULONG		cbNewEntryId = 0;
	LPENTRYID	lpNewEntryId = NULL;
	SPropValue	sPropValue[3];
	IECPropStorage*	lpStorage = NULL;

	if(!fModify) {
		hr = MAPI_E_NO_ACCESS;
		goto exit;
	}

	hr = ECMessage::Create(this->GetMsgStore(), TRUE, TRUE, ulFlags & MAPI_ASSOCIATED, FALSE, NULL, &lpMessage);
	if(hr != hrSuccess)
		goto exit;

    if(cbEntryID == 0 || lpEntryID == NULL || HrCompareEntryIdWithStoreGuid(cbEntryID, lpEntryID, &this->GetMsgStore()->GetStoreGuid()) != hrSuccess) {
		// No entryid passed or bad entryid passed, create one
		hr = HrCreateEntryId(GetMsgStore()->GetStoreGuid(), MAPI_MESSAGE, &cbNewEntryId, &lpNewEntryId);
		if (hr != hrSuccess)
			goto exit;

		hr = lpMessage->SetEntryId(cbNewEntryId, lpNewEntryId);
		if (hr != hrSuccess)
			goto exit;

		hr = this->GetMsgStore()->lpTransport->HrOpenPropStorage(m_cbEntryId, m_lpEntryId, cbNewEntryId, lpNewEntryId, ulFlags & MAPI_ASSOCIATED, &lpStorage);
		if(hr != hrSuccess)
			goto exit;

	} else {
		// use the passed entryid
        hr = lpMessage->SetEntryId(cbEntryID, lpEntryID);
        if(hr != hrSuccess)
            goto exit;

		hr = this->GetMsgStore()->lpTransport->HrOpenPropStorage(m_cbEntryId, m_lpEntryId, cbEntryID, lpEntryID, ulFlags & MAPI_ASSOCIATED, &lpStorage);
		if(hr != hrSuccess)
			goto exit;
    }

	hr = lpMessage->HrSetPropStorage(lpStorage, FALSE);
	if(hr != hrSuccess)
		goto exit;

	// Load an empty property set
	hr = lpMessage->HrLoadEmptyProps();
	if(hr != hrSuccess)
		goto exit;

	//Set defaults
	// Same as ECAttach::OpenProperty
	hr = ECAllocateBuffer(sizeof(MAPIUID), reinterpret_cast<void **>(&lpMapiUID));
	if (hr != hrSuccess)
		goto exit;
	hr = this->GetMsgStore()->lpSupport->NewUID(lpMapiUID);
	if(hr != hrSuccess)
		goto exit;

	sPropValue[0].ulPropTag = PR_MESSAGE_FLAGS;
	sPropValue[0].Value.l = MSGFLAG_UNSENT | MSGFLAG_READ;

	sPropValue[1].ulPropTag = PR_MESSAGE_CLASS_A;
	sPropValue[1].Value.lpszA = const_cast<char *>("IPM");
		
	sPropValue[2].ulPropTag = PR_SEARCH_KEY;
	sPropValue[2].Value.bin.cb = sizeof(MAPIUID);
	sPropValue[2].Value.bin.lpb = (LPBYTE)lpMapiUID;

	lpMessage->SetProps(3, sPropValue, NULL);

	// We don't actually create the object until savechanges is called, so remember in which
	// folder it was created
	hr = Util::HrCopyEntryId(this->m_cbEntryId, this->m_lpEntryId, &lpMessage->m_cbParentID, &lpMessage->m_lpParentID);
	if(hr != hrSuccess)
		goto exit;

	if(lpInterface)
		hr = lpMessage->QueryInterface(*lpInterface, (void **)lppMessage);
	else
		hr = lpMessage->QueryInterface(IID_IMessage, (void **)lppMessage);

	AddChild(lpMessage);

exit:
	if (lpStorage)
		lpStorage->Release();

	if (lpNewEntryId)
		ECFreeBuffer(lpNewEntryId);

	if(lpMapiUID)
		ECFreeBuffer(lpMapiUID);

	if(lpMessage)
		lpMessage->Release();

	return hr;
}

HRESULT ECMAPIFolder::CopyMessages(LPENTRYLIST lpMsgList, LPCIID lpInterface, LPVOID lpDestFolder, ULONG ulUIParam, LPMAPIPROGRESS lpProgress, ULONG ulFlags)
{
	HRESULT hr = hrSuccess;
	HRESULT hrEC = hrSuccess;
	IMAPIFolder	*lpMapiFolder = NULL;
	LPSPropValue lpDestPropArray = NULL;

	LPENTRYLIST lpMsgListEC = NULL;
	LPENTRYLIST lpMsgListSupport = NULL;
	unsigned int i;
	GUID		guidFolder;
	GUID		guidMsg;

	if(lpMsgList == NULL || lpMsgList->cValues == 0)
		goto exit;

	if (lpMsgList->lpbin == NULL) {
		hr = MAPI_E_INVALID_PARAMETER;
		goto exit;
	}

	// FIXME progress bar
	
	//Get the interface of destinationfolder
	if(lpInterface == NULL || *lpInterface == IID_IMAPIFolder)
		hr = ((IMAPIFolder*)lpDestFolder)->QueryInterface(IID_IMAPIFolder, (void**)&lpMapiFolder);
	else if(*lpInterface == IID_IMAPIContainer)
		hr = ((IMAPIContainer*)lpDestFolder)->QueryInterface(IID_IMAPIFolder, (void**)&lpMapiFolder);
	else if(*lpInterface == IID_IUnknown)
		hr = ((IUnknown*)lpDestFolder)->QueryInterface(IID_IMAPIFolder, (void**)&lpMapiFolder);
	else if(*lpInterface == IID_IMAPIProp)
		hr = ((IMAPIProp*)lpDestFolder)->QueryInterface(IID_IMAPIFolder, (void**)&lpMapiFolder);
	else
		hr = MAPI_E_INTERFACE_NOT_SUPPORTED;
	
	if(hr != hrSuccess)
		goto exit;

	// Get the destination entry ID, and check for favories public folders, so get PR_ORIGINAL_ENTRYID first.
	hr = HrGetOneProp(lpMapiFolder, PR_ORIGINAL_ENTRYID, &lpDestPropArray);
	if (hr != hrSuccess)
		hr = HrGetOneProp(lpMapiFolder, PR_ENTRYID, &lpDestPropArray);
	if (hr != hrSuccess)
		goto exit;

	// Check if the destination entryid is a kopano entryid and if there is a folder transport
	if( IsKopanoEntryId(lpDestPropArray[0].Value.bin.cb, lpDestPropArray[0].Value.bin.lpb) &&
		lpFolderOps != NULL) 
	{
		hr = HrGetStoreGuidFromEntryId(lpDestPropArray[0].Value.bin.cb, lpDestPropArray[0].Value.bin.lpb, &guidFolder);
		if(hr != hrSuccess)
			goto exit;

		// Allocate memory for support list and kopano list
		hr = ECAllocateBuffer(sizeof(ENTRYLIST), (void**)&lpMsgListEC);
		if(hr != hrSuccess)
			goto exit;
		
		lpMsgListEC->cValues = 0;

		hr = ECAllocateMore(sizeof(SBinary) * lpMsgList->cValues, lpMsgListEC, (void**)&lpMsgListEC->lpbin);
		if(hr != hrSuccess)
			goto exit;
		
		hr = ECAllocateBuffer(sizeof(ENTRYLIST), (void**)&lpMsgListSupport);
		if(hr != hrSuccess)
			goto exit;
		
		lpMsgListSupport->cValues = 0;

		hr = ECAllocateMore(sizeof(SBinary) * lpMsgList->cValues, lpMsgListSupport, (void**)&lpMsgListSupport->lpbin);
		if(hr != hrSuccess)
			goto exit;
	

		//FIXME
		//hr = lpMapiFolder->SetReadFlags(GENERATE_RECEIPT_ONLY);
		//if(hr != hrSuccess)
			//goto exit;

		// Check if right store	
		for (i = 0; i < lpMsgList->cValues; ++i) {
			hr = HrGetStoreGuidFromEntryId(lpMsgList->lpbin[i].cb, lpMsgList->lpbin[i].lpb, &guidMsg);
			// check if the message in the store of the folder (serverside copy possible)
			if(hr == hrSuccess && IsKopanoEntryId(lpMsgList->lpbin[i].cb, lpMsgList->lpbin[i].lpb) && memcmp(&guidMsg, &guidFolder, sizeof(MAPIUID)) == 0)
				lpMsgListEC->lpbin[lpMsgListEC->cValues++] = lpMsgList->lpbin[i];// cheap copy
			else
				lpMsgListSupport->lpbin[lpMsgListSupport->cValues++] = lpMsgList->lpbin[i];// cheap copy

			hr = hrSuccess;
		}
		
		if(lpMsgListEC->cValues > 0)
		{
			hr = this->lpFolderOps->HrCopyMessage(lpMsgListEC, lpDestPropArray[0].Value.bin.cb, (LPENTRYID)lpDestPropArray[0].Value.bin.lpb, ulFlags, 0);
			if(FAILED(hr))
				goto exit;
			hrEC = hr;
		}

		if(lpMsgListSupport->cValues > 0)
		{
			hr = this->GetMsgStore()->lpSupport->CopyMessages(&IID_IMAPIFolder, &this->m_xMAPIFolder, lpMsgListSupport, lpInterface, lpDestFolder, ulUIParam, lpProgress, ulFlags);
			if(FAILED(hr))
				goto exit;
		}

	}else
	{
		// Do copy with the storeobject
		// Copy between two or more different stores
		hr = this->GetMsgStore()->lpSupport->CopyMessages(&IID_IMAPIFolder, &this->m_xMAPIFolder, lpMsgList, lpInterface, lpDestFolder, ulUIParam, lpProgress, ulFlags);
	}	

exit:
	if (lpDestPropArray != NULL)
		ECFreeBuffer(lpDestPropArray);
	if(lpMsgListEC)
		ECFreeBuffer(lpMsgListEC);

	if(lpMsgListSupport)
		ECFreeBuffer(lpMsgListSupport);

	if(lpMapiFolder)
		lpMapiFolder->Release();

	return (hr == hrSuccess)?hrEC:hr;
}

HRESULT ECMAPIFolder::DeleteMessages(LPENTRYLIST lpMsgList, ULONG ulUIParam, LPMAPIPROGRESS lpProgress, ULONG ulFlags)
{
	if (lpMsgList == NULL)
		return MAPI_E_INVALID_PARAMETER;
	if (!ValidateZEntryList(lpMsgList, MAPI_MESSAGE))
		return MAPI_E_INVALID_ENTRYID;
	// FIXME progress bar
	return this->GetMsgStore()->lpTransport->HrDeleteObjects(ulFlags, lpMsgList, 0);
}

HRESULT ECMAPIFolder::CreateFolder(ULONG ulFolderType, LPTSTR lpszFolderName, LPTSTR lpszFolderComment, LPCIID lpInterface, ULONG ulFlags, LPMAPIFOLDER *lppFolder)
{
	HRESULT			hr = hrSuccess;
	ULONG			cbEntryId = 0;
	LPENTRYID		lpEntryId = NULL;
	IMAPIFolder*	lpFolder = NULL;
	ULONG			ulObjType = 0;

	// SC TODO: new code:
	// create new lpFolder object (load empty props ?)
	// create entryid and set it
	// create storage and set it
	// set props (comment)
	// save changes(keep open readwrite)  <- the only call to the server

	if (lpFolderOps == NULL) {
		hr = MAPI_E_NO_SUPPORT;
		goto exit;
	}

	// Create the actual folder on the server
	hr = lpFolderOps->HrCreateFolder(ulFolderType, convstring(lpszFolderName, ulFlags), convstring(lpszFolderComment, ulFlags), ulFlags & OPEN_IF_EXISTS, 0, NULL, 0, NULL, &cbEntryId, &lpEntryId);

	if(hr != hrSuccess)
		goto exit;

	// Open the folder we just created
	hr = this->GetMsgStore()->OpenEntry(cbEntryId, lpEntryId, lpInterface, MAPI_MODIFY | MAPI_DEFERRED_ERRORS, &ulObjType, (IUnknown **)&lpFolder);
	
	if(hr != hrSuccess)
		goto exit;

	*lppFolder = lpFolder;

exit:
	if(hr != hrSuccess && lpFolder)
		lpFolder->Release();

	if(lpEntryId)
		ECFreeBuffer(lpEntryId);

	return hr;
}

// @note if you change this function please look also at ECMAPIFolderPublic::CopyFolder
HRESULT ECMAPIFolder::CopyFolder(ULONG cbEntryID, LPENTRYID lpEntryID, LPCIID lpInterface, LPVOID lpDestFolder, LPTSTR lpszNewFolderName, ULONG ulUIParam, LPMAPIPROGRESS lpProgress, ULONG ulFlags)
{
	HRESULT hr = hrSuccess;
	IMAPIFolder	*lpMapiFolder = NULL;
	LPSPropValue lpPropArray = NULL;
	GUID guidDest;
	GUID guidFrom;

	//Get the interface of destinationfolder
	if(lpInterface == NULL || *lpInterface == IID_IMAPIFolder)
		hr = ((IMAPIFolder*)lpDestFolder)->QueryInterface(IID_IMAPIFolder, (void**)&lpMapiFolder);
	else if(*lpInterface == IID_IMAPIContainer)
		hr = ((IMAPIContainer*)lpDestFolder)->QueryInterface(IID_IMAPIFolder, (void**)&lpMapiFolder);
	else if(*lpInterface == IID_IUnknown)
		hr = ((IUnknown*)lpDestFolder)->QueryInterface(IID_IMAPIFolder, (void**)&lpMapiFolder);
	else if(*lpInterface == IID_IMAPIProp)
		hr = ((IMAPIProp*)lpDestFolder)->QueryInterface(IID_IMAPIFolder, (void**)&lpMapiFolder);
	else
		hr = MAPI_E_INTERFACE_NOT_SUPPORTED;
	
	if(hr != hrSuccess)
		goto exit;

	// Get the destination entry ID
	hr = HrGetOneProp(lpMapiFolder, PR_ENTRYID, &lpPropArray);
	if(hr != hrSuccess)
		goto exit;

	// Check if it's  the same store of kopano so we can copy/move fast
	if( IsKopanoEntryId(cbEntryID, (LPBYTE)lpEntryID) && 
		IsKopanoEntryId(lpPropArray[0].Value.bin.cb, lpPropArray[0].Value.bin.lpb) &&
		HrGetStoreGuidFromEntryId(cbEntryID, (LPBYTE)lpEntryID, &guidFrom) == hrSuccess && 
		HrGetStoreGuidFromEntryId(lpPropArray[0].Value.bin.cb, lpPropArray[0].Value.bin.lpb, &guidDest) == hrSuccess &&
		memcmp(&guidFrom, &guidDest, sizeof(GUID)) == 0 &&
		lpFolderOps != NULL)
	{
		//FIXME: Progressbar
		hr = this->lpFolderOps->HrCopyFolder(cbEntryID, lpEntryID, lpPropArray[0].Value.bin.cb, (LPENTRYID)lpPropArray[0].Value.bin.lpb, convstring(lpszNewFolderName, ulFlags), ulFlags, 0);
			
	}else
	{
		// Support object handled de copy/move
		hr = this->GetMsgStore()->lpSupport->CopyFolder(&IID_IMAPIFolder, &this->m_xMAPIFolder, cbEntryID, lpEntryID, lpInterface, lpDestFolder, lpszNewFolderName, ulUIParam, lpProgress, ulFlags);
	}

exit:
	if(lpMapiFolder)
		lpMapiFolder->Release();
	if (lpPropArray != NULL)
		ECFreeBuffer(lpPropArray);
	return hr;
}

HRESULT ECMAPIFolder::DeleteFolder(ULONG cbEntryID, LPENTRYID lpEntryID, ULONG ulUIParam, LPMAPIPROGRESS lpProgress, ULONG ulFlags)
{
	if (!ValidateZEntryId(cbEntryID, reinterpret_cast<LPBYTE>(lpEntryID), MAPI_FOLDER))
		return MAPI_E_INVALID_ENTRYID;
	if (lpFolderOps == NULL)
		return MAPI_E_NO_SUPPORT;
	return this->lpFolderOps->HrDeleteFolder(cbEntryID, lpEntryID, ulFlags, 0);
}

HRESULT ECMAPIFolder::SetReadFlags(LPENTRYLIST lpMsgList, ULONG ulUIParam, LPMAPIPROGRESS lpProgress, ULONG ulFlags)
{
	HRESULT		hr = hrSuccess;
	LPMESSAGE	lpMessage = NULL;
	BOOL		bError = FALSE;
	ULONG		ulObjType = 0;

	// Progress bar
	ULONG ulPGMin = 0;
	ULONG ulPGMax = 0;
	ULONG ulPGDelta = 0;
	ULONG ulPGFlags = 0;
	
	if((ulFlags &~ (CLEAR_READ_FLAG | CLEAR_NRN_PENDING | CLEAR_RN_PENDING | GENERATE_RECEIPT_ONLY | MAPI_DEFERRED_ERRORS | MESSAGE_DIALOG | SUPPRESS_RECEIPT)) != 0 ||
		(ulFlags & (SUPPRESS_RECEIPT | CLEAR_READ_FLAG)) == (SUPPRESS_RECEIPT | CLEAR_READ_FLAG) ||
		(ulFlags & (SUPPRESS_RECEIPT | CLEAR_READ_FLAG | GENERATE_RECEIPT_ONLY)) == (SUPPRESS_RECEIPT | CLEAR_READ_FLAG | GENERATE_RECEIPT_ONLY) ||
		(ulFlags & (CLEAR_READ_FLAG | GENERATE_RECEIPT_ONLY)) == (CLEAR_READ_FLAG | GENERATE_RECEIPT_ONLY)	)
	{
		hr = MAPI_E_INVALID_PARAMETER;
		goto exit;
	}

	if (lpFolderOps == NULL) {
		hr = MAPI_E_NO_SUPPORT;
		goto exit;
	}

	//FIXME: (GENERATE_RECEIPT_ONLY | SUPPRESS_RECEIPT) not yet implement ok on the server (update PR_READ_RECEIPT_REQUESTED to false)
	if( (!(ulFlags & (SUPPRESS_RECEIPT|CLEAR_READ_FLAG|CLEAR_NRN_PENDING|CLEAR_RN_PENDING)) || (ulFlags&GENERATE_RECEIPT_ONLY))&& lpMsgList){
		if((ulFlags&MESSAGE_DIALOG ) && lpProgress) {
			lpProgress->GetMin(&ulPGMin);
			lpProgress->GetMax(&ulPGMax);
			ulPGDelta = (ulPGMax-ulPGMin);
			lpProgress->GetFlags(&ulPGFlags);
		}

		for (ULONG i = 0; i < lpMsgList->cValues; ++i) {
			if(OpenEntry(lpMsgList->lpbin[i].cb, (LPENTRYID)lpMsgList->lpbin[i].lpb, &IID_IMessage, MAPI_MODIFY, &ulObjType, (LPUNKNOWN*)&lpMessage) == hrSuccess)
			{
				if(lpMessage->SetReadFlag(ulFlags&~MESSAGE_DIALOG) != hrSuccess)
					bError = TRUE;

				lpMessage->Release(); lpMessage = NULL;
			}else
				bError = TRUE;
			
			// Progress bar
			if((ulFlags&MESSAGE_DIALOG ) && lpProgress) {
				if (ulPGFlags & MAPI_TOP_LEVEL)
					hr = lpProgress->Progress((int)((float)i * ulPGDelta / lpMsgList->cValues + ulPGMin), i, lpMsgList->cValues);
				else
					hr = lpProgress->Progress((int)((float)i * ulPGDelta / lpMsgList->cValues + ulPGMin), 0, 0);

				if(hr == MAPI_E_USER_CANCEL) {// MAPI_E_USER_CANCEL is user click on the Cancel button.
					hr = hrSuccess;
					bError = TRUE;
					goto exit;
				}else if(hr != hrSuccess) {
					goto exit;
				}
			}

		}
	}else {
		hr = lpFolderOps->HrSetReadFlags(lpMsgList, ulFlags, 0);
	}

exit:
	if(hr == hrSuccess && bError == TRUE)
		hr = MAPI_W_PARTIAL_COMPLETION;

	return hr;
}

HRESULT ECMAPIFolder::GetMessageStatus(ULONG cbEntryID, LPENTRYID lpEntryID, ULONG ulFlags, ULONG *lpulMessageStatus)
{
	if (lpEntryID == NULL || !IsKopanoEntryId(cbEntryID, reinterpret_cast<LPBYTE>(lpEntryID)))
		return MAPI_E_INVALID_ENTRYID;
	if (lpulMessageStatus == NULL)
		return MAPI_E_INVALID_OBJECT;
	if (lpFolderOps == NULL)
		return MAPI_E_NO_SUPPORT;
	return lpFolderOps->HrGetMessageStatus(cbEntryID, lpEntryID, ulFlags, lpulMessageStatus);
}

HRESULT ECMAPIFolder::SetMessageStatus(ULONG cbEntryID, LPENTRYID lpEntryID, ULONG ulNewStatus, ULONG ulNewStatusMask, ULONG *lpulOldStatus)
{
	if (lpEntryID == NULL || !IsKopanoEntryId(cbEntryID, reinterpret_cast<LPBYTE>(lpEntryID)))
		return MAPI_E_INVALID_ENTRYID;
	if (lpFolderOps == NULL)
		return MAPI_E_NO_SUPPORT;
	return lpFolderOps->HrSetMessageStatus(cbEntryID, lpEntryID, ulNewStatus, ulNewStatusMask, 0, lpulOldStatus);
}

HRESULT ECMAPIFolder::SaveContentsSort(const SSortOrderSet *lpSortCriteria, ULONG ulFlags)
{
	return MAPI_E_NO_ACCESS;
}

HRESULT ECMAPIFolder::EmptyFolder(ULONG ulUIParam, LPMAPIPROGRESS lpProgress, ULONG ulFlags)
{
	if((ulFlags &~ (DEL_ASSOCIATED | FOLDER_DIALOG | DELETE_HARD_DELETE)) != 0)
		return MAPI_E_INVALID_PARAMETER;
	if (lpFolderOps == NULL)
		return MAPI_E_NO_SUPPORT;
	return lpFolderOps->HrEmptyFolder(ulFlags, 0);
}

HRESULT ECMAPIFolder::GetProps(LPSPropTagArray lpPropTagArray, ULONG ulFlags, ULONG *lpcValues, LPSPropValue *lppPropArray)
{
	HRESULT hr;
	
	// Check if there is a storage needed because favorites and ipmsubtree of the public folder 
	// doesn't have a prop storage.
	if(lpStorage != NULL) {
		hr = HrLoadProps();
		if (hr != hrSuccess)
			return hr;
	}

	return ECMAPIProp::GetProps(lpPropTagArray, ulFlags, lpcValues, lppPropArray);
}

HRESULT ECMAPIFolder::GetSupportMask(DWORD * pdwSupportMask)
{
	if (pdwSupportMask == NULL)
		return MAPI_E_INVALID_PARAMETER;
	*pdwSupportMask = FS_SUPPORTS_SHARING; //Indicates that the folder supports sharing.
	return hrSuccess;
}

HRESULT ECMAPIFolder::CreateMessageFromStream(ULONG ulFlags, ULONG ulSyncId, ULONG cbEntryID, LPENTRYID lpEntryID, WSMessageStreamImporter **lppsStreamImporter)
{
	HRESULT hr;
	WSMessageStreamImporterPtr	ptrStreamImporter;

	hr = GetMsgStore()->lpTransport->HrGetMessageStreamImporter(ulFlags, ulSyncId, cbEntryID, lpEntryID, m_cbEntryId, m_lpEntryId, true, nullptr, &~ptrStreamImporter);
	if (hr != hrSuccess)
		return hr;

	*lppsStreamImporter = ptrStreamImporter.release();
	return hrSuccess;
}

HRESULT ECMAPIFolder::GetChangeInfo(ULONG cbEntryID, LPENTRYID lpEntryID, LPSPropValue *lppPropPCL, LPSPropValue *lppPropCK)
{
	return lpFolderOps->HrGetChangeInfo(cbEntryID, lpEntryID, lppPropPCL, lppPropCK);
}

HRESULT ECMAPIFolder::UpdateMessageFromStream(ULONG ulSyncId, ULONG cbEntryID, LPENTRYID lpEntryID, LPSPropValue lpConflictItems, WSMessageStreamImporter **lppsStreamImporter)
{
	HRESULT hr;
	WSMessageStreamImporterPtr	ptrStreamImporter;

	hr = GetMsgStore()->lpTransport->HrGetMessageStreamImporter(0, ulSyncId, cbEntryID, lpEntryID, m_cbEntryId, m_lpEntryId, false, lpConflictItems, &~ptrStreamImporter);
	if (hr != hrSuccess)
		return hr;

	*lppsStreamImporter = ptrStreamImporter.release();
	return hrSuccess;
}

DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, QueryInterface, (REFIID, refiid), (void **, lppInterface))
DEF_ULONGMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, AddRef, (void))
DEF_ULONGMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, Release, (void))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, GetLastError, (HRESULT, hError), (ULONG, ulFlags), (LPMAPIERROR *, lppMapiError))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, SaveChanges, (ULONG, ulFlags))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, GetProps, (LPSPropTagArray, lpPropTagArray), (ULONG, ulFlags), (ULONG *, lpcValues), (LPSPropValue *, lppPropArray))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, GetPropList, (ULONG, ulFlags), (LPSPropTagArray *, lppPropTagArray))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, OpenProperty, (ULONG, ulPropTag), (LPCIID, lpiid), (ULONG, ulInterfaceOptions), (ULONG, ulFlags), (LPUNKNOWN *, lppUnk))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, SetProps, (ULONG, cValues), (LPSPropValue, lpPropArray), (LPSPropProblemArray *, lppProblems))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, DeleteProps, (LPSPropTagArray, lpPropTagArray), (LPSPropProblemArray *, lppProblems))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, CopyTo, (ULONG, ciidExclude), (LPCIID, rgiidExclude), (LPSPropTagArray, lpExcludeProps), (ULONG, ulUIParam), (LPMAPIPROGRESS, lpProgress), (LPCIID, lpInterface), (LPVOID, lpDestObj), (ULONG, ulFlags), (LPSPropProblemArray *, lppProblems))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, CopyProps, (LPSPropTagArray, lpIncludeProps), (ULONG, ulUIParam), (LPMAPIPROGRESS, lpProgress), (LPCIID, lpInterface), (LPVOID, lpDestObj), (ULONG, ulFlags), (LPSPropProblemArray *, lppProblems))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, GetNamesFromIDs, (LPSPropTagArray *, pptaga), (LPGUID, lpguid), (ULONG, ulFlags), (ULONG *, pcNames), (LPMAPINAMEID **, pppNames))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, GetIDsFromNames, (ULONG, cNames), (LPMAPINAMEID *, ppNames), (ULONG, ulFlags), (LPSPropTagArray *, pptaga))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, GetContentsTable, (ULONG, ulFlags), (LPMAPITABLE *, lppTable))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, GetHierarchyTable, (ULONG, ulFlags), (LPMAPITABLE *, lppTable))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, OpenEntry, (ULONG, cbEntryID), (LPENTRYID, lpEntryID), (LPCIID, lpInterface), (ULONG, ulFlags), (ULONG *, lpulObjType), (LPUNKNOWN *, lppUnk))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, SetSearchCriteria, (LPSRestriction, lpRestriction), (LPENTRYLIST, lpContainerList), (ULONG, ulSearchFlags))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, GetSearchCriteria, (ULONG, ulFlags), (LPSRestriction *, lppRestriction), (LPENTRYLIST *, lppContainerList), (ULONG *, lpulSearchState))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, CreateMessage, (LPCIID, lpInterface), (ULONG, ulFlags), (LPMESSAGE *, lppMessage))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, CopyMessages, (LPENTRYLIST, lpMsgList), (LPCIID, lpInterface), (LPVOID, lpDestFolder), (ULONG, ulUIParam), (LPMAPIPROGRESS, lpProgress), (ULONG, ulFlags))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, DeleteMessages, (LPENTRYLIST, lpMsgList), (ULONG, ulUIParam), (LPMAPIPROGRESS, lpProgress), (ULONG, ulFlags))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, CreateFolder, (ULONG, ulFolderType), (LPTSTR, lpszFolderName), (LPTSTR, lpszFolderComment), (LPCIID, lpInterface), (ULONG, ulFlags), (LPMAPIFOLDER *, lppFolder))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, CopyFolder, (ULONG, cbEntryID), (LPENTRYID, lpEntryID), (LPCIID, lpInterface), (LPVOID, lpDestFolder), (LPTSTR, lpszNewFolderName), (ULONG, ulUIParam), (LPMAPIPROGRESS, lpProgress), (ULONG, ulFlags))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, DeleteFolder, (ULONG, cbEntryID), (LPENTRYID, lpEntryID), (ULONG, ulUIParam), (LPMAPIPROGRESS, lpProgress), (ULONG, ulFlags))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, SetReadFlags, (LPENTRYLIST, lpMsgList), (ULONG, ulUIParam), (LPMAPIPROGRESS, lpProgress), (ULONG, ulFlags))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, GetMessageStatus, (ULONG, cbEntryID), (LPENTRYID, lpEntryID), (ULONG, ulFlags), (ULONG *, lpulMessageStatus))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, SetMessageStatus, (ULONG, cbEntryID), (LPENTRYID, lpEntryID), (ULONG, ulNewStatus), (ULONG, ulNewStatusMask), (ULONG *, lpulOldStatus))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, SaveContentsSort, (const SSortOrderSet *, lpSortCriteria), (ULONG, ulFlags))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, MAPIFolder, EmptyFolder, (ULONG, ulUIParam), (LPMAPIPROGRESS, lpProgress), (ULONG, ulFlags))

// IFolderSupport
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, FolderSupport, QueryInterface, (REFIID, refiid), (void **, lppInterface))
DEF_ULONGMETHOD1(TRACE_MAPI, ECMAPIFolder, FolderSupport, AddRef, (void))
DEF_ULONGMETHOD1(TRACE_MAPI, ECMAPIFolder, FolderSupport, Release, (void))
DEF_HRMETHOD1(TRACE_MAPI, ECMAPIFolder, FolderSupport, GetSupportMask, (DWORD *, pdwSupportMask))
