/*
 * Copyright (C) 2026 Huawei Technologies Co.,Ltd.
 *
 * dstore is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * dstore is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. if not, see <https://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------------------
 *
 * dstore_index_handler.cpp
 *        index handler stuff
 *
 *
 * IDENTIFICATION
 *
 *
 * ---------------------------------------------------------------------------------------
 */
#include "index/dstore_index_handler.h"
#include "index/dstore_btree_scan.h"
#include "transaction/dstore_transaction.h"
#include "common/memory/dstore_memory_allocator_stack.h"

namespace DSTORE {

/* This is mainly about initialization of private member m_desc. */
/* remember to call InitIndexScanHandler after construtor. */
IndexScanHandler::IndexScanHandler() : m_scan(nullptr) {}

IndexScanHandler::~IndexScanHandler()
{
    if (m_desc.keyData != nullptr) {
        DstorePfree(m_desc.keyData);
    }
    delete m_scan;
    m_scan = nullptr;
}

/* remember to call it after construtor. */
RetStatus IndexScanHandler::InitIndexScanHandler(StorageRelation indexRel, IndexInfo* indexInfo,
                                                 int numKeys, int numOrderbys, bool showAnyTuples)
{
    StorageAssert(numOrderbys == 0);    /* not support now */
    m_desc.numberOfKeys = numKeys;
    m_desc.numberOfOrderBys = numOrderbys;

    if (numKeys > 0) {
        m_desc.keyData = (ScanKey)DstorePalloc(static_cast<unsigned int>(numKeys) * sizeof(ScanKeyData));
        if (unlikely(m_desc.keyData == nullptr)) {
            /* Do not cover the error message from DstorePalloc */
            ErrLog(DSTORE_ERROR, MODULE_INDEX, ErrMsg("DstorePalloc fail when IndexScanHandler."));
            storage_set_error(INDEX_ERROR_MEMORY_ALLOC);
            return DSTORE_FAIL;
        }
    } else {
        m_desc.keyData = nullptr;
    }

    m_desc.wantItup = false; /* will be set true if indexonlyscan by index interface later */
    m_desc.wantPartOid = (indexInfo->relKind == static_cast<char>(SYS_RELKIND_GLOBAL_INDEX));
    m_desc.wantXid = showAnyTuples;

    /* results */
    m_desc.itup = nullptr;
    m_desc.itupDesc = indexInfo->attributes;
    m_desc.heapCtid = INVALID_ITEM_POINTER;
    m_desc.needRecheck = false;
    m_desc.insertXid = INVALID_XID;
    m_desc.deleteXid = INVALID_XID;

    /* intialize BtreeScan class. */
    m_scan = DstoreNew(g_dstoreCurrentMemoryContext) BtreeScan(indexRel, indexInfo);
    if (unlikely(m_scan == nullptr)) {
        storage_set_error(INDEX_ERROR_MEMORY_ALLOC);
        ErrLog(DSTORE_ERROR, MODULE_INDEX, ErrMsg("DstoreNew fail when create BtreeScan object."));
        return DSTORE_FAIL;
    }
    m_pdbId = indexRel->m_pdbId;
    return DSTORE_SUCC;
}

RetStatus IndexScanHandler::BeginScan()
{
    return m_scan->BeginScan(&m_desc);
}

RetStatus IndexScanHandler::PointGetUnique(ScanKey skey, ItemPointerData *heapCtid)
{
    AutoMemCxtSwitch autoMemCxtSwitch(thrd->m_memoryMgr->GetGroupContext(MEMORY_CONTEXT_QUERY));
    return m_scan->PointGetUnique(skey, heapCtid);
}

RetStatus IndexScanHandler::ReScan(ScanKey skey)
{
    /* step1: Store scankey */
    if (skey && m_desc.numberOfKeys > 0) {
        errno_t rc = memmove_s(m_desc.keyData,
                               static_cast<uint32>(m_desc.numberOfKeys) * sizeof(ScanKeyData),
                               skey,
                               static_cast<uint32>(m_desc.numberOfKeys) * sizeof(ScanKeyData));
        storage_securec_check(rc, "\0", "\0");
    }
    /* step2: pass scankey info to btreeScan */
    return m_scan->ReScan(&m_desc);
}

void IndexScanHandler::EndScan()
{
    m_scan->EndScan();
}

RetStatus IndexScanHandler::GetNextTuple(ScanDirection dir, bool *found)
{
    AutoMemCxtSwitch autoMemCxtSwitch(thrd->m_memoryMgr->GetGroupContext(MEMORY_CONTEXT_QUERY));
    return m_scan->GetNextTuple(&m_desc, dir, found);
}

Oid IndexScanHandler::GetStorageRelOid()
{
    return m_storageRelOid;
}

void IndexScanHandler::SetStorageRelOid(Oid relOid)
{
    m_storageRelOid = relOid;
}

RetStatus IndexScanHandler::SetShowAnyTuples(bool showAnyTuples)
{
    return m_scan->SetShowAnyTuples(showAnyTuples);
}

void IndexScanHandler::InitSnapshot(Snapshot snapshot)
{
    m_scan->InitSnapshot(snapshot);
}

void IndexScanHandler::DumpScanPage(Datum &fileId, Datum &blockId, Datum &data)
{
    m_scan->DumpScanPage(fileId, blockId, data);
}

void IndexScanHandler::MarkPosition()
{
    m_scan->MarkPosition();
}

void IndexScanHandler::RestorePosition()
{
    m_scan->RestorePosition();
}

#ifdef UT
void IndexScanHandler::GetScanStatus(bool &keysConflictFlag, int &numberOfKeys, int &numArrayKeys, ScanKey &skey,
                                     bool &isCrExtend)
{
    m_scan->GetProcessedScanKeyInfo(keysConflictFlag, numberOfKeys, numArrayKeys, skey, isCrExtend);
}
#endif
}
