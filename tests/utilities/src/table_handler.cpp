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
 */

#include "table_handler.h"
#include "table/dstore_table_interface.h"
#include "transaction/dstore_transaction_interface.h"
#include "transaction/dstore_transaction.h"
#include "tablespace/dstore_tablespace_interface.h"
#include "catalog/dstore_function_struct.h"

#include "catalog/dstore_fake_type.h"
#include "catalog/dstore_typecache.h"
#include "errorcode/dstore_index_error_code.h"
#include "framework/dstore_pdb.h"

#include "table_data_generator.h"
#include "common_persistence_handler.h"

#include <atomic>
#include <cstdint>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
using namespace DSTORE;
StorageInstanceInterface *g_instance = nullptr;
StorageTableContext *simulator = nullptr;

DstoreTableHandler::DstoreTableHandler(StorageInstanceInterface *instance) :
    m_instance(instance),
    m_heapRel(nullptr),
    m_indexRel(nullptr),
    m_isCreateByInit(false),
    m_isCreateBtreeByInit(false)
{
}

DstoreTableHandler::~DstoreTableHandler()
{
    Destory();
}

StorageRelation DstoreTableHandler::CopyRelation(StorageRelation rel)
{
    StorageRelation relation = static_cast<StorageRelation>(DstorePalloc0(sizeof(StorageRelationData)));
    StorageAssert(DstoreRelationIsValid(relation));
    
    relation->attr = rel->attr->Copy();
    int fillFactor = DstoreRelationIsIndex(rel) ? INVALID_INDEX_FILLFACTOR : DEFAULT_HEAP_FILLFACTOR;
    __attribute__((__unused__)) RetStatus ret = relation->Construct(g_defaultPdbId, rel->relOid, rel->rel, relation->attr,
        fillFactor, static_cast<TablespaceId>(rel->rel->reltablespace));
    StorageAssert(ret == DSTORE_SUCC);
    relation->rel = static_cast<SysClassTupDef *>(DstorePalloc0(sizeof(SysClassTupDef)));
    StorageAssert(relation->rel != nullptr);
    errno_t rc = memcpy_s(relation->rel, sizeof(SysClassTupDef), rel->rel, sizeof(SysClassTupDef));
    storage_securec_check(rc, "\0", "\0");

    if (DstoreRelationIsIndex(rel)) {
        relation->indexInfo = static_cast<SysIndexTupDef *>(DstorePalloc0(sizeof(SysIndexTupDef)));
        StorageAssert(relation->indexInfo != nullptr);
        rc = memcpy_s(relation->indexInfo, sizeof(SysIndexTupDef), rel->indexInfo, sizeof(SysIndexTupDef));
        storage_securec_check(rc, "\0", "\0");

        relation->index =
            DstoreTableHandler::BuildIndexInfo(relation, rel->index->indexKeyAttrsNum, rel->index->isUnique);
        StorageAssert(relation->index != nullptr);
    } else {
        relation->index = nullptr;
    }

    return relation;
}

void DstoreTableHandler::Init(StorageRelation heapRel, StorageRelation indexRel)
{
    StorageAssert(heapRel != nullptr);
    StorageAssert(heapRel->rel != nullptr);
    StorageAssert(heapRel->attr != nullptr);
    errno_t rc = memcpy_s(m_tableName, NAME_MAX_LEN, heapRel->rel->relname.data, NAME_MAX_LEN);
    storage_securec_check(rc, "\0", "\0");

    /* Building a Local Heap Relation */
    m_heapRel = CopyRelation(heapRel);
    m_isCreateByInit = true;

    /* Building a Local Index Relation */
    if (indexRel) {
        m_indexRel = CopyRelation(indexRel);
        m_isCreateBtreeByInit = true;
    }
}

int DstoreTableHandler::RecoveryTable(const char* tableName)
{
    char *filePath = (char *)DstorePalloc0(VFS_FILE_PATH_MAX_LEN);
    StorageAssert(filePath != nullptr);
    errno_t rc =
        sprintf_s(filePath, VFS_FILE_PATH_MAX_LEN, "%s/%s/%s",
                  (dynamic_cast<StorageInstance *>(m_instance))->GetGuc()->dataDir, TABLE_METADATA_DIR, tableName);
    storage_securec_check_ss(rc);
    if (access(filePath, F_OK) != 0) {
        DstorePfree(filePath);
        StorageAssert(0);
        return 1;
    }

    CommonPersistentHandler tablePersistentHandler;
    StorageRelation relation = ReadDiskTable(&tablePersistentHandler, filePath);
    DstorePfree(filePath);
    tablePersistentHandler.Close();

    StorageAssert(relation != nullptr);

    __attribute__((__unused__)) StorageRelation rel = nullptr;
    if (relation->rel->relkind == SYS_RELKIND_RELATION) {
        rel = simulator->GetHeapRelationEntry(tableName);
    } else if (DstoreRelationIsIndex(relation)) {
        rel = simulator->GetIndexRelationEntry(tableName);
    } else { /* The type is not supported currently. */
        StorageAssert(false);
    }
    if(rel != nullptr) {
        return 0;
    }

    /* Insert into local cache */
    rc = memcpy_s(relation->rel->relname.data, strlen(tableName) + 1, tableName, strlen(tableName) + 1);
    storage_securec_check(rc, "\0", "\0");
    if (relation->rel->relkind == SYS_RELKIND_RELATION) {
        simulator->InsertHeapTable(tableName, relation);
    } else {
        simulator->InsertIndexTable(tableName, relation);
    }

    return 0;
}

int DstoreTableHandler::CreateTable(const TableInfo &tableInfo)
{
    StorageAssert(tableInfo.relation.name != nullptr);
    if (simulator->GetHeapRelationEntry(tableInfo.relation.name) != nullptr) {
        assert(0);
        return 1;
    }
    errno_t rc = memcpy_s(m_tableName, strlen(tableInfo.relation.name) + 1, tableInfo.relation.name,
                          strlen(tableInfo.relation.name) + 1);
    storage_securec_check(rc, "\0", "\0");

    /* generate meta oidInfo */
    Oid relOid = simulator->GetNextOid();
    PageId segmentId = TableSpace_Interface::AllocSegment(g_defaultPdbId,
        static_cast<TablespaceId>(TBS_ID::DEFAULT_TABLE_SPACE_ID), SegmentType::HEAP_SEGMENT_TYPE);
    TupleDesc attr = GenerateHeapTupDesc(relOid, tableInfo.colDesc, tableInfo.relation.attrNum);
    StorageAssert(attr != nullptr);
    /* Build class tuple. */
    TablespaceId tabelSpaceId = static_cast<TablespaceId>(TBS_ID::DEFAULT_TABLE_SPACE_ID);
    SysClassTupDef *classTuple = BuildSysRelationTuple(tableInfo, static_cast<Oid>(tabelSpaceId), segmentId);
    StorageAssert(classTuple != nullptr);
    m_heapRel = static_cast<StorageRelation>(DstorePalloc(sizeof(StorageRelationData)));
    __attribute__((__unused__)) RetStatus ret =
        m_heapRel->Construct(g_defaultPdbId, relOid, classTuple, attr, DEFAULT_HEAP_FILLFACTOR, tabelSpaceId);
    StorageAssert(ret == DSTORE_SUCC);
    m_heapRel->rel = classTuple;
    m_heapRel->indexInfo = nullptr;
    m_heapRel->index = nullptr;
    /* add relation into relcache */
    simulator->InsertHeapTable(m_tableName, m_heapRel);
    FlushTable(m_tableName, m_heapRel);

    return 0;
}

StorageRelation DstoreTableHandler::GetTable(const char *tableName)
{
    StorageAssert(tableName != nullptr);
    StorageRelation heapRel = simulator->GetHeapRelationEntry(tableName);
    StorageAssert(heapRel != nullptr);
    return CopyRelation(heapRel);
}
StorageRelation DstoreTableHandler::GetIndex(const char* indexName)
{
    StorageRelation indexRel = simulator->GetIndexRelationEntry(indexName);
    StorageAssert(indexRel != nullptr);
    return CopyRelation(indexRel);
}

int DstoreTableHandler::CheckTable(const TableInfo &tableInfo)
{
    StorageAssert(m_heapRel != nullptr);
    return CheckTable(m_heapRel, tableInfo);
}

int DstoreTableHandler::CheckIndex(const TableInfo &indexTableInfo)
{
    StorageAssert(m_heapRel != nullptr);
    StorageAssert(m_indexRel != nullptr);

    int ret = CheckTable(m_indexRel, indexTableInfo);
    StorageAssert(m_indexRel->indexInfo->indnatts == static_cast<int16>(indexTableInfo.indexDesc->indexAttrNum));
    StorageAssert(m_indexRel->indexInfo->indexrelid == m_indexRel->relOid);
    StorageAssert(m_indexRel->indexInfo->indrelid == m_heapRel->relOid);
    StorageAssert(m_indexRel->indexInfo->indisunique == indexTableInfo.indexDesc->isUnique);
    return ret;
}


void DstoreTableHandler::Destory()
{
    if (m_indexRel != nullptr) {
        m_indexRel->Destroy();
    }
    if (m_isCreateBtreeByInit) {
        DestroyObject((void**)&m_indexRel->attr);
        DestroyObject((void**)&m_indexRel->rel);
        DestroyObject((void**)&m_indexRel->index);
        DestroyObject((void**)&m_indexRel->indexInfo);
        DestroyObject((void**)&m_indexRel);
        m_indexRel = nullptr;
    }
    
    if (m_heapRel != nullptr) {
        m_heapRel->Destroy();
    }

    if (m_isCreateByInit) {
        DestroyObject((void**)&m_heapRel->attr);
        DestroyObject((void**)&m_heapRel->rel);
        DestroyObject((void**)&m_heapRel);
        m_heapRel = nullptr;
    }
}

SysClassTupDef *DstoreTableHandler::BuildSysRelationTuple(const TableInfo &tableInfo, Oid tablespaceOid,
                                                          PageId segmentId)
{
    /* Step1: Allocates memory for a SysClassTupDef object. */
    SysClassTupDef *relation = static_cast<SysClassTupDef *>(DstorePalloc0(sizeof(SysClassTupDef)));
    StorageAssert(relation != nullptr);

    /* Step2: Generates data for the SysClassTupDef object. */
    errno_t rc = memcpy_s(relation->relname.data, strlen(tableInfo.relation.name), tableInfo.relation.name,
                          strlen(tableInfo.relation.name));
    storage_securec_check(rc, "\0", "\0");
    relation->relnamespace = 11;
    relation->reltablespace = tablespaceOid;
    relation->reltype = DSTORE_INVALID_OID;
    relation->reloftype = DSTORE_INVALID_OID;
    relation->relowner = 10;
    relation->relam = 0;
    relation->reltoastrelid = DSTORE_INVALID_OID;
    relation->reltoastidxid = DSTORE_INVALID_OID;

    relation->reldeltarelid = DSTORE_INVALID_OID;
    relation->reldeltaidx = DSTORE_INVALID_OID;
    relation->relcudescrelid = DSTORE_INVALID_OID;
    relation->relcudescidx = DSTORE_INVALID_OID;
    relation->relhasindex = false;
    relation->relisshared = false;
    relation->relpersistence = tableInfo.relation.persistenceLevel;
    relation->relkind = tableInfo.relation.relKind;
    relation->relnatts = static_cast<int16_t>(tableInfo.relation.attrNum);
    relation->relchecks = 0;
    relation->relhasoids = false;
    relation->relhaspkey = false;
    relation->relhasrules = false;
    relation->relhastriggers = false;
    relation->relhassubclass = false;
    relation->relcmprs = 0;
    relation->relhasclusterkey = false;
    relation->relrowmovement = false;
    relation->parttype = tableInfo.relation.partType;
    relation->relfileid = segmentId.m_fileId;
    relation->relblknum = segmentId.m_blockId;
    relation->rellobfileid = INVALID_VFS_FILE_ID;
    relation->rellobblknum = DSTORE_INVALID_BLOCK_NUMBER;
    return relation;
}

TupleDesc DstoreTableHandler::GenerateHeapTupDesc(Oid relOid, ColumnDesc colDef[], uint8_t colNum)
{
    TupleDesc desc = GenerateTupDescTemplate(colNum);
    for (int i = 0; i < colNum; i++) {
        desc->attrs[i]->attrelid = relOid;
        errno_t rc = memcpy_s(desc->attrs[i]->attname.data, NAME_MAX_LEN, colDef[i].name, NAME_MAX_LEN);
        storage_securec_check(rc, "\0", "\0");
        desc->attrs[i]->atttypid = colDef[i].type;
        desc->attrs[i]->attnum = i;
        desc->attrs[i]->attnotnull = !colDef[i].canBeNull;
        desc->attrs[i]->attlen = colDef[i].len;
        desc->attrs[i]->attbyval = colDef[i].isByVal;
        desc->attrs[i]->attalign = colDef[i].align;
        desc->attrs[i]->attcacheoff = -1;
        desc->attrs[i]->atthasdef = colDef[i].isHaveDefVal;
        desc->attrs[i]->atttypmod = -1;
        desc->attrs[i]->attstorage = colDef[i].storageType;
    }
    return desc;
}

TupleDesc DstoreTableHandler::GenerateTupDescTemplate(int natts)
{
    size_t descSize = GetTupleDescSize(natts);
    size_t attrsSize = GetAttrSize(natts);
    char *offset = (char *)DstorePalloc0(descSize + attrsSize);
    auto desc = (TupleDesc)offset;

    desc->natts = natts;
    desc->tdisredistable = false;
    desc->attrs = (Form_pg_attribute *)(offset + sizeof(TupleDescData));
    desc->initdefvals = nullptr;
    desc->tdtypeid = RECORDOID;
    desc->tdtypmod = -1;
    desc->tdhasoid = false;
    desc->tdrefcount = -1;
    desc->tdhasuids = false;

    offset += descSize;
    for (int i = 0; i < desc->natts; i++) {
        desc->attrs[i] = (Form_pg_attribute)offset;
        offset += MAXALIGN(ATTRIBUTE_FIXED_PART_SIZE);
    }

    return desc;
}

int DstoreTableHandler::Insert(Datum* values, bool* isNulls, uint32_t *colIndex)
{
    AutoMemCxtSwitch autoMemCxtSwitch(thrd->GetTransactionMemoryContext());

    HeapTuple* memTuple = TupleInterface::FormHeapTuple(m_heapRel->attr, values, isNulls, nullptr);
    DSTORE::ItemPointerData ctid;
    RetStatus ret = HeapInterface::Insert(m_heapRel, memTuple, ctid, thrd->GetActiveTransaction()->GetCurCid());
    StorageAssert(ret == DSTORE_SUCC);
    DestroyObject((void**)&memTuple);

    if (m_indexRel == nullptr) {
        return ret;
    }
    StorageAssert(colIndex != nullptr);

    /* Convert heapValues to indexValues */
    uint16_t keyNum = GetIndexKeyNum();
    Datum indexValues[keyNum];
    bool indexIsNulls[keyNum];
    for (uint16_t i = 0; i < keyNum; i++) {
        uint32_t keyIdx = colIndex[i];
        indexValues[i] = values[keyIdx];
        indexIsNulls[i] = isNulls[keyIdx];
    }
    BtreeInsertAndDeleteCommonData btreeContext;
    btreeContext.indexRel = m_indexRel;
    btreeContext.indexInfo = m_indexRel->index;
    btreeContext.skey = ConstructNormalScanKey(keyNum);
    btreeContext.values = indexValues;
    btreeContext.isnull = indexIsNulls;
    btreeContext.heapCtid = &ctid;

    ret = IndexInterface::Insert(btreeContext, false, true);
    StorageAssert(ret == DSTORE_SUCC);
    DestroyObject((void**)&btreeContext.skey);
    TransactionInterface::IncreaseCommandCounter();
    return ret;
}

/* Because TPCC update does not involve the update of index columns,
 * the update operation does not support index update.
 */
int DstoreTableHandler::Update(ItemPointerData* oldCtid, uint32_t *colIndex, Datum* values, bool* isNulls)
{
    HeapTuple *memTuple = TupleInterface::FormHeapTuple(m_heapRel->attr, values, isNulls, nullptr);
    AutoMemCxtSwitch autoMemCxtSwitch(thrd->GetTransactionMemoryContext());
    HeapUpdateContext updateContext;
    updateContext.oldCtid = *oldCtid;
    updateContext.newTuple  = memTuple;
    updateContext.needReturnOldTup = true;
    updateContext.snapshot = *thrd->GetActiveTransaction()->GetSnapshot();
    updateContext.cid = thrd->GetActiveTransaction()->GetCurCid();
    updateContext.executedEpq = true;
    RetStatus ret = HeapInterface::Update(m_heapRel, &updateContext);
    DestroyObject((void**)&memTuple);
    if (unlikely(ret != DSTORE_SUCC)) {
        return ret;
    }
    if (updateContext.oldCtid == updateContext.newCtid) {
        return ret;
    }
    /* Convert heapValues to indexValues */
    uint16_t keyNum = GetIndexKeyNum();
    Datum indexValues[keyNum];
    bool indexIsNulls[keyNum];
    for (int i = 0; i < keyNum; i++) {
        uint32_t keyIdx = colIndex[i];
        indexValues[i] = values[keyIdx];
        indexIsNulls[i] = isNulls[keyIdx];
    }
    BtreeInsertAndDeleteCommonData btreeContext;
    btreeContext.indexRel = m_indexRel;
    btreeContext.indexInfo = m_indexRel->index;
    btreeContext.skey = ConstructNormalScanKey(keyNum);
    btreeContext.values = indexValues;
    btreeContext.isnull = indexIsNulls;
    btreeContext.heapCtid = oldCtid;

    /* Step 2. Do the delete here */
    ret = IndexInterface::Delete(btreeContext);
    if (STORAGE_FUNC_FAIL(ret)) {
        /* Handler ERROR here in case of index insert failure */
        DestroyObject((void**)&btreeContext.skey);
        StorageAssert(0);
        return DSTORE_FAIL;
    }
    btreeContext.heapCtid = &updateContext.newCtid;
    ret = IndexInterface::Insert(btreeContext, false, true);
    StorageAssert(ret == DSTORE_SUCC);
    DestroyObject((void**)&btreeContext.skey);
    TransactionInterface::IncreaseCommandCounter();
    return ret;
}

Datum int4_eq(FunctionCallInfo fcinfo)
{
    int32 arg1 = DatumGetInt32(fcinfo->arg[0]);
    int32 arg2 = DatumGetInt32(fcinfo->arg[1]);
    return arg1 == arg2;
}

int DstoreTableHandler::Scan(__attribute__((__unused__)) uint32_t *colSeq, Datum *indexValues, HeapTuple **tuple,
                             uint32_t indexColNum)
{
    AutoMemCxtSwitch autoMemCxtSwitch(thrd->GetTransactionMemoryContext());
    uint16_t keyNum = GetIndexKeyNum();
    indexColNum = DstoreMin(indexColNum, keyNum);
    DSTORE::Snapshot snapshot = thrd->GetActiveTransaction()->GetSnapshot();

    ItemPointerData pointGetHeapCtid = INVALID_ITEM_POINTER;
    PointGetHeapCtidStatus pointGetStatus = TryPointGetHeapCtid(indexValues, indexColNum, &pointGetHeapCtid);
    if (pointGetStatus == PointGetHeapCtidStatus::MISS || pointGetStatus == PointGetHeapCtidStatus::FAILED) {
        return -1;
    }
    if (pointGetStatus == PointGetHeapCtidStatus::FOUND) {
        HeapScanHandler *heapScan = HeapInterface::CreateHeapScanHandler(m_heapRel);
        if (STORAGE_FUNC_FAIL(HeapInterface::BeginScan(heapScan, snapshot))) {
            HeapInterface::DestroyHeapScanHandler(heapScan);
            return -1;
        }

        *tuple = HeapInterface::FetchTuple(heapScan, pointGetHeapCtid);
        HeapInterface::EndScan(heapScan);
        HeapInterface::DestroyHeapScanHandler(heapScan);
        StorageAssert(*tuple != nullptr);
        if (unlikely(*tuple == nullptr)) {
            return -1;
        }
        return 0;
    }

    IndexScanHandler *indexScan = IndexInterface::ScanBegin(m_indexRel, m_indexRel->index, indexColNum, 0);
    IndexInterface::IndexScanSetSnapshot(indexScan, snapshot);

    ScanKey keyInfos = ConstructEqualScanKey(indexColNum, indexValues);
    __attribute__((__unused__)) RetStatus ret = IndexInterface::ScanRescan(indexScan, keyInfos);
    StorageAssert(ret == DSTORE_SUCC);
    DestroyObject((void**)&keyInfos);
    __attribute__((__unused__)) bool recheck = false;
    bool found;
    (void)IndexInterface::ScanNext(indexScan, ScanDirection::FORWARD_SCAN_DIRECTION, &found, &recheck);
    ItemPointer heapCtid = IndexInterface::GetResultHeapCtid(indexScan);
    if (unlikely(heapCtid == nullptr || *heapCtid == INVALID_ITEM_POINTER)) {
        IndexInterface::ScanEnd(indexScan);
        return -1;
    }

    HeapScanHandler *heapScan = HeapInterface::CreateHeapScanHandler(m_heapRel);
    if (STORAGE_FUNC_FAIL(HeapInterface::BeginScan(heapScan, snapshot))) {
        HeapInterface::DestroyHeapScanHandler(heapScan);
        IndexInterface::ScanEnd(indexScan);
        return -1;
    }

    *tuple = HeapInterface::FetchTuple(heapScan, *heapCtid);
    HeapInterface::EndScan(heapScan);
    HeapInterface::DestroyHeapScanHandler(heapScan);
    IndexInterface::ScanEnd(indexScan);
    StorageAssert(*tuple != nullptr);
    if (unlikely(*tuple == nullptr)) {
        return -1;
    }
    return 0;
}

int DstoreTableHandler::LockTuple(__attribute__((__unused__)) uint32_t* colSeq, Datum* indexValues, HeapTuple** tuple, uint32_t indexColNum)
{
    AutoMemCxtSwitch autoMemCxtSwitch(thrd->GetTransactionMemoryContext());
    uint16_t keyNum = GetIndexKeyNum();
    indexColNum = DstoreMin(indexColNum, keyNum);

    ItemPointerData pointGetHeapCtid = INVALID_ITEM_POINTER;
    PointGetHeapCtidStatus pointGetStatus = TryPointGetHeapCtid(indexValues, indexColNum, &pointGetHeapCtid);
    if (pointGetStatus == PointGetHeapCtidStatus::MISS || pointGetStatus == PointGetHeapCtidStatus::FAILED) {
        return -1;
    }
    if (pointGetStatus == PointGetHeapCtidStatus::FOUND) {
        HeapLockTupleContext lockTupContext;
        lockTupContext.ctid = pointGetHeapCtid;
        lockTupContext.needRetTup = true;
        lockTupContext.executedEpq = true;
        lockTupContext.snapshot = *thrd->GetActiveTransaction()->GetSnapshot();
        RetStatus ret = HeapInterface::LockUnchangedTuple(m_heapRel, &lockTupContext);
        if (ret == DSTORE_SUCC) {
            *tuple = lockTupContext.retTup;
        } else if (lockTupContext.failureInfo.reason == DSTORE::HeapHandlerFailureReason::UPDATED) {
            lockTupContext.ctid = lockTupContext.failureInfo.ctid;
            ret = HeapInterface::LockNewestTuple(m_heapRel, &lockTupContext);
            StorageAssert(ret == DSTORE_SUCC);
            *tuple = lockTupContext.retTup;
            StorageAssert(*tuple != nullptr);
        } else {
            printf("LockTuple errcode:%llu, reason:%d, csn:%lu UniqueQueryId:%lu\n", thrd->GetErrorCode(),
                   static_cast<int>(lockTupContext.failureInfo.reason), thrd->GetSnapShotCsn(),
                   thrd->GetUniqueQueryId());
        }
        TransactionInterface::IncreaseCommandCounter();
        return ret;
    }

    IndexScanHandler *indexScan = IndexInterface::ScanBegin(m_indexRel, m_indexRel->index, indexColNum, 0);
    IndexInterface::IndexScanSetSnapshot(indexScan, thrd->GetActiveTransaction()->GetSnapshot());
    ScanKey keyInfos = ConstructEqualScanKey(indexColNum, indexValues);
    IndexInterface::ScanRescan(indexScan, keyInfos);
    DestroyObject((void**)&keyInfos);

    __attribute__((__unused__)) bool recheck = false;
    bool found;
    (void)IndexInterface::ScanNext(indexScan, ScanDirection::FORWARD_SCAN_DIRECTION, &found, &recheck);
    ItemPointer heapCtid = IndexInterface::GetResultHeapCtid(indexScan);
    if (heapCtid == nullptr || *heapCtid == INVALID_ITEM_POINTER) {
        IndexInterface::ScanEnd(indexScan);
        return -1;
    }

    HeapLockTupleContext lockTupContext;
    lockTupContext.ctid = *heapCtid;
    lockTupContext.needRetTup = true;
    lockTupContext.executedEpq = true;
    lockTupContext.snapshot = *thrd->GetActiveTransaction()->GetSnapshot();
    RetStatus ret = HeapInterface::LockUnchangedTuple(m_heapRel, &lockTupContext);
    if(ret ==  DSTORE_SUCC) {
        *tuple = lockTupContext.retTup;
    } else if (lockTupContext.failureInfo.reason == DSTORE::HeapHandlerFailureReason::UPDATED) {
        lockTupContext.ctid = lockTupContext.failureInfo.ctid;
        ret = HeapInterface::LockNewestTuple(m_heapRel, &lockTupContext);
        StorageAssert(ret == DSTORE_SUCC);
        *tuple = lockTupContext.retTup;
        StorageAssert(*tuple != nullptr);
    } else {
        printf("LockTuple errcode:%llu, reason:%d, csn:%lu UniqueQueryId:%lu\n", thrd->GetErrorCode(),
               static_cast<int>(lockTupContext.failureInfo.reason), thrd->GetSnapShotCsn(), thrd->GetUniqueQueryId());
    }

    IndexInterface::ScanEnd(indexScan);
    TransactionInterface::IncreaseCommandCounter();
    return ret;
}

int DstoreTableHandler::Sum(__attribute__((__unused__)) uint32_t *colSeq, Datum *indexValues, uint32_t heapColSeq,
                            double &sumNum, uint32_t indexColNum)
{
    AutoMemCxtSwitch autoMemCxtSwitch(thrd->GetTransactionMemoryContext());
    uint16_t keyNum = GetIndexKeyNum();
    indexColNum = DstoreMin(indexColNum, keyNum);
    IndexScanHandler *indexScan = IndexInterface::ScanBegin(m_indexRel, m_indexRel->index, indexColNum, 0);
    IndexInterface::IndexScanSetSnapshot(indexScan, thrd->GetActiveTransaction()->GetSnapshot());
    ScanKey keyInfos = ConstructEqualScanKey(indexColNum, indexValues);
    IndexInterface::ScanRescan(indexScan, keyInfos);
    DestroyObject((void**)&keyInfos);

    HeapScanHandler *heapScan = HeapInterface::CreateHeapScanHandler(m_heapRel);
    if (STORAGE_FUNC_FAIL(HeapInterface::BeginScan(heapScan, thrd->GetActiveTransaction()->GetSnapshot()))) {
        HeapInterface::DestroyHeapScanHandler(heapScan);
        IndexInterface::ScanEnd(indexScan);
        return -1;
    }
    HeapTuple *tuple = nullptr;
    bool isNull = false;
    sumNum = 0.0;
    bool found;
    __attribute__((__unused__)) bool recheck = false;
    (void)IndexInterface::ScanNext(indexScan, ScanDirection::FORWARD_SCAN_DIRECTION, &found, &recheck);
    while (found) {
        tuple = HeapInterface::FetchTuple(heapScan, *IndexInterface::GetResultHeapCtid(indexScan));
        StorageAssert(tuple != nullptr);
        sumNum += DatumGetFloat64(TupleInterface::GetHeapAttr(tuple, heapColSeq, m_heapRel->attr, &isNull));
        (void)IndexInterface::ScanNext(indexScan, ScanDirection::FORWARD_SCAN_DIRECTION, &found, &recheck);
    }
    HeapInterface::EndScan(heapScan);
    HeapInterface::DestroyHeapScanHandler(heapScan);

    IndexInterface::ScanEnd(indexScan);
    return 0;
}

int DstoreTableHandler::GetCount(uint32_t &cnt, __attribute__((__unused__)) uint32_t *colSeq, Datum *indexValues,
                                 uint32_t indexColNum)
{
    AutoMemCxtSwitch autoMemCxtSwitch(thrd->GetTransactionMemoryContext());
    cnt = 0;
    uint16_t keyNum = GetIndexKeyNum();
    indexColNum = DstoreMin(indexColNum, keyNum);
    IndexScanHandler *indexScan = IndexInterface::ScanBegin(m_indexRel, m_indexRel->index, indexColNum, 0);
    IndexInterface::IndexScanSetSnapshot(indexScan, thrd->GetActiveTransaction()->GetSnapshot());
    ScanKey keyInfos = ConstructEqualScanKey(indexColNum, indexValues);
    IndexInterface::ScanRescan(indexScan, keyInfos);
    DestroyObject((void**)&keyInfos);

    __attribute__((__unused__)) bool recheck = false;
    bool found;
    while (STORAGE_FUNC_SUCC(IndexInterface::ScanNext(indexScan, ScanDirection::FORWARD_SCAN_DIRECTION,
                                                      &found, &recheck)) && found) {
        cnt++;
    }
    IndexInterface::ScanEnd(indexScan);
    return 0;
}

int DstoreTableHandler::GetMin(__attribute__((__unused__)) uint32_t *colSeq, Datum *indexValues, HeapTuple **tuple,
                               uint32_t minColSeq, uint32_t indexColNum)
{
    AutoMemCxtSwitch autoMemCxtSwitch(thrd->GetTransactionMemoryContext());
    uint16_t keyNum = GetIndexKeyNum();
    indexColNum = DstoreMin(indexColNum, keyNum);

    IndexScanHandler *indexScan = IndexInterface::ScanBegin(m_indexRel, m_indexRel->index, indexColNum, 0);
    IndexInterface::IndexScanSetSnapshot(indexScan, thrd->GetActiveTransaction()->GetSnapshot());
    IndexInterface::ScanSetWantItup(indexScan, true);
    ScanKey keyInfos = ConstructEqualScanKey(indexColNum, indexValues);
    IndexInterface::ScanRescan(indexScan, keyInfos);
    DestroyObject((void**)&keyInfos);

    bool isNull = false;
    ItemPointerData extremeCtid = INVALID_ITEM_POINTER;
    uint32_t extremeValue = -1;
    __attribute__((__unused__)) bool recheck = false;
    IndexTuple *indexTuple = nullptr;
    do {
        indexTuple = IndexInterface::OnlyScanNext(indexScan, ScanDirection::FORWARD_SCAN_DIRECTION,
                                                  &m_indexRel->attr, &recheck);
        if (indexTuple == nullptr) {
            break;
        }

        if (extremeValue > TupleInterface::GetIndexAttr(indexTuple, minColSeq + 1, m_indexRel->attr, &isNull)) {
            extremeValue = TupleInterface::GetIndexAttr(indexTuple, minColSeq + 1, m_indexRel->attr, &isNull);
            extremeCtid = indexTuple->GetHeapCtid();
        }
    } while (true);

    ItemPointerData pre_extremeCtid = extremeCtid;

    if (extremeCtid != INVALID_ITEM_POINTER) {
        HeapScanHandler *heapScan = HeapInterface::CreateHeapScanHandler(m_heapRel);
        if (STORAGE_FUNC_FAIL(HeapInterface::BeginScan(heapScan,
            thrd->GetActiveTransaction()->GetSnapshot()))) {
            HeapInterface::DestroyHeapScanHandler(heapScan);
            IndexInterface::ScanEnd(indexScan);
            return -1;
        }
        *tuple = HeapInterface::FetchTuple(heapScan, extremeCtid);
        StorageAssert(*tuple != nullptr);
        HeapInterface::EndScan(heapScan);
        HeapInterface::DestroyHeapScanHandler(heapScan);
    }

    IndexInterface::ScanEnd(indexScan);;
    return pre_extremeCtid == INVALID_ITEM_POINTER ? -1 : 0;
}

int DstoreTableHandler::GetMax(__attribute__((__unused__)) uint32_t *colSeq, Datum *indexValues, HeapTuple **tuple,
                               uint32_t maxColSeq, uint32_t indexColNum)
{
    AutoMemCxtSwitch autoMemCxtSwitch(thrd->GetTransactionMemoryContext());
    uint16_t keyNum = GetIndexKeyNum();
    indexColNum = DstoreMin(indexColNum, keyNum);

    IndexScanHandler *indexScan = IndexInterface::ScanBegin(m_indexRel, m_indexRel->index, indexColNum, 0);
    IndexInterface::IndexScanSetSnapshot(indexScan, thrd->GetActiveTransaction()->GetSnapshot());
    IndexInterface::ScanSetWantItup(indexScan, true);
    ScanKey keyInfos = ConstructEqualScanKey(indexColNum, indexValues);
    IndexInterface::ScanRescan(indexScan, keyInfos);
    DestroyObject((void**)&keyInfos);

    bool isNull = false;
    ItemPointerData extremeCtid = INVALID_ITEM_POINTER;
    uint32_t extremeValue = 0;
    __attribute__((__unused__)) bool recheck = false;
    IndexTuple *indexTuple = nullptr;
    do {
        indexTuple = IndexInterface::OnlyScanNext(indexScan, ScanDirection::FORWARD_SCAN_DIRECTION,
                                                  &m_indexRel->attr, &recheck);
        if (indexTuple == nullptr) {
            break;
        }
        if (extremeValue < TupleInterface::GetIndexAttr(indexTuple, maxColSeq + 1, m_indexRel->attr, &isNull)) {
            extremeValue = TupleInterface::GetIndexAttr(indexTuple, maxColSeq + 1, m_indexRel->attr, &isNull);
            extremeCtid = indexTuple->GetHeapCtid();
        }
    } while (true);
    StorageAssert(extremeCtid != INVALID_ITEM_POINTER);

    HeapScanHandler *heapScan = HeapInterface::CreateHeapScanHandler(m_heapRel);
    if (STORAGE_FUNC_FAIL(HeapInterface::BeginScan(heapScan, thrd->GetActiveTransaction()->GetSnapshot()))) {
        HeapInterface::DestroyHeapScanHandler(heapScan);
        IndexInterface::ScanEnd(indexScan);
        return -1;
    }
    *tuple = HeapInterface::FetchTuple(heapScan, extremeCtid);
    StorageAssert(*tuple != nullptr);
    HeapInterface::EndScan(heapScan);
    HeapInterface::DestroyHeapScanHandler(heapScan);

    IndexInterface::ScanEnd(indexScan);
    return 0;
}

int DstoreTableHandler::Delete(__attribute__((__unused__)) uint32_t *colSeq, Datum *indexValues, uint32_t indexColNum)
{
    AutoMemCxtSwitch autoMemCxtSwitch(thrd->GetTransactionMemoryContext());
    uint16_t keyNum = GetIndexKeyNum();
    indexColNum = DstoreMin(indexColNum, keyNum);

    ItemPointerData pointGetHeapCtid = INVALID_ITEM_POINTER;
    PointGetHeapCtidStatus pointGetStatus = TryPointGetHeapCtid(indexValues, indexColNum, &pointGetHeapCtid);
    if (pointGetStatus == PointGetHeapCtidStatus::MISS || pointGetStatus == PointGetHeapCtidStatus::FAILED) {
        return -1;
    }
    if (pointGetStatus == PointGetHeapCtidStatus::FOUND) {
        HeapDeleteContext deleteContext;
        deleteContext.ctid = pointGetHeapCtid;
        deleteContext.needReturnTup = false;
        deleteContext.snapshot = *thrd->GetActiveTransaction()->GetSnapshot();
        deleteContext.cid = thrd->GetActiveTransaction()->GetCurCid();
        deleteContext.executedEpq = true;
        RetStatus retVal = HeapInterface::Delete(m_heapRel, &deleteContext);
        if (retVal == DSTORE_FAIL) {
            return -1;
        }

        bool deleteIndexNulls[keyNum];
        for (uint16_t i = 0; i < keyNum; i++) {
            deleteIndexNulls[i] = false;
        }

        BtreeInsertAndDeleteCommonData btreeContext;
        btreeContext.indexRel = m_indexRel;
        btreeContext.indexInfo = m_indexRel->index;
        btreeContext.skey = ConstructNormalScanKey(keyNum);
        btreeContext.values = indexValues;
        btreeContext.isnull = deleteIndexNulls;
        btreeContext.heapCtid = &pointGetHeapCtid;
        retVal = IndexInterface::Delete(btreeContext);

        StorageAssert(retVal == 0);
        DestroyObject((void**)&btreeContext.skey);
        TransactionInterface::IncreaseCommandCounter();
        return retVal;
    }

    IndexScanHandler *indexScan = IndexInterface::ScanBegin(m_indexRel, m_indexRel->index, indexColNum, 0);
    IndexInterface::IndexScanSetSnapshot(indexScan, thrd->GetActiveTransaction()->GetSnapshot());
    IndexInterface::ScanSetWantItup(indexScan, true);
    auto keyInfos = ConstructEqualScanKey(keyNum, indexValues);
    IndexInterface::ScanRescan(indexScan, keyInfos);
    DestroyObject((void**)&keyInfos);

    __attribute__((__unused__)) bool recheck = false;
    IndexTuple *indexTuple =
        IndexInterface::OnlyScanNext(indexScan, ScanDirection::FORWARD_SCAN_DIRECTION, &m_indexRel->attr, &recheck);
    StorageAssert(indexTuple != nullptr);
    ItemPointerData heapCtid = indexTuple->GetHeapCtid();

    HeapDeleteContext deleteContext;
    deleteContext.ctid = heapCtid;
    deleteContext.needReturnTup = false;
    deleteContext.snapshot = *thrd->GetActiveTransaction()->GetSnapshot();
    deleteContext.cid = thrd->GetActiveTransaction()->GetCurCid();
    deleteContext.executedEpq = true;
    RetStatus retVal = HeapInterface::Delete(m_heapRel, &deleteContext);
    if (retVal == DSTORE_FAIL) {
        IndexInterface::ScanEnd(indexScan);
        return -1;
    }

    /* Index delete */
    Datum deleteIndexValues[keyNum];
    bool deleteIndexNulls[keyNum];
    TupleInterface::DeformIndexTuple(indexTuple, m_indexRel->attr, deleteIndexValues, deleteIndexNulls);

    BtreeInsertAndDeleteCommonData btreeContext;
    btreeContext.indexRel = m_indexRel;
    btreeContext.indexInfo = m_indexRel->index;
    btreeContext.skey = ConstructNormalScanKey(keyNum);
    btreeContext.values = deleteIndexValues;
    btreeContext.isnull = deleteIndexNulls;
    btreeContext.heapCtid = &heapCtid;
    retVal = IndexInterface::Delete(btreeContext);

    StorageAssert(retVal == 0);
    IndexInterface::ScanEnd(indexScan);
    DestroyObject((void**)&btreeContext.skey);
    TransactionInterface::IncreaseCommandCounter();
    return retVal;
}

int DstoreTableHandler::CreateIndex(const TableInfo& indexTableInfo)
{
    StorageAssert(m_heapRel != nullptr);
    StorageAssert(m_tableName != nullptr);
    char *indexName = indexTableInfo.relation.name;
    if(indexName == nullptr || strlen(indexName) == 0) {
        indexName = TableDataGenerator::GenerateIndexName(m_tableName, 
                                      indexTableInfo.indexDesc->indexCol, 
                                      indexTableInfo.indexDesc->indexAttrNum);
    }
    StorageRelation indexRel = simulator->GetIndexRelationEntry(indexName);
    if (indexRel != nullptr) {
        DestroyObject((void**)&indexName);
        return 1; /* The index table already exists */
    }
    if (indexTableInfo.relation.name == nullptr) {
        DestroyObject((void**)&indexName);
    }
    /* create index */
    IndexBuildInfo buildInfo = CreateIndexMeta(indexTableInfo);
    StorageAssert(m_indexRel != nullptr);
    ScanKey keyInfos = ConstructNormalScanKey(indexTableInfo.indexDesc->indexAttrNum);
    RetStatus ret = IndexInterface::Build(m_indexRel, keyInfos, &buildInfo);
    StorageAssert(ret == DSTORE_SUCC);
    DestroyObject((void**)&keyInfos);
    return ret;
}

IndexBuildInfo DstoreTableHandler::CreateIndexMeta(const TableInfo& indexTableInfo)
{
    PageId segmentId = TableSpace_Interface::AllocSegment(g_defaultPdbId, static_cast<TablespaceId>(TBS_ID::DEFAULT_TABLE_SPACE_ID),
        SegmentType::INDEX_SEGMENT_TYPE);

    uint8_t indexAttrNum = indexTableInfo.indexDesc->indexAttrNum;
    StorageAssert(m_indexRel == nullptr);
    Oid relOid = simulator->GetNextOid();
    /* Build index tuple descriptor from heap relation */
    TupleDesc attr = GenerateIndexTupDesc(m_heapRel, relOid, indexAttrNum, indexTableInfo.indexDesc->indexCol);

    /* The m_indexRel and m_heapRel use the same tablespace. */
    TablespaceId tabelSpaceId = static_cast<TablespaceId>(TBS_ID::DEFAULT_TABLE_SPACE_ID);
    SysClassTupDef *classTuple =
        BuildSysRelationTuple(indexTableInfo, static_cast<Oid>(tabelSpaceId), segmentId);
    classTuple->relnatts = indexAttrNum;
    m_indexRel = static_cast<StorageRelation>(DstorePalloc(sizeof(StorageRelationData)));
    __attribute__((__unused__)) RetStatus ret =
        m_indexRel->Construct(g_defaultPdbId, relOid, classTuple, attr, INVALID_INDEX_FILLFACTOR, tabelSpaceId);
    StorageAssert(ret == DSTORE_SUCC);
    m_indexRel->rel = classTuple;
    m_indexRel->index = BuildIndexInfo(m_indexRel, indexAttrNum, indexTableInfo.indexDesc->isUnique);

    m_indexRel->indexInfo = static_cast<SysIndexTupDef *>(DstorePalloc(sizeof(SysIndexTupDef)));
    m_indexRel->indexInfo->indnatts = indexAttrNum;
    m_indexRel->indexInfo->indexrelid = relOid;
    m_indexRel->indexInfo->indrelid = m_heapRel->relOid;
    m_indexRel->indexInfo->indisunique = indexTableInfo.indexDesc->isUnique;

    /* add relation into relcache */
    char *indexName = TableDataGenerator::GenerateIndexName(m_tableName, indexTableInfo.indexDesc->indexCol, indexAttrNum);
    errno_t rc = memcpy_s(m_indexRel->rel->relname.data, strlen(indexName) + 1, indexName, strlen(indexName) + 1);
    storage_securec_check(rc, "\0", "\0");

    simulator->InsertIndexTable(indexName, m_indexRel);
    FlushTable(indexName, m_indexRel);
    DestroyObject((void**)&indexName);

    /* Build btree buildInfo */
    IndexBuildInfo buildInfo = ConstructBtreeBuilder(m_heapRel, indexTableInfo);
    buildInfo.baseInfo = *m_indexRel->index;
    return buildInfo;
}

TupleDesc DstoreTableHandler::GenerateIndexTupDesc(StorageRelation heapRel, Oid indexRelOid, int numKeyAttrs,
                                                   const uint32_t *colIndex)
{
    StorageAssert(heapRel != nullptr);
    TupleDesc heapTupDesc = heapRel->attr;
    TupleDesc indexTupDesc = GenerateTupDescTemplate(numKeyAttrs);
    StorageAssert(indexTupDesc != nullptr);

    for (int i = 0; i < numKeyAttrs; i++) {
        errno_t rc = memcpy_s(indexTupDesc->attrs[i], sizeof(SysAttributeTupDef), heapTupDesc->attrs[colIndex[i]],
                              sizeof(SysAttributeTupDef));
        storage_securec_check(rc, "\0", "\0");

        indexTupDesc->attrs[i]->attrelid = indexRelOid;
        indexTupDesc->attrs[i]->attnum = i + 1;
    }
    return indexTupDesc;
}

IndexBuildInfo DstoreTableHandler::ConstructBtreeBuilder(StorageRelation heapRel, const TableInfo &indexTableInfo)
{
    /* generate IndexBuildInfo */
    IndexBuildInfo buildInfo;
    buildInfo.heapRelationOid = heapRel->relOid;
    buildInfo.heapAttributes = heapRel->attr;
    buildInfo.heapRels = static_cast<StorageRelation *>(DstorePalloc(sizeof(StorageRelation)));
    buildInfo.heapRels[0] = heapRel;
    buildInfo.heapRels[0]->tableSmgr = heapRel->tableSmgr;
    buildInfo.heapRelNum = 1;
    buildInfo.allPartOids = nullptr;
    buildInfo.allPartTuples = nullptr;
    buildInfo.heapTuples = 0;
    buildInfo.indexTuples = 0;

    for (uint8_t i = 0; i < indexTableInfo.indexDesc->indexAttrNum; i++) {
        /* get attribute number of key */
        buildInfo.indexAttrOffset[i] = (AttrNumber)indexTableInfo.indexDesc->indexCol[i] + 1;
    }
    return buildInfo;
}
IndexInfo *DstoreTableHandler::CreateIndexInfo(int attrNum)
{
    Size idxInfoSize = MAXALIGN(sizeof(IndexInfo));
    Size optionSize = MAXALIGN(sizeof(int16_t) * static_cast<uint32_t>(attrNum));
    Size opcintypeSize = MAXALIGN(sizeof(Oid) * static_cast<uint32_t>(attrNum));
    Size totalSize = idxInfoSize + optionSize + opcintypeSize;
    char *index = (char *)DstorePalloc0(totalSize);
    if (unlikely(index == nullptr)) {
        return nullptr;
    }
    IndexInfo *indexInfo = static_cast<IndexInfo *>(static_cast<void *>(index));
    indexInfo->opcinType = static_cast<Oid *>(static_cast<void *>(index + idxInfoSize));
    indexInfo->indexOption = static_cast<int16_t *>(static_cast<void *>(index + idxInfoSize + opcintypeSize));

    return indexInfo;
}

IndexInfo *DstoreTableHandler::BuildIndexInfo(StorageRelation idxRel, uint16_t keyNum, bool isUnique)
{
    int colNum = idxRel->attr->natts;
    IndexInfo *indexInfo = CreateIndexInfo(colNum);
    __attribute__((__unused__)) errno_t rc =
        memcpy_s(indexInfo->indexRelName, NAMEDATA_LEN, idxRel->rel->relname.data, NAMEDATA_LEN);
    storage_securec_check(rc, "\0", "\0");
    indexInfo->relKind = idxRel->rel->relkind;
    indexInfo->isUnique = isUnique;
    indexInfo->indexAttrsNum = static_cast<uint16_t>(colNum);
    indexInfo->indexKeyAttrsNum = keyNum;
    indexInfo->attributes = idxRel->attr;
    for (uint16_t i = 0; i < colNum; i++) {
        /* test INDOPTION_NULLS_FIRST and INDOPTION_DESC */
        indexInfo->indexOption[i] = 0;
    }

    for (uint16_t i = 0; i < keyNum; i++) {
        indexInfo->opcinType[i] = idxRel->attr->attrs[i]->atttypid;
    }
    return indexInfo;
}

ScanKey DstoreTableHandler::ConstructEqualScanKey(uint32_t indexColNum, Datum *indexValues)
{
    ScanKey keyInfos = (ScanKey)DstorePalloc0(MAXALIGN(sizeof(ScanKeyData)) * indexColNum);
    for (uint32_t i = 0; i < indexColNum; i++) {
        /* get attribute number of key */
        /* get scanKey func by attribute type */
        GetScanFuncByType(&keyInfos[i], m_indexRel->attr->attrs[i]->atttypid, m_indexRel->attr->attrs[i]->atttypid);
        keyInfos[i].skFunc.fnAddr = Int4Eq;
        keyInfos[i].skFunc.fnOid = 65;
        keyInfos[i].skStrategy  = SCAN_ORDER_EQUAL;
        keyInfos[i].skArgument  = indexValues[i];
        keyInfos[i].skAttno     = static_cast<AttrNumber>(i + 1);
    }
    return keyInfos;
}

bool DstoreTableHandler::CanUsePointGet(uint32_t indexColNum) const
{
    return m_indexRel != nullptr && m_indexRel->index != nullptr && m_indexRel->index->isUnique &&
           m_indexRel->index->indexKeyAttrsNum == 1 && indexColNum == 1;
}

void DstoreTableHandler::InitPointGetScanKey(ScanKeyData &scanKey, uint32_t indexColNum, Datum *indexValues)
{
    StorageAssert(indexColNum == 1);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(m_indexRel->attr->attrs[0]->atttypid, indexValues[0],
                                                          SCAN_ORDER_EQUAL, &scanKey, 1);
}

DstoreTableHandler::PointGetHeapCtidStatus DstoreTableHandler::TryPointGetHeapCtid(Datum *indexValues,
                                                                                   uint32_t indexColNum,
                                                                                   ItemPointerData *heapCtid)
{
    if (heapCtid == nullptr || indexValues == nullptr ||
        !g_storageInstance->GetGuc()->IsPointGetFastPathEnabled() || !CanUsePointGet(indexColNum)) {
        return PointGetHeapCtidStatus::FALLBACK;
    }

    ScanKeyData keyInfo = {};
    InitPointGetScanKey(keyInfo, indexColNum, indexValues);
    *heapCtid = INVALID_ITEM_POINTER;
    DSTORE::Snapshot snapshot = thrd->GetActiveTransaction()->GetSnapshot();
    StorageClearError();
    if (STORAGE_FUNC_FAIL(IndexInterface::PointGetUnique(m_indexRel, m_indexRel->index, &keyInfo, snapshot,
                                                         heapCtid))) {
        ErrorCode err = StorageGetErrorCode();
        if (err == INDEX_ERROR_POINTGET_UNSUPPORTED) {
            StorageClearError();
            return PointGetHeapCtidStatus::FALLBACK;
        }
        return PointGetHeapCtidStatus::FAILED;
    }

    return *heapCtid == INVALID_ITEM_POINTER ? PointGetHeapCtidStatus::MISS : PointGetHeapCtidStatus::FOUND;
}

ScanKey DstoreTableHandler::ConstructNormalScanKey(uint32_t indexColNum)
{
    ScanKey keyInfos = (ScanKey)DstorePalloc0(MAXALIGN(sizeof(ScanKeyData)) * indexColNum);
    for (uint32_t i = 0; i < indexColNum; i++) {
        /* get attribute number of key */
        /* get scanKey func by attribute type */
        GetScanFuncByType(&keyInfos[i], m_indexRel->attr->attrs[i]->atttypid, m_indexRel->attr->attrs[i]->atttypid);
        keyInfos[i].skAttno = static_cast<AttrNumber>(i + 1);
    }
    return keyInfos;
}

void DstoreTableHandler::GetScanFuncByType(ScanKey scanKey, Oid leftType, Oid rightType)
{
    FuncCache cache = g_storageInstance->GetCacheHashMgr()->GetFuncCacheFromArgType(leftType, rightType, MAINTAIN_ORDER);
    GetScanKeyByTypeKeyCache(scanKey, (Pointer)&cache);
}

void DstoreTableHandler::GetScanKeyByTypeKeyCache(ScanKey scanKey, __attribute__((__unused__)) char *cachePointer)
{
    auto *typeKeyCache = (FuncCache *)cachePointer;
    scanKey->skFunc.fnAddr = typeKeyCache->fnAddr;
    scanKey->skFunc.fnOid = typeKeyCache->fnOid;
    scanKey->skFunc.fnNargs = 2;
    scanKey->skFunc.fnStrict = true;
    scanKey->skFunc.fnRetset = false;
    scanKey->skFunc.fnMcxt = g_dstoreCurrentMemoryContext;
    scanKey->skFlags = SCAN_KEY_ROW_MEMBER;
    scanKey->skCollation = DEFAULT_COLLATION_OID;
}
void DstoreTableHandler::DestroyRelation(StorageRelation relation)
{
    if(relation == nullptr) {
        return;
    }
    relation->Destroy();
    DestroyObject((void**)&relation->attr);
    DestroyObject((void**)&relation->rel);
    DestroyObject((void**)&relation->index);
    DestroyObject((void**)&relation->indexInfo);
    DestroyObject((void**)&relation);
    relation = nullptr;
}

void DstoreTableHandler::OpenDiskTableFile(CommonPersistentHandler *tablePersistentHander, const char *tableName,
                                           bool isIndex, uint32_t colNum)
{
    char *path = (char *)DstorePalloc0(VFS_FILE_PATH_MAX_LEN);
    StorageAssert(path != nullptr);

    errno_t rc = sprintf_s(path, VFS_FILE_PATH_MAX_LEN, "%s/%s/",
                           (dynamic_cast<StorageInstance *>(m_instance))->GetGuc()->dataDir, TABLE_METADATA_DIR);
    storage_securec_check_ss(rc);

    if (access(path, F_OK) != 0) {
        __attribute__((__unused__)) int ret = mkdir(path, 0777);
        StorageAssert(ret == 0);
    }

    rc = strcat_s(path, VFS_FILE_PATH_MAX_LEN, tableName);
    storage_securec_check(rc, "\0", "\0");
    bool isExist = tablePersistentHander->IsExist(path);
    if (!isExist) {
        size_t totalSize = GetPersistenceTableSize(colNum, isIndex);
        char *relPointer = (char *)DstorePalloc0(totalSize);
        StorageAssert(relPointer != nullptr);
        tablePersistentHander->Create(path, relPointer, totalSize);
    } else {
        tablePersistentHander->Open(path);
    }

    /* Storage space partitioned in disks */
    char *relationObj = static_cast<char *>(tablePersistentHander->GetObject());
    StorageAssert(relationObj != nullptr);
    DiskRelation diskRel = static_cast<DiskRelation>(static_cast<void*>(relationObj));
    AlignDiskTable(relationObj, diskRel, colNum, isIndex);
    if (!isExist) {
        diskRel->Init(isIndex);
    }
    DstorePfree(path);
}
StorageRelation DstoreTableHandler::ReadDiskTable(CommonPersistentHandler *tablePersistentHander, const char *path)
{
    tablePersistentHander->Open(path);
    char *relationObj = static_cast<char *>(tablePersistentHander->GetObject());
    DiskRelation diskRel = static_cast<DiskRelation>(static_cast<void *>(relationObj));
    StorageAssert(diskRel != nullptr);
    bool isIndex = diskRel->isIndex;
    uint32_t colNum = diskRel->colNum;

    /* Constructing StorageRelation */
    StorageRelation relation = static_cast<StorageRelation>(DstorePalloc0(sizeof(StorageRelationData)));
    /* Copy relation info */
    relation->relOid = diskRel->relation.relOid;

    /* Constructing pg_class */
    relation->rel = (SysClassTupDef *)DstorePalloc0(GetPgClassRelSize());
    StorageAssert(relation->rel != nullptr);
    
    /* Constructing TupleDesc */
    size_t descSize = GetTupleDescSize(colNum);
    size_t attrsSize = GetAttrSize(colNum);
    char *attrOffset = (char *)DstorePalloc0(descSize + attrsSize);
    relation->attr = (TupleDesc)attrOffset;
    
    /* Copy pg_class info */
    size_t headerSize = sizeof(FakeRelationMetaData);
    char *offset = relationObj + headerSize;
    errno_t rc = memcpy_s(relation->rel, sizeof(SysClassTupDef), offset, sizeof(SysClassTupDef));
    storage_securec_check(rc, "\0", "\0");

    /* Copy TupleDesc info */
    offset += GetPgClassRelSize();
    rc = memcpy_s(relation->attr, sizeof(TupleDescData), offset, sizeof(TupleDescData));
    storage_securec_check(rc, "\0", "\0");
    relation->attr->attrs = static_cast<Form_pg_attribute *>(static_cast<void *>(attrOffset + sizeof(TupleDescData)));
    rc = memcpy_s(relation->attr->attrs, sizeof(Form_pg_attribute *), offset + sizeof(TupleDescData),
                  sizeof(Form_pg_attribute *));
    storage_securec_check(rc, "\0", "\0");

    offset += descSize;
    attrOffset += descSize;
    for (uint32_t i = 0; i < colNum; i++) {
        size_t size = MAXALIGN(ATTRIBUTE_FIXED_PART_SIZE);
        relation->attr->attrs[i] = static_cast<SysAttributeTupDef *>(static_cast<void *>(attrOffset));
        rc = memcpy_s(relation->attr->attrs[i], size, offset, size);
        storage_securec_check(rc, "\0", "\0");

        offset += size;
        attrOffset += size;
    }
    if (isIndex) {
        /* Copy indexOption info */
        relation->indexInfo = (SysIndexTupDef *)DstorePalloc0(GetPgIndexSize());
        rc = memcpy_s(relation->indexInfo, sizeof(SysIndexTupDef), offset, sizeof(SysIndexTupDef));
        storage_securec_check(rc, "\0", "\0");

        relation->index = BuildIndexInfo(relation, diskRel->keyNum, diskRel->isUnique);
    } else {
        relation->indexInfo = nullptr;
        relation->index = nullptr;
    }
    return relation;
}

void DstoreTableHandler::AlignDiskTable(char *relPointer, DiskRelation diskRel, uint32_t colNum, bool isIndex)
{
    size_t headerSize = sizeof(FakeRelationMetaData);
    char *offset = relPointer + headerSize;
    diskRel->relation.rel = static_cast<SysClassTupDef *>(static_cast<void*>(offset));
    offset += GetPgClassRelSize();
    diskRel->relation.attr = static_cast<TupleDesc>(static_cast<void *>(offset));
    diskRel->relation.attr->attrs =
        static_cast<SysAttributeTupDef **>(static_cast<void *>(offset + sizeof(TupleDescData)));

    offset += GetTupleDescSize(colNum);
    for (uint32_t i = 0; i < colNum; i++) {
        diskRel->relation.attr->attrs[i] = static_cast<SysAttributeTupDef *>(static_cast<void*>(offset));
        offset += MAXALIGN(static_cast<uint32_t>(sizeof(SysAttributeTupDef)));
    }
    if (isIndex) {
        diskRel->relation.indexInfo = static_cast<SysIndexTupDef *>(static_cast<void *>(offset));
    } else { 
        diskRel->relation.indexInfo = nullptr;
    }
    diskRel->relation.index = nullptr;
}

void DstoreTableHandler::FlushTable(const char *tableName, StorageRelation relation)
{
    uint32_t colNum = relation->attr->natts;
    bool isIndexRel = (relation->rel->relkind == SYS_RELKIND_INDEX);
    CommonPersistentHandler tablePersistentHandler;
    OpenDiskTableFile(&tablePersistentHandler, tableName, isIndexRel, colNum);
    DiskRelation diskRel = static_cast<DiskRelation>(tablePersistentHandler.GetObject());
    StorageAssert(diskRel != nullptr);
    diskRel->Init(colNum, relation);
    
    tablePersistentHandler.Sync();
    tablePersistentHandler.Close();
}
int DstoreTableHandler::CheckTable(StorageRelation relation, const TableInfo &tableInfo)
{
    int colNum = tableInfo.relation.attrNum;
    StorageAssert(colNum == relation->attr->natts);

    if(tableInfo.relation.relOid != DSTORE_INVALID_OID) {
        StorageAssert(relation->relOid == tableInfo.relation.relOid);
    }
    StorageAssert(relation->rel->relpersistence == tableInfo.relation.persistenceLevel);
    StorageAssert(relation->rel != nullptr);
    StorageAssert(relation->rel->relkind == tableInfo.relation.relKind);
    StorageAssert(relation->rel->parttype == tableInfo.relation.partType);
    bool isIndex = relation->rel->relkind == SYS_RELKIND_INDEX;
    if (isIndex) {
        StorageAssert(relation->index != nullptr);
        StorageAssert(relation->indexInfo != nullptr);
        StorageAssert(relation->indexInfo->indnatts == colNum);
    } else {
        StorageAssert(relation->index == nullptr);
    }
    StorageAssert(relation->attr != nullptr);
    StorageAssert(relation->attr->natts == colNum);
    StorageAssert(!relation->attr->tdisredistable);
    StorageAssert(relation->attr->initdefvals == nullptr);
    StorageAssert(relation->attr->tdtypeid == RECORDOID);
    StorageAssert(relation->attr->tdtypmod == -1);
    StorageAssert(!relation->attr->tdhasoid);
    StorageAssert(relation->attr->tdrefcount == -1);
    StorageAssert(!relation->attr->tdhasuids);

    for(int i = 0; i < colNum; ++i) {
        StorageAssert(relation->attr->attrs[i] != nullptr);
        StorageAssert(relation->attr->attrs[i]->attrelid == relation->relOid);
        StorageAssert(relation->attr->attrs[i]->atttypid == tableInfo.colDesc[i].type);
        if (strncmp(relation->attr->attrs[i]->attname.data, tableInfo.colDesc[i].name, NAME_MAX_LEN) != 0) {
            StorageAssert(false);
        }
        StorageAssert(relation->attr->attrs[i]->attnum == isIndex ? i + 1 : i);
        StorageAssert(relation->attr->attrs[i]->attlen == tableInfo.colDesc[i].len);
        StorageAssert(relation->attr->attrs[i]->attnotnull == !tableInfo.colDesc[i].canBeNull);
        StorageAssert(relation->attr->attrs[i]->attbyval == tableInfo.colDesc[i].isByVal);
        StorageAssert(relation->attr->attrs[i]->attalign == tableInfo.colDesc[i].align);
        StorageAssert(relation->attr->attrs[i]->atthasdef == tableInfo.colDesc[i].isHaveDefVal);
        StorageAssert(relation->attr->attrs[i]->attstorage == tableInfo.colDesc[i].storageType);
        StorageAssert(relation->attr->attrs[i]->attcacheoff == -1);
        StorageAssert(relation->attr->attrs[i]->atttypmod == -1);
    }
    return 0;
}

size_t DstoreTableHandler::GetPgClassRelSize() const
{
    return sizeof(SysClassTupDef);
}
size_t DstoreTableHandler::GetTupleDescSize(uint32_t colNum) const
{
    return MAXALIGN((uint32_t)(sizeof(TupleDescData) + colNum * sizeof(SysAttributeTupDef *)));
}
size_t DstoreTableHandler::GetAttrSize(uint32_t colNum) const
{
    return colNum * static_cast<uint32_t>(MAXALIGN(ATTRIBUTE_FIXED_PART_SIZE));
}
size_t DstoreTableHandler::GetIndexInfoSize(int attrNum) const
{
    Size idxInfoSize = MAXALIGN(sizeof(IndexInfo));
    Size optionSize = MAXALIGN(sizeof(int16_t) * static_cast<uint32_t>(attrNum));
    Size opcintypeSize = MAXALIGN(sizeof(Oid) * static_cast<uint32_t>(attrNum));
    Size indexInfoSize = idxInfoSize + optionSize + opcintypeSize;
    return indexInfoSize;
}

size_t DstoreTableHandler::GetPgIndexSize() const
{
    return sizeof(SysIndexTupDef);
}

size_t DstoreTableHandler::GetPersistenceTableSize(int colNum, bool isIndex) const
{
    size_t headerSize = sizeof(FakeRelationMetaData);
    size_t indexSize = 0;
    if (isIndex) {
        indexSize = GetPgIndexSize();
    }
    size_t totalSize = headerSize + GetPgClassRelSize() + GetTupleDescSize(colNum) + GetAttrSize(colNum) + indexSize;
    return totalSize;
}

void FakeRelationMetaData::Init(bool isIndex)
{
    colNum = 0;
    isIndex = isIndex;
    keyNum = 0;
    isUnique = false;
    relation.relOid = DSTORE_INVALID_OID;
    relation.attr->natts = 0;
    relation.tableSmgr = nullptr;
    relation.btreeSmgr = nullptr;

    for (uint32_t i = 0; i < colNum; i++) {
        relation.attr->attrs[i]->attrelid = DSTORE_INVALID_OID;
        errno_t rc = memset_s(relation.attr->attrs[i]->attname.data, NAME_MAX_LEN, 0, NAME_MAX_LEN);
        storage_securec_check(rc, "\0", "\0");
        relation.attr->attrs[i]->atttypid = DSTORE_INVALID_OID;
    }
    if (!isIndex) {
        relation.indexInfo = nullptr;
    }
    relation.index = nullptr;
}

void FakeRelationMetaData::Init(uint32_t attrNum, const StorageRelation rel)
{
    colNum = attrNum;
    isIndex = rel->rel->relkind == SYS_RELKIND_INDEX;
    relation.relOid = rel->relOid;
    *relation.rel = *rel->rel;
    if (isIndex) {
        *relation.indexInfo = *rel->indexInfo;
        keyNum = rel->index->indexKeyAttrsNum;
        isUnique =  rel->index->isUnique;
    }
    relation.index = nullptr;

    relation.attr->natts = rel->attr->natts;
    relation.attr->tdisredistable = rel->attr->tdisredistable;
    relation.attr->tdtypeid = rel->attr->tdtypeid;
    relation.attr->tdtypmod = rel->attr->tdtypmod;
    relation.attr->tdhasoid = rel->attr->tdhasoid;
    relation.attr->tdrefcount = rel->attr->tdrefcount;
    relation.attr->tdhasuids = rel->attr->tdhasuids;

    error_t rc = EOK;
    for (uint32_t i = 0; i < colNum; i++) {
        rc = memcpy_s(relation.attr->attrs[i], sizeof(SysAttributeTupDef), rel->attr->attrs[i],
                      sizeof(SysAttributeTupDef));
        storage_securec_check(rc, "\0", "\0");
    }
}

void StorageTableContext::InsertHeapTable(const char *tableName, StorageRelation relation)
{
    heapTable[tableName] = relation;
}
void StorageTableContext::InsertIndexTable(const char *tableName, StorageRelation relation)
{
    indexTable[tableName] = relation;
}

DstoreTableHandler *StorageTableContext::GetTableHandler(const char *tableName, const char *indexName)
{
    StorageRelation heapRel = simulator->GetHeapRelationEntry(tableName);
    StorageAssert(heapRel != nullptr);
    auto *tableHandler = new DstoreTableHandler(storageInstance);
    StorageRelation indexRel = nullptr;
    if (indexName != nullptr && indexTable.count(indexName) != 0) {
        indexRel = indexTable[indexName];
        StorageAssert(indexRel != nullptr);
    }
    tableHandler->Init(heapRel, indexRel);
    return tableHandler;
}
void StorageTableContext::Destory() noexcept
{
    for (auto table : heapTable) {
        StorageRelation entry = table.second;
        if (entry != nullptr) {
            entry->Destroy();
            DestroyObject((void**)&entry->attr);
            DestroyObject((void**)&entry->rel);
            DestroyObject((void**)&entry);
        }
    }
    heapTable.clear();
    for (auto table : indexTable) {
        StorageRelation entry = table.second;
        if (entry != nullptr) {
            entry->Destroy();
            DestroyObject((void**)&entry->attr);
            DestroyObject((void**)&entry->rel);
            DestroyObject((void**)&entry->index);
            DestroyObject((void**)&entry->indexInfo);
            DestroyObject((void**)&entry);
        }
    }
    indexTable.clear();
}

uint16_t DstoreTableHandler::GetIndexKeyNum() const
{
    return m_indexRel->index->indexKeyAttrsNum;
}
