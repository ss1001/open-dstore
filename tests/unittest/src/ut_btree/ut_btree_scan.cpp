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
#include <gtest/gtest.h>
#include "heap/dstore_heap_scan.h"
#include "index/dstore_btree_build.h"
#include "index/dstore_index_interface.h"
#include "index/dstore_index_handler.h"
#include "index/dstore_btree_scan.h"
#include "ut_btree/ut_btree.h"
#include "diagnose/dstore_index_diagnose.h"
#include "errorcode/dstore_index_error_code.h"
#include "framework/dstore_thread.h"

using namespace DSTORE;

void UTBtree::InitScanKeyInt24(ScanKey keyInfos, Datum arg1, uint16 strategy1, Datum arg2, uint16 strategy2)
{
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Int16GetDatum(arg1), strategy1, keyInfos, 1);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Int32GetDatum(arg2), strategy2, keyInfos + 1, 2);
}

static inline void InitScanKeyInt2TextInt4(ScanKey keyInfos, Datum arg1, uint16 strategy1, Datum arg2, uint16 strategy2, Datum arg3, uint16 strategy3)
{
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Int16GetDatum(arg1), strategy1, keyInfos, 1);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(TEXTOID, PointerGetDatum(arg2), strategy2, keyInfos + 1, 2);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Int32GetDatum(arg3), strategy3, keyInfos + 2, 3);
}

void InitScanKeyIsNull(ScanKey keyInfo, AttrNumber attrNumber, bool nullsFrist = false)
{
    keyInfo->skFlags = 0;
    keyInfo->skFlags |= SCAN_KEY_SEARCHNULL;
    keyInfo->skFlags |= SCAN_KEY_ISNULL;
    if (nullsFrist) {
        keyInfo->skFlags |= SCANKEY_NULLS_FIRST;
    }
    keyInfo->skFlags |= SCAN_KEY_ROW_MEMBER;
    keyInfo->skCollation = DEFAULT_COLLATION_OID;
    keyInfo->skAttno = attrNumber;
}

void InitScanKeyIsNotNull(ScanKey keyInfo, AttrNumber attrNumber, bool nullsFirst = false)
{
    keyInfo->skFlags = 0;
    keyInfo->skFlags |= SCAN_KEY_SEARCHNOTNULL;
    keyInfo->skFlags |= SCAN_KEY_ISNULL;
    keyInfo->skFlags |= SCAN_KEY_ROW_MEMBER;
    if (nullsFirst) {
        keyInfo->skFlags |= SCANKEY_NULLS_FIRST;
    }
    keyInfo->skCollation = DEFAULT_COLLATION_OID;
    keyInfo->skAttno = attrNumber;
}

bool IsScanKeyEqual(ScanKey skey1, ScanKey skey2)
{
    if (skey1->skAttno != skey2->skAttno) {
        return false;
    }
    if (skey1->skArgument != skey2->skArgument) {
        return false;
    }
    if (skey1->skStrategy != skey2->skStrategy) {
        return false;
    }
    return true;
}

TEST_F(UTBtree, BtreePointGetUniqueMatchesFullScan_level0)
{
    int indexCols[] = {2};
    m_utTableHandler->CreateIndex(indexCols, 1, true);

    DefaultRowDef baseRow = m_utTableHandler->GetDefaultRowDef();
    for (int i = 1; i <= 16; ++i) {
        DefaultRowDef row = baseRow;
        row.column_int32 = i;
        IndexTuple *indexTuple = InsertSpecificIndexTuple(&row);
        ASSERT_NE(indexTuple, nullptr);
        DstorePfreeExt(indexTuple);
    }

    Transaction *txn = thrd->GetActiveTransaction();
    txn->Start();
    txn->SetSnapshotCsn();

    IndexScanHandler indexScan;
    ASSERT_EQ(indexScan.InitIndexScanHandler(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(), 1, 0),
              DSTORE_SUCC);
    indexScan.InitSnapshot(txn->GetSnapshot());
    ASSERT_EQ(indexScan.BeginScan(), DSTORE_SUCC);

    HeapScanHandler heapScan(g_storageInstance, thrd, m_utTableHandler->GetTableRel());
    ASSERT_EQ(heapScan.Begin(txn->GetSnapshot()), DSTORE_SUCC);

    int keysToCheck[] = {1, 7, 16};
    for (int key : keysToCheck) {
        ScanKeyData keyInfo[1];
        g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Int32GetDatum(key), SCAN_ORDER_EQUAL, keyInfo,
                                                              1);

        ItemPointerData pointGetCtid = INVALID_ITEM_POINTER;
        ASSERT_EQ(IndexInterface::PointGetUnique(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(),
                                                 keyInfo, txn->GetSnapshot(), &pointGetCtid), DSTORE_SUCC);
        ASSERT_NE(pointGetCtid, INVALID_ITEM_POINTER);

        ASSERT_EQ(indexScan.ReScan(keyInfo), DSTORE_SUCC);
        bool found = false;
        bool recheck = false;
        ASSERT_EQ(IndexInterface::ScanNext(&indexScan, ScanDirection::FORWARD_SCAN_DIRECTION, &found, &recheck),
                  DSTORE_SUCC);
        ASSERT_TRUE(found);
        EXPECT_EQ(*indexScan.GetResultHeapCtid(), pointGetCtid);

        HeapTuple *pointGetTuple = heapScan.FetchTuple(pointGetCtid);
        ASSERT_NE(pointGetTuple, nullptr);
        heapScan.EndFetch();

        ItemPointerData fullScanCtid = *indexScan.GetResultHeapCtid();
        HeapTuple *fullScanTuple = heapScan.FetchTuple(fullScanCtid);
        ASSERT_NE(fullScanTuple, nullptr);
        heapScan.EndFetch();

        EXPECT_EQ(pointGetTuple->GetDiskTupleSize(), fullScanTuple->GetDiskTupleSize());
        EXPECT_EQ(memcmp(pointGetTuple->GetDiskTuple()->GetData(), fullScanTuple->GetDiskTuple()->GetData(),
                         pointGetTuple->GetDiskTupleSize() - HEAP_DISK_TUP_HEADER_SIZE), 0);

        DstorePfreeExt(pointGetTuple);
        DstorePfreeExt(fullScanTuple);
    }

    ScanKeyData missKey[1];
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Int32GetDatum(9999), SCAN_ORDER_EQUAL, missKey, 1);
    ItemPointerData missCtid = INVALID_ITEM_POINTER;
    ASSERT_EQ(IndexInterface::PointGetUnique(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(),
                                             missKey, txn->GetSnapshot(), &missCtid), DSTORE_SUCC);
    EXPECT_EQ(missCtid, INVALID_ITEM_POINTER);

    indexScan.EndScan();
    txn->Commit();
}

TEST_F(UTBtree, BtreePointGetUniqueRejectNonUniqueIndex_level0)
{
    int indexCols[] = {2};
    m_utTableHandler->CreateIndex(indexCols, 1, false);

    Transaction *txn = thrd->GetActiveTransaction();
    txn->Start();
    txn->SetSnapshotCsn();

    ScanKeyData keyInfo[1];
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Int32GetDatum(1), SCAN_ORDER_EQUAL, keyInfo, 1);

    StorageClearError();
    ItemPointerData pointGetCtid = INVALID_ITEM_POINTER;
    EXPECT_EQ(IndexInterface::PointGetUnique(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(),
                                             keyInfo, txn->GetSnapshot(), &pointGetCtid), DSTORE_FAIL);
    EXPECT_EQ(StorageGetErrorCode(), INDEX_ERROR_POINTGET_UNSUPPORTED);
    EXPECT_EQ(pointGetCtid, INVALID_ITEM_POINTER);

    txn->Commit();
}

TEST_F(UTBtree, BtreePointGetUniqueRejectMultiColumn_level0)
{
    int indexCols[] = {1, 2};
    m_utTableHandler->CreateIndex(indexCols, 2, true);

    Transaction *txn = thrd->GetActiveTransaction();
    txn->Start();
    txn->SetSnapshotCsn();

    ScanKeyData keyInfo[1];
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Int16GetDatum(1), SCAN_ORDER_EQUAL, keyInfo, 1);

    StorageClearError();
    ItemPointerData pointGetCtid = INVALID_ITEM_POINTER;
    EXPECT_EQ(IndexInterface::PointGetUnique(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(),
                                             keyInfo, txn->GetSnapshot(), &pointGetCtid), DSTORE_FAIL);
    EXPECT_EQ(StorageGetErrorCode(), INDEX_ERROR_POINTGET_UNSUPPORTED);
    EXPECT_EQ(pointGetCtid, INVALID_ITEM_POINTER);

    txn->Commit();
}

TEST_F(UTBtree, BtreePointGetUniqueNonEqualScanKey_level0)
{
    int indexCols[] = {2};
    m_utTableHandler->CreateIndex(indexCols, 1, true);

    Transaction *txn = thrd->GetActiveTransaction();
    txn->Start();
    txn->SetSnapshotCsn();

    ScanKeyData keyInfo[1];
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Int32GetDatum(1), SCAN_ORDER_GREATER, keyInfo, 1);

    StorageClearError();
    ItemPointerData pointGetCtid = INVALID_ITEM_POINTER;
    EXPECT_EQ(IndexInterface::PointGetUnique(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(),
                                             keyInfo, txn->GetSnapshot(), &pointGetCtid), DSTORE_FAIL);
    EXPECT_EQ(StorageGetErrorCode(), INDEX_ERROR_POINTGET_UNSUPPORTED);
    EXPECT_EQ(pointGetCtid, INVALID_ITEM_POINTER);

    txn->Commit();
}

int UTBtree::GetSatisfiedTupleNum(IndexScanHandler &indexScan, HeapScanHandler &heapScan, ScanDirection dir, bool checkIndexOnly)
{
    int foundNum = 0;
    HeapTuple *heapTuple = nullptr;
    IndexScanDesc indexScanDesc = indexScan.GetScanDesc();

    bool found;
    while (STORAGE_FUNC_SUCC(indexScan.GetNextTuple(ScanDirection::FORWARD_SCAN_DIRECTION, &found)) && found) {
        if (!checkIndexOnly) {
            heapTuple = heapScan.FetchTuple(indexScanDesc->heapCtid);
            StorageAssert(heapTuple != nullptr);
            DstorePfreeExt(heapTuple);
        }
        foundNum++;
    }
    return foundNum;
}

TEST_F(UTBtree, BtreeScanProcessScanKeyTest_level0)
{
    /* Construct index. */
    int indexCols[] = {1, 2};
    bool isUnique = false;
    int numAttrs = sizeof(indexCols) / sizeof(int);
    m_utTableHandler->CreateIndex(indexCols, numAttrs, isUnique);
    int nkeys = m_utTableHandler->GetIndexInfo()->indexKeyAttrsNum;
    EXPECT_EQ(nkeys, 2);

    /* Construct index scan handler. */
    Transaction *txn = thrd->GetActiveTransaction();
    txn->Start();
    txn->SetSnapshotCsn();

    int scankeyNum = 4;
    IndexScanHandler indexScan;
    indexScan.InitIndexScanHandler(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(),
                                   scankeyNum, 0);
    indexScan.InitSnapshot(txn->GetSnapshot());
    indexScan.BeginScan();
    IndexScanDesc indexScanDesc = indexScan.GetScanDesc();
    EXPECT_NE(indexScanDesc, nullptr);

    ScanKeyData keyInfos[scankeyNum];
    bool keysConflictFlag = true;
    int numberOfKeys;
    int numArrayKeys;
    ScanKey processedKey;
    bool isCrExtend;
    bool processOk = false;
    
    /* case 1: [test scankey merge] colnum_int2 > 10 and colnum_int2 > 20 colnum_int4 > 7 and colnum_int4 >= 8 */
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Datum(10), SCAN_ORDER_GREATER, keyInfos, 1);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Datum(20), SCAN_ORDER_GREATER, keyInfos + 1, 1);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Datum(7), SCAN_ORDER_GREATER, keyInfos + 2, 2);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Datum(8), SCAN_ORDER_GREATEREQUAL, keyInfos + 3, 2);
    /* check */
    indexScan.ReScan(keyInfos);
    indexScan.GetScanStatus(keysConflictFlag, numberOfKeys, numArrayKeys, processedKey, isCrExtend);
    EXPECT_EQ(keysConflictFlag, false);
    EXPECT_EQ(numberOfKeys, 2);
    EXPECT_EQ(numArrayKeys, 0);
    /* colnum_int2 > 20 and colnum_int4 >= 8 save */
    processOk = (IsScanKeyEqual(processedKey, keyInfos + 1) && IsScanKeyEqual(processedKey + 1, keyInfos + 3));
    EXPECT_EQ(processOk, true);

    /* case 2: [test scankey merge] colnum_int2 < 2 and colnum_int2 < 10 colnum_int4 < 11 and colnum_int4 <= 11 */
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Datum(2), SCAN_ORDER_LESS, keyInfos, 1);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Datum(10), SCAN_ORDER_LESS, keyInfos + 1, 1);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Datum(11), SCAN_ORDER_LESS, keyInfos + 2, 2);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Datum(11), SCAN_ORDER_LESSEQUAL, keyInfos + 3, 2);
    indexScan.ReScan(keyInfos);
    /* check */
    indexScan.GetScanStatus(keysConflictFlag, numberOfKeys, numArrayKeys, processedKey, isCrExtend);
    EXPECT_EQ(keysConflictFlag, false);
    EXPECT_EQ(numberOfKeys, 2);
    EXPECT_EQ(numArrayKeys, 0);
    /* colnum_int2 < 2 and colnum_int4 < 11 save */
    processOk = (IsScanKeyEqual(processedKey, keyInfos) && IsScanKeyEqual(processedKey + 1, keyInfos + 2));
    EXPECT_EQ(processOk, true);

    /* case 3: [test scankey conflict] colnum_int4 = 2 and colnum_int4 = 10 */
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Datum(2), SCAN_ORDER_EQUAL, keyInfos, 2);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Datum(10), SCAN_ORDER_EQUAL, keyInfos + 1, 2);
    indexScan.ReScan(keyInfos);
    /* check */
    indexScan.GetScanStatus(keysConflictFlag, numberOfKeys, numArrayKeys, processedKey, isCrExtend);
    EXPECT_EQ(keysConflictFlag, true);

    /* case 4: [test scankey conflict] colnum_int4 = 2 and colnum_int4 < 1 */
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Datum(2), SCAN_ORDER_EQUAL, keyInfos, 2);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Datum(1), SCAN_ORDER_LESS, keyInfos + 1, 2);
    indexScan.ReScan(keyInfos);
    /* check */
    indexScan.GetScanStatus(keysConflictFlag, numberOfKeys, numArrayKeys, processedKey, isCrExtend);
    EXPECT_EQ(keysConflictFlag, true);

    /* case 5: [test scankey conflict] colnum_int4 > 2 and colnum_int4 is null */
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Datum(2), SCAN_ORDER_GREATER, keyInfos, 2);
    InitScanKeyIsNull(keyInfos + 1, 2);
    indexScan.ReScan(keyInfos);
    /* check */
    indexScan.GetScanStatus(keysConflictFlag, numberOfKeys, numArrayKeys, processedKey, isCrExtend);
    EXPECT_EQ(keysConflictFlag, true);

    /*
     * case 6: [test scankey (is not null)]
     * colnum_int2 is not null and colnum_int2 < 2 and colnum_int4 is not null and colnum_int4 <= 2
     * with nulls rank last
     */
    InitScanKeyIsNotNull(keyInfos, 1);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Datum(2), SCAN_ORDER_LESS, keyInfos + 1, 1);
    InitScanKeyIsNotNull(keyInfos + 2, 2);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Datum(2), SCAN_ORDER_LESSEQUAL, keyInfos + 3, 2);
    indexScan.ReScan(keyInfos);
    /* check */
    indexScan.GetScanStatus(keysConflictFlag, numberOfKeys, numArrayKeys, processedKey, isCrExtend);
    EXPECT_EQ(keysConflictFlag, false);
    EXPECT_EQ(numberOfKeys, 2);
    EXPECT_EQ(numArrayKeys, 0);
    processOk = (IsScanKeyEqual(processedKey, keyInfos + 1) && IsScanKeyEqual(processedKey + 1, keyInfos + 3));

    /*
     * case 7: [test scankey (is not null)]
     * colnum_int2 is not null and colnum_int2 > 2 and colnum_int4 is not null and colnum_int4 >= 2
     * with nulls rank first
     */
    InitScanKeyIsNotNull(keyInfos, 1, true);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Datum(2), SCAN_ORDER_GREATER, keyInfos + 1, 1);
    InitScanKeyIsNotNull(keyInfos + 2, 2, true);
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Datum(2), SCAN_ORDER_GREATEREQUAL, keyInfos + 3, 2);
    indexScan.ReScan(keyInfos);
    /* check */
    indexScan.GetScanStatus(keysConflictFlag, numberOfKeys, numArrayKeys, processedKey, isCrExtend);
    EXPECT_EQ(keysConflictFlag, false);
    EXPECT_EQ(numberOfKeys, 2);
    EXPECT_EQ(numArrayKeys, 0);
    processOk = (IsScanKeyEqual(processedKey, keyInfos + 1) && IsScanKeyEqual(processedKey + 1, keyInfos + 3));
    indexScan.EndScan();
    
    txn->Commit();
}

TEST_F(UTBtree, BtreeScanCheckTupleFastTest_level0)
{
    Transaction *txn = thrd->GetActiveTransaction();

    /* Fill table with specific data */
    int rowNum = 1000;
    DefaultRowDef *tableDef = (DefaultRowDef*)DstorePalloc(rowNum * sizeof(DefaultRowDef));
    IndexTuple* indexTuple;
    for (int i = 1; i <= rowNum; ++i) {
        int16 r1 = (int16)(i);
        int32 r2 = (int32)(i);
        DefaultRowDef rowDef = m_utTableHandler->GetDefaultRowDef();
        rowDef.column_int16 = r1;
        rowDef.column_int32 = r2;
        rowDef.column_text = (text*) m_utTableHandler->GenerateSpecificDatumPtr(TEXTOID, i%26);
        tableDef[i-1] = rowDef;
    }
    m_utTableHandler->FillTableWithSpecificData(tableDef, rowNum);

    /* Build btree index. */
    int indexCols[] = {2};
    bool isUnique = false;
    int numAttrs = sizeof(indexCols) / sizeof(int);
    m_utTableHandler->CreateIndex(indexCols, numAttrs, isUnique);

    /* Use condition only int32, will cause fast checktuple (memory compare). */
    txn->Start();
    txn->SetSnapshotCsn();
    IndexScanHandler indexScan;
    indexScan.InitIndexScanHandler(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(), 1, 0);
    indexScan.InitSnapshot(txn->GetSnapshot());

    indexScan.BeginScan();
    IndexScanDesc indexScanDesc = indexScan.GetScanDesc();
    EXPECT_NE(indexScanDesc, nullptr);
    {
        HeapScanHandler heapScan(g_storageInstance, thrd, m_utTableHandler->GetTableRel());
        heapScan.Begin(txn->GetSnapshot());
        ScanKeyData keyInfo[1];
        /* colnum_int2 = 500 */
        g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT4OID, Datum(500), SCAN_ORDER_EQUAL, keyInfo, 1);
        indexScan.ReScan(keyInfo);
        int foundNum = GetSatisfiedTupleNum(indexScan, heapScan);
        EXPECT_EQ(foundNum, 1);

        indexScan.ReScan(keyInfo);
        foundNum = GetSatisfiedTupleNum(indexScan, heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
        EXPECT_EQ(foundNum, 1);

        heapScan.EndFetch();
    }
    indexScan.EndScan();
    txn->Commit();
}

TEST_F(UTBtree, BtreeScanEmptyIndexTest_level0)
{
    /* Build empty btree index. */
    int indexCols[] = {1, 2};
    bool isUnique = false;
    int numAttrs = sizeof(indexCols) / sizeof(int);
    m_utTableHandler->CreateIndex(indexCols, numAttrs, isUnique);
    
    Transaction *txn = thrd->GetActiveTransaction();
    txn->Start();
    txn->SetSnapshotCsn();
    int foundNum;
    IndexScanHandler indexScan;
    indexScan.InitIndexScanHandler(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(), 1, 0);
    {
        HeapScanHandler heapScan(g_storageInstance, thrd, m_utTableHandler->GetTableRel());
        heapScan.Begin(txn->GetSnapshot());
        indexScan.BeginScan();
        IndexScanDesc indexScanDesc = indexScan.GetScanDesc();
        EXPECT_NE(indexScanDesc, nullptr);

        ScanKeyData keyInfos[1];

        g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Datum(20), SCAN_ORDER_GREATER, keyInfos, 1);
        indexScan.ReScan(keyInfos);
        foundNum = GetSatisfiedTupleNum(indexScan, heapScan);
        EXPECT_EQ(foundNum, 0);

        g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Datum(20), SCAN_ORDER_LESSEQUAL, keyInfos, 1);
        indexScan.ReScan(keyInfos);
        foundNum = GetSatisfiedTupleNum(indexScan, heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
        EXPECT_EQ(foundNum, 0);

        heapScan.EndFetch();
    }
    indexScan.EndScan();
    
    txn->Commit();
}

TEST_F(UTBtree, BtreeScanWholeIndexTest_level0)
{
    Transaction *txn = thrd->GetActiveTransaction();

    /* Fill table with specific data */
    int rowNum = 5000;
    DefaultRowDef *tableDef = (DefaultRowDef*)DstorePalloc(rowNum * sizeof(DefaultRowDef));
    bool *nullbitmap = DefaultNullBitMap;
    IndexTuple* indexTuple;
    for (int i = 1; i <= rowNum; ++i) {
        int16 r1 = (int16)(i);
        int32 r2 = (int32)(i);
        DefaultRowDef rowDef = m_utTableHandler->GetDefaultRowDef();
        rowDef.column_int16 = r1;
        rowDef.column_int32 = r2;
        rowDef.column_text = (text*) m_utTableHandler->GenerateSpecificDatumPtr(TEXTOID, i%26);
        tableDef[i-1] = rowDef;
    }
    m_utTableHandler->FillTableWithSpecificData(tableDef, rowNum);

    /* Build btree index. */
    int indexCols[] = {1, TEXT_IDX, 2};
    bool isUnique = false;
    int numAttrs = sizeof(indexCols) / sizeof(int);
    m_utTableHandler->CreateIndex(indexCols, numAttrs, isUnique);
    int foundNum;

    /* Scan All index data using empty scankey */
    txn->Start();
    txn->SetSnapshotCsn();
    IndexScanHandler indexScan;
    indexScan.InitIndexScanHandler(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(), 0, 0);
    indexScan.InitSnapshot(txn->GetSnapshot());
    indexScan.BeginScan();
    IndexScanDesc indexScanDesc = indexScan.GetScanDesc();
    EXPECT_NE(indexScanDesc, nullptr);
    {
        HeapScanHandler heapScan(g_storageInstance, thrd, m_utTableHandler->GetTableRel());
        heapScan.Begin(txn->GetSnapshot());
        /* Scan with forward direction */
        indexScan.ReScan(nullptr);
        foundNum = GetSatisfiedTupleNum(indexScan, heapScan);
        EXPECT_EQ(foundNum, rowNum);
        /* Scan with backward direction */
        indexScan.ReScan(nullptr);
        foundNum = GetSatisfiedTupleNum(indexScan, heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
        EXPECT_EQ(foundNum, rowNum);

        heapScan.EndFetch();
    }
    indexScan.EndScan();
    
    txn->Commit();
}

TEST_F(UTBtree, BtreeScanNormalTest_level0)
{
    Transaction *txn = thrd->GetActiveTransaction();

    /* Fill table with specific data */
    int rowNum = 10000;
    DefaultRowDef *tableDef = (DefaultRowDef*)DstorePalloc(rowNum * sizeof(DefaultRowDef));
    bool *nullbitmap = DefaultNullBitMap;
    IndexTuple* indexTuple;
    for (int i = 1; i <= rowNum; ++i) {
        int16 r1 = (int16)(i);
        int32 r2 = (int32)(i);
        DefaultRowDef rowDef = m_utTableHandler->GetDefaultRowDef();
        rowDef.column_int16 = r1;
        rowDef.column_int32 = r2;
        rowDef.column_text = (text*) m_utTableHandler->GenerateSpecificDatumPtr(TEXTOID, i%26);
        tableDef[i-1] = rowDef;
    }
    m_utTableHandler->FillTableWithSpecificData(tableDef, rowNum);

    /* Build btree index. */
    int indexCols[] = {1, TEXT_IDX, 2};
    bool isUnique = false;
    int numAttrs = sizeof(indexCols) / sizeof(int);
    m_utTableHandler->CreateIndex(indexCols, numAttrs, isUnique);

    /* Scan btree index and fetch heap tuple. */
    int nkeys = m_utTableHandler->GetIndexInfo()->indexKeyAttrsNum;
    PageId pageId = m_utTableHandler->GetBtreeSmgr()->GetSegMetaPageId();
    char *statInfo = IndexDiagnose::PrintIndexInfo(g_defaultPdbId, pageId.m_fileId, pageId.m_blockId);
    DstorePfree(statInfo);
    /* Use condition only include int16, text and int32 */
    txn->Start();
    txn->SetSnapshotCsn();
    IndexScanHandler indexScan;
    indexScan.InitIndexScanHandler(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(), nkeys, 0);
    indexScan.InitSnapshot(txn->GetSnapshot());
    indexScan.BeginScan();
    IndexScanDesc indexScanDesc = indexScan.GetScanDesc();
    EXPECT_NE(indexScanDesc, nullptr);
    HeapScanHandler *heapScan = DstoreNew(g_dstoreCurrentMemoryContext)
        HeapScanHandler (g_storageInstance, thrd, m_utTableHandler->GetTableRel());
    heapScan->Begin(txn->GetSnapshot());
    ScanKeyData keyInfos[nkeys];
    int foundNum;

    text* text0 = (text*)m_utTableHandler->GenerateSpecificDatumPtr(TEXTOID, 0%26);
    text* text1 = (text*)m_utTableHandler->GenerateSpecificDatumPtr(TEXTOID, 1%26);
    text* text2 = (text*)m_utTableHandler->GenerateSpecificDatumPtr(TEXTOID, 2%26);
    text* text80 = (text*)m_utTableHandler->GenerateSpecificDatumPtr(TEXTOID, 80%26);

    /* Scan 1: the tuple number which satisfies the condition(arg1>=1000 arg2>='BB' arg3>=2000) is 7385 */
    InitScanKeyInt2TextInt4(keyInfos, 1000, SCAN_ORDER_GREATEREQUAL, (Datum)text1, SCAN_ORDER_GREATEREQUAL, 2000, SCAN_ORDER_GREATEREQUAL);
    /* Scan with forward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, 7693);
    /* Scan with backward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, 7693);

    /* Scan 2: the tuple number which satisfies the condition(arg1>=2000 arg2>='BB' arg3>=1000) is 7385 as well */
    InitScanKeyInt2TextInt4(keyInfos, 2000, SCAN_ORDER_GREATEREQUAL, (Datum)text1, SCAN_ORDER_GREATEREQUAL, 1000, SCAN_ORDER_GREATEREQUAL);
    /* Scan with forward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, 7693);
    /* Scan with backward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, 7693);

    /* Scan 3: the tuple number which satisfies the condition(arg1>=2000 arg2='BB' arg3>=1000) is 308
     * 308 + 7693 ~= 8000
     * 8000 * 25/26 = 7692.3
     */
    InitScanKeyInt2TextInt4(keyInfos, 2000, SCAN_ORDER_GREATEREQUAL, (Datum)text1, SCAN_ORDER_EQUAL, 1000, SCAN_ORDER_GREATEREQUAL);
    /* Scan with forward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, 308);
    /* Scan with backward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, 308);

    /* Scan 4: the tuple number which satisfies the condition(arg1>=8000 arg2>='BB' arg3>=1000) is 1924
     * 2000 * 25/26 = 1923
     */
    InitScanKeyInt2TextInt4(keyInfos, 1000, SCAN_ORDER_GREATEREQUAL, (Datum)text1, SCAN_ORDER_GREATEREQUAL, 8000, SCAN_ORDER_GREATEREQUAL);
    /* Scan with forward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, 1924);
    /* Scan with backward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, 1924);
    InitScanKeyInt2TextInt4(keyInfos, 1000, SCAN_ORDER_GREATEREQUAL, (Datum)text1, SCAN_ORDER_EQUAL, 8000, SCAN_ORDER_GREATEREQUAL);
    /* Scan with forward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, 77);
    /* Scan with backward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, 77);

    /* Scan 6: the tuple number which satisfies the condition(arg1=80 arg2='CCC' arg3=80) is 1 */
    InitScanKeyInt2TextInt4(keyInfos, 80, SCAN_ORDER_EQUAL, (Datum)text80, SCAN_ORDER_EQUAL, 80, SCAN_ORDER_EQUAL);
    /* Scan with forward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, 1);
    /* Scan with backward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, 1);

    /* Scan 7: the tuple number which satisfies the condition(arg1<=5000 arg2>='BB' arg3<=4000) is 3847
     * 4000 * 25/26 = 3847
     */
    InitScanKeyInt2TextInt4(keyInfos, 5000, SCAN_ORDER_LESSEQUAL, (Datum)text1, SCAN_ORDER_GREATEREQUAL, 4000, SCAN_ORDER_LESSEQUAL);
    /* Scan with forward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, 3847);
    /* Scan with backward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, 3847);

    /* Scan 8: the tuple number which satisfies the condition(arg1<=5000 arg2>='BB' arg2<4000) is 3846
     * 3999 * 25/26 = 3846
     */
    InitScanKeyInt2TextInt4(keyInfos, 5000, SCAN_ORDER_LESSEQUAL, (Datum)text1, SCAN_ORDER_GREATEREQUAL, 4000, SCAN_ORDER_LESS);
    /* Scan with forward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, 3846);
    /* Scan with backward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, 3846);

    /* Scan 9: the tuple number which satisfies the condition(arg1<=5000 arg2>='A' arg2<=4000) is 4000
     * 4000 * 26/26 = 4000
     */
    InitScanKeyInt2TextInt4(keyInfos, 5000, SCAN_ORDER_LESSEQUAL, (Datum)text0, SCAN_ORDER_GREATEREQUAL, 4000, SCAN_ORDER_LESSEQUAL);
    /* Scan with forward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, 4000);
    /* Scan with backward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, 4000);

    /* Scan 10: the tuple number which satisfies the condition(arg1<=5000 arg2>='CCC' arg2<=4000) is 3693
     * 4000 * 24/26 = 3693
     */
    InitScanKeyInt2TextInt4(keyInfos, 5000, SCAN_ORDER_LESSEQUAL, (Datum)text2, SCAN_ORDER_GREATEREQUAL, 4000, SCAN_ORDER_LESSEQUAL);
    /* Scan with forward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, 3693);
    /* Scan with backward direction */
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, 3693);

    heapScan->EndFetch();
    delete heapScan;
    indexScan.EndScan();
    
    txn->Commit();
}

TEST_F(UTBtree, BtreeScanSearchNULLTest_level0)
{
    int indexCols[] = {1, 2};
    TupleDesc heapTupleDesc = m_utTableHandler->GetHeapTupDesc();

    /* Fill table with specific data with some null */
    int rowNum = 600;
    HeapTuple *heapTuple;
    bool *nullbitmap = DefaultNullBitMap;
    Datum *heapValues = (Datum*)DstorePalloc(heapTupleDesc->natts * sizeof(Datum));
    bool *heapIsnulls = (bool*)DstorePalloc(heapTupleDesc->natts * sizeof(bool));
    /* Insert (...1,null...)(...1,1...)(...null,1...)(...null,null...)
     * (...2,null...)(...2,2...)(...null,2...)(...null,null...)... into heap */
    for (int i = 1; i <= rowNum; ++i) {
        DefaultRowDef insertRow = m_utTableHandler->GetDefaultRowDef();
        insertRow.column_int16 = i;
        insertRow.column_int32 = i;
        /* Insert (i,null) */
        nullbitmap[indexCols[1]] = true;
        heapTuple = m_utTableHandler->GetSpecificHeapTuple(&insertRow, heapValues, heapIsnulls, nullbitmap);
        m_utTableHandler->InsertHeapTupAndCheckResult(heapTuple);
        DstorePfreeExt(heapTuple);
        nullbitmap[indexCols[1]] = false;
        /* Insert (i,i) */
        heapTuple = m_utTableHandler->GetSpecificHeapTuple(&insertRow, heapValues, heapIsnulls, nullbitmap);
        m_utTableHandler->InsertHeapTupAndCheckResult(heapTuple);
        DstorePfreeExt(heapTuple);
        /* Insert (null,i) */
        nullbitmap[indexCols[0]] = true;
        heapTuple = m_utTableHandler->GetSpecificHeapTuple(&insertRow, heapValues, heapIsnulls, nullbitmap);
        m_utTableHandler->InsertHeapTupAndCheckResult(heapTuple);
        DstorePfreeExt(heapTuple);
        nullbitmap[indexCols[0]] = false;
        /* Insert (null,null) */
        nullbitmap[indexCols[0]] = true;
        nullbitmap[indexCols[1]] = true;
        heapTuple = m_utTableHandler->GetSpecificHeapTuple(&insertRow, heapValues, heapIsnulls, nullbitmap);
        m_utTableHandler->InsertHeapTupAndCheckResult(heapTuple);
        DstorePfreeExt(heapTuple);
        nullbitmap[indexCols[0]] = false;
        nullbitmap[indexCols[1]] = false;
    }
    DstorePfree(heapValues);
    DstorePfree(heapIsnulls);

    /* Build btree index */
    bool isUnique = false;
    int numAttrs = sizeof(indexCols) / sizeof(int);
    /* Default: Nulls Last */
    m_utTableHandler->CreateIndex(indexCols, numAttrs, isUnique); 
    
    /* Construct index scan handler. */
    Transaction *txn = thrd->GetActiveTransaction();
    txn->Start();
    txn->SetSnapshotCsn();

    int scankeyNum = 2;
    int foundNum;
    IndexScanHandler indexScan;
    indexScan.InitIndexScanHandler(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(), scankeyNum, 0);
    HeapScanHandler *heapScan = DstoreNew(g_dstoreCurrentMemoryContext)
        HeapScanHandler (g_storageInstance, thrd, m_utTableHandler->GetTableRel());
    heapScan->Begin(txn->GetSnapshot());
    indexScan.InitSnapshot(txn->GetSnapshot());
    indexScan.BeginScan();
    IndexScanDesc indexScanDesc = indexScan.GetScanDesc();
    EXPECT_NE(indexScanDesc, nullptr);
    ScanKeyData keyInfos[scankeyNum];

    /* case 1: scan (not null, null) */
    InitScanKeyIsNotNull(keyInfos, 1);
    InitScanKeyIsNull(keyInfos + 1, 2);
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, rowNum);
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, rowNum);

    /* case 2: scan (null, not null) */
    InitScanKeyIsNull(keyInfos, 1);
    InitScanKeyIsNotNull(keyInfos + 1, 2);
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, rowNum);
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, rowNum);

    /* case 3: scan (null, null) */
    InitScanKeyIsNull(keyInfos, 1);
    InitScanKeyIsNull(keyInfos + 1, 2);
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, rowNum);
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, rowNum);

    /* case 4: scan (not null, not null) */
    InitScanKeyIsNotNull(keyInfos, 1);
    InitScanKeyIsNotNull(keyInfos + 1, 2);
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, rowNum);
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, rowNum);

    /* case 5: [mix] scan (> 500, not null) */
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Datum(500), SCAN_ORDER_GREATER, keyInfos, 1);
    InitScanKeyIsNotNull(keyInfos + 1, 2);
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, 100);
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, 100);

    /* case 6: [mix] scan (> 500, null) */
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Datum(500), SCAN_ORDER_GREATER, keyInfos, 1);
    InitScanKeyIsNull(keyInfos + 1, 2);
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, 100);
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan, ScanDirection::BACKWARD_SCAN_DIRECTION);
    EXPECT_EQ(foundNum, 100);

    /* case 7: [mix] scan (= 500 && is null) => conflict */
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Datum(500), SCAN_ORDER_EQUAL, keyInfos, 1);
    InitScanKeyIsNull(keyInfos + 1, 1);
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, 0);

    heapScan->EndFetch();
    delete heapScan;
    indexScan.EndScan();
    
    txn->Commit();
}

TEST_F(UTBtree, BtreeScanResetPosTest_level0)
{
    /* Fill table with specific data */
    int rowNum = 1000;
    DefaultRowDef *tableDef = (DefaultRowDef*)DstorePalloc(rowNum * sizeof(DefaultRowDef));
    IndexTuple* indexTuple;
    for (int i = 1; i <= rowNum; ++i) {
        int16 r1 = (int16)(i);
        int32 r2 = (int32)(i);
        DefaultRowDef rowDef = m_utTableHandler->GetDefaultRowDef();
        rowDef.column_int16 = r1;
        rowDef.column_int32 = r2;
        rowDef.column_text = (text*) m_utTableHandler->GenerateSpecificDatumPtr(TEXTOID, i%26);
        tableDef[i-1] = rowDef;
    }
    m_utTableHandler->FillTableWithSpecificData(tableDef, rowNum);

    /* Build btree index. */
    int indexCols[] = {1, 2};
    bool isUnique = false;
    int numAttrs = sizeof(indexCols) / sizeof(int);
    m_utTableHandler->CreateIndex(indexCols, numAttrs, isUnique);

    /* Construct index scan handler. */
    Transaction *txn = thrd->GetActiveTransaction();
    txn->Start();
    txn->SetSnapshotCsn();

    IndexScanHandler indexScan;
    indexScan.InitIndexScanHandler(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(), 1, 0);
    indexScan.InitSnapshot(txn->GetSnapshot());
    indexScan.BeginScan();
    IndexScanDesc indexScanDesc = indexScan.GetScanDesc();
    EXPECT_NE(indexScanDesc, nullptr);
    ScanKeyData keyInfos[1];
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Datum(1), SCAN_ORDER_GREATER, keyInfos, 1);
    indexScan.ReScan(keyInfos);

    /* just get one index tuple */
    bool found;
    EXPECT_EQ(indexScan.GetNextTuple(ScanDirection::FORWARD_SCAN_DIRECTION, &found), DSTORE_SUCC);
    EXPECT_TRUE(found);
    EXPECT_NE(*indexScan.GetResultHeapCtid(), INVALID_ITEM_POINTER);

    /* reset scan position */
    indexScan.ReScan(nullptr);
    /* only reset scan position can it fetch the same tuple */
    EXPECT_EQ(indexScan.GetNextTuple(ScanDirection::FORWARD_SCAN_DIRECTION, &found), DSTORE_SUCC);
    EXPECT_TRUE(found);
    EXPECT_NE(*indexScan.GetResultHeapCtid(), INVALID_ITEM_POINTER);

    indexScan.EndScan();

    txn->Commit();
}

TEST_F(UTBtree, BtreeScanRollbackTest_level0)
{
    HeapScanHandler *heapScan = nullptr;
    Transaction *txn = thrd->GetActiveTransaction();

    /* Build btree index using empty table */
    int indexCols[] = {1, TEXT_IDX, 2};
    bool isUnique = false;
    int numAttrs = sizeof(indexCols) / sizeof(int);
    m_utTableHandler->CreateIndex(indexCols, numAttrs, isUnique);
    std::vector<IndexTuple*> deleteList;
    DefaultRowDef insertRow = m_utTableHandler->GetDefaultRowDef();

    /*
     * Step 1. Insert (1,text,1)(2,text,2)... into btree, and abort one txn in every two txn.
     * In this case, rowNum should satisfy the condition: rowNum % 4 = 0.
     */
    int rowNum = 1000;
    IndexTuple *indexTuple;
    bool *nullbitmap = DefaultNullBitMap;
    bool txnCommit = true;
    for (int i = 1; i <= rowNum; ++i) {
        DefaultRowDef insertRowTemp = insertRow;
        insertRowTemp.column_int16 = (int16)(i);
        insertRowTemp.column_int32 = (int32)(i);
        insertRowTemp.column_text = (text*) m_utTableHandler->GenerateSpecificDatumPtr(TEXTOID, i%26);
        /* Insert (i,i) */
        txn->Start();
        indexTuple = InsertSpecificIndexTuple(&insertRowTemp, nullbitmap, true);
        EXPECT_NE(indexTuple, nullptr);
        if (txnCommit) {
            txn->Commit();
            deleteList.push_back(indexTuple);
        } else {
            txn->Abort();
        }
        txnCommit = !txnCommit;
    }

    /* Step 2. Scan every tuple, inserted tuple whose txn is abort should not be visible. */
    int nkeys = m_utTableHandler->GetIndexInfo()->indexKeyAttrsNum;
    IndexScanHandler indexScan;
    indexScan.InitIndexScanHandler(m_utTableHandler->GetIndexRel(),m_utTableHandler->GetIndexInfo(), nkeys, 0);
    txn->Start();
    txn->SetSnapshotCsn();
    heapScan = DstoreNew(g_dstoreCurrentMemoryContext)
        HeapScanHandler (g_storageInstance, thrd, m_utTableHandler->GetTableRel());
    heapScan->Begin(txn->GetSnapshot());
    indexScan.InitSnapshot(txn->GetSnapshot());
    indexScan.BeginScan();
    IndexScanDesc indexScanDesc = indexScan.GetScanDesc();
    ASSERT_NE(indexScanDesc, nullptr);
    ScanKeyData keyInfos[nkeys];
    int foundNum = 0;
    for (int i = 1; i <= rowNum; ++i) {
        text* text1 = (text*) m_utTableHandler->GenerateSpecificDatumPtr(TEXTOID, i%26);
        InitScanKeyInt2TextInt4(keyInfos, i, SCAN_ORDER_EQUAL, (Datum)text1, SCAN_ORDER_EQUAL, i, SCAN_ORDER_EQUAL);
        indexScan.ReScan(keyInfos);
        foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
        if (txnCommit) {
            EXPECT_EQ(foundNum, 1);
        } else {
            EXPECT_EQ(foundNum, 0);
        }
        txnCommit = !txnCommit;
    }
    indexScan.EndScan();
    heapScan->EndFetch();
    delete heapScan;
    txn->Commit();

    /* Step 3. Delete tuple and abort one txn in every two txn. */
    for (int i = 0; i < (int)deleteList.size(); ++i) {
        txn->Start();
        txn->SetSnapshotCsn();
        RetStatus status = DeleteIndexTuple(deleteList[i], true);
        EXPECT_EQ(status, DSTORE_SUCC);
        if (txnCommit) {
            txn->Commit();
        } else {
            txn->Abort();
        }
        txnCommit = !txnCommit;
        DstorePfreeExt(deleteList[i]);
    }

    /* Step 4. Scan every tuple, deleted tuple whose txn is abort should be visible. */
    txn->Start();
    txn->SetSnapshotCsn();
    heapScan = DstoreNew(g_dstoreCurrentMemoryContext) HeapScanHandler(g_storageInstance, thrd,
                                                                       m_utTableHandler->GetTableRel());
    heapScan->Begin(txn->GetSnapshot());
    indexScan.InitSnapshot(txn->GetSnapshot());
    indexScan.BeginScan();
    indexScanDesc = indexScan.GetScanDesc();
    ASSERT_NE(indexScanDesc, nullptr);
    foundNum = 0;
    for (int i = 1; i <= rowNum; i += 2) {
        text* text1 = (text*) m_utTableHandler->GenerateSpecificDatumPtr(TEXTOID, i%26);
        InitScanKeyInt2TextInt4(keyInfos, i, SCAN_ORDER_EQUAL, (Datum)text1, SCAN_ORDER_EQUAL, i, SCAN_ORDER_EQUAL);
        indexScan.ReScan(keyInfos);
        foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
        if (txnCommit) {
            EXPECT_EQ(foundNum, 0);
        } else {
            EXPECT_EQ(foundNum, 1);
        }
        txnCommit = !txnCommit;
    }
    indexScan.EndScan();
    heapScan->EndFetch();
    delete heapScan;
    txn->Commit();

    /* Step 5. In a transaction, insert 1000 tuples to split one page, then rollback the transaction. */
    int rowNumAbort = 1000;
    txn->Start();
    for (int i = 1; i <= rowNumAbort; ++i) {
        DefaultRowDef insertRowTemp = insertRow;
        insertRowTemp.column_int16 = (int16)(i);
        insertRowTemp.column_int32 = (int32)(i);
        /* Insert(i,i) */
        indexTuple = InsertSpecificIndexTuple(&insertRowTemp, nullbitmap, true);
        EXPECT_NE(indexTuple, nullptr);
    }
    txn->Abort();

    /* Step 6: Scan all tuples, aborted tuples shouldn't be visible. */
    txn->Start();
    txn->SetSnapshotCsn();
    heapScan = DstoreNew(g_dstoreCurrentMemoryContext) HeapScanHandler(g_storageInstance, thrd,
                                                                       m_utTableHandler->GetTableRel());
    heapScan->Begin(txn->GetSnapshot());
    indexScan.InitSnapshot(txn->GetSnapshot());
    indexScan.BeginScan();

    text* text0 = (text*) m_utTableHandler->GenerateSpecificDatumPtr(TEXTOID, 0);
    InitScanKeyInt2TextInt4(keyInfos, 0, SCAN_ORDER_GREATEREQUAL, (Datum) text0, SCAN_ORDER_GREATEREQUAL, 0, SCAN_ORDER_GREATEREQUAL);
    indexScan.ReScan(keyInfos);
    foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, rowNum / 4);
    heapScan->EndFetch();
    delete heapScan;
    indexScan.EndScan();
    
    txn->Commit();
}

TEST_F(UTBtree, BtreeScanCRPageTest_level0)
{
    Transaction *txn = thrd->GetActiveTransaction();

    /* Step 1: Build btree index using empty table */
    int indexCols[] = {1, 2};
    bool isUnique = true;
    int numAttrs = sizeof(indexCols) / sizeof(int);
    m_utTableHandler->CreateIndex(indexCols, numAttrs, isUnique);

    /* Step 2: Insert (1,1)(2,2)...(1000,1000) into btree. */
    int rowNum = 1000;
    IndexTuple *indexTuple;
    std::vector<CommitSeqNo> specialSnapshot;
    std::vector<IndexTuple*> deleteList;
    for (int i = 1; i <= rowNum; ++i) {
        DefaultRowDef insertRow = m_utTableHandler->GetDefaultRowDef();
        insertRow.column_int16 = i;
        insertRow.column_int32 = i;
        /* Insert (i,i) */
        indexTuple = InsertSpecificIndexTuple(&insertRow);
        EXPECT_NE(indexTuple, nullptr);
        if (i % 2 == 0) {
            deleteList.push_back(indexTuple);
        } else {
            DstorePfreeExt(indexTuple);
        }
        if (i % 100 == 0) {
            /* Record some old snapshot for read cr page */
            txn->Start();
            txn->SetSnapshotCsn();
            specialSnapshot.push_back(txn->GetSnapshotCsn());
            txn->Commit();
        }
    }

    /* Step 3: Use all old snapshot to scan btree */
    int nkeys = m_utTableHandler->GetIndexInfo()->indexKeyAttrsNum;

    IndexScanHandler indexScan;
    indexScan.InitIndexScanHandler(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(), nkeys, 0);
    HeapScanHandler *heapScan = nullptr;
    txn->Start();
    indexScan.BeginScan();
    IndexScanDesc indexScanDesc = indexScan.GetScanDesc();
    ASSERT_NE(indexScanDesc, nullptr);
    ScanKeyData keyInfos[2];
    InitScanKeyInt24(keyInfos, 0, SCAN_ORDER_GREATEREQUAL, 0, SCAN_ORDER_GREATEREQUAL);
    for (int i = 1; i <= (int)specialSnapshot.size(); ++i) {
        txn->SetSnapshotCsnForFlashback(specialSnapshot[i - 1]);
        indexScan.ReScan(keyInfos);
        heapScan = DstoreNew(g_dstoreCurrentMemoryContext)
            HeapScanHandler(g_storageInstance, thrd, m_utTableHandler->GetTableRel());
        indexScan.InitSnapshot(txn->GetSnapshot());
        heapScan->Begin(txn->GetSnapshot());
        int foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
        EXPECT_EQ(foundNum, i * 100);
        heapScan->EndFetch();
        delete heapScan;
        heapScan = nullptr;
    }

    /* Step 4: Use existing cr page to construct new cr page */
    int i = specialSnapshot.size() / 2;
    txn->SetSnapshotCsnForFlashback(specialSnapshot[i - 1]);
    indexScan.ReScan(keyInfos);
    heapScan = DstoreNew(g_dstoreCurrentMemoryContext)
        HeapScanHandler(g_storageInstance, thrd, m_utTableHandler->GetTableRel());
    indexScan.InitSnapshot(txn->GetSnapshot());
    heapScan->Begin(txn->GetSnapshot());
    int foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
    EXPECT_EQ(foundNum, i * 100);
    heapScan->EndFetch();
    delete heapScan;
    heapScan = nullptr;
    indexScan.EndScan();
    txn->Commit();

    /* Step 5: Delete tuple and save some old snpshot. */
    std::vector<CommitSeqNo>().swap(specialSnapshot);
    for (int i = 1; i <= (int)deleteList.size(); ++i) {
        EXPECT_EQ(DeleteIndexTuple(deleteList[i - 1]), DSTORE_SUCC);
        DstorePfreeExt(deleteList[i - 1]);
        if (i % 100 == 0) {
            txn->Start();
            txn->SetSnapshotCsn();
            specialSnapshot.push_back(txn->GetSnapshotCsn());
            txn->Commit();
        }
    }

    /* Step 6: Use all old snapshot to scan btree */
    txn->Start();
    indexScan.BeginScan();
    indexScanDesc = indexScan.GetScanDesc();
    ASSERT_NE(indexScanDesc, nullptr);
    InitScanKeyInt24(keyInfos, 0, SCAN_ORDER_GREATEREQUAL, 0, SCAN_ORDER_GREATEREQUAL);
    for (int i = 1; i <= (int)specialSnapshot.size(); ++i) {
        txn->SetSnapshotCsnForFlashback(specialSnapshot[i - 1]);
        indexScan.InitSnapshot(txn->GetSnapshot());
        heapScan = DstoreNew(g_dstoreCurrentMemoryContext)
            HeapScanHandler(g_storageInstance, thrd, m_utTableHandler->GetTableRel());
        heapScan->Begin(txn->GetSnapshot());
        indexScan.ReScan(keyInfos);
        int foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
        EXPECT_EQ(foundNum, rowNum - i * 100);
        heapScan->EndFetch();
        delete heapScan;
        heapScan = nullptr;
    }
    indexScan.EndScan();
    
    txn->Commit();
}

/*
 * In one scenario, we have the timeline in one transaction as follows:
 * TIMELINE 1: Scan page A and construct cr page of A.
 * TIMELINE 2: Insert one tuple causing page A split and new tuple is assigned to new right page B.
 * TIMELINE 3: Scan page A
 * At TIMELINE 3, the new tuple must be visible to current transaction.
 */
TEST_F(UTBtree, BtreeScanCRCornerCaseTest1_level0)
{
    Transaction *txn = thrd->GetActiveTransaction();
    bool *nullbitmap = DefaultNullBitMap;

    /* Fill table with specific data */
    int rowNum = 270;
    DefaultRowDef rowDef = m_utTableHandler->GetDefaultRowDef();
    DefaultRowDef *tableDef = (DefaultRowDef*)DstorePalloc(rowNum * sizeof(DefaultRowDef));
    /* Insert (1,1)(2,2)...(270,270) into heap */
    for (int i = 1; i <= rowNum; ++i) {
        rowDef.column_int16 = (int16)i;
        rowDef.column_int32 = (int32)i;
        tableDef[i - 1] = rowDef;
    }
    m_utTableHandler->FillTableWithSpecificData(tableDef, rowNum);
    DstorePfree(tableDef);

    /* Build btree index */
    int indexCols[] = {1, 2};
    bool isUnique = false;
    int numAttrs = sizeof(indexCols) / sizeof(int);
    m_utTableHandler->CreateIndex(indexCols, numAttrs, isUnique);

    /* Insert new tuple until page split and new tuple is assigned to right page. */
    IndexScanHandler indexScan;
    indexScan.InitIndexScanHandler(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(), 2, 0);
    for (int i = 1; i <= 30; ++i) {
        int foundNum;
        txn->Start();
        txn->SetSnapshotCsn();
        indexScan.InitSnapshot(txn->GetSnapshot());

        indexScan.BeginScan();
        HeapScanHandler *heapScan = DstoreNew(g_dstoreCurrentMemoryContext)
            HeapScanHandler(g_storageInstance, thrd, m_utTableHandler->GetTableRel());
        heapScan->Begin(txn->GetSnapshot());

        ScanKeyData keyInfos[2];
        InitScanKeyInt24(keyInfos, 0, SCAN_ORDER_GREATEREQUAL, 0, SCAN_ORDER_GREATEREQUAL);

        indexScan.ReScan(keyInfos);
        foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
        ASSERT_EQ(foundNum, rowNum + i - 1);

        rowDef.column_int16 = (int16)(rowNum + i);
        rowDef.column_int32 = (int32)(rowNum + i);
        EXPECT_NE(InsertSpecificIndexTuple(&rowDef, nullbitmap, true), nullptr);
        indexScan.InitSnapshot(txn->GetSnapshot());
        heapScan->EndFetch();
        delete heapScan;
        heapScan = DstoreNew(g_dstoreCurrentMemoryContext)
            HeapScanHandler(g_storageInstance, thrd, m_utTableHandler->GetTableRel());
        heapScan->Begin(txn->GetSnapshot());

        indexScan.ReScan(keyInfos);
        foundNum = GetSatisfiedTupleNum(indexScan, *heapScan);
        ASSERT_EQ(foundNum, rowNum + i);
        heapScan->EndFetch();
        delete heapScan;
        indexScan.EndScan();

        txn->Commit();
    }
}

TEST_F(UTBtree, BtreeScanMarkAndRestorePosTest_level0)
{
    Transaction *txn = thrd->GetActiveTransaction();

    /* Fill table with specific data */
    int rowNum = 2002, repeatNumber1 = 1, repeatNumber2 = 333;
    DefaultRowDef rowDef = m_utTableHandler->GetDefaultRowDef();
    DefaultRowDef *tableDef = (DefaultRowDef*)DstorePalloc(rowNum * sizeof(DefaultRowDef));
    for (int i = 1; i <= rowNum; ++i) {
        if (i <= 1000) {
            rowDef.column_int16 = (int16)(i);
        } else if (i <= 2000) {
            rowDef.column_int16 = (int16)(repeatNumber2);
        } else {
            rowDef.column_int16 = (int16)(repeatNumber1);
        }
        tableDef[i - 1] = rowDef;
    }
    m_utTableHandler->FillTableWithSpecificData(tableDef, rowNum);
    DstorePfree(tableDef);

    /* Build btree index. */
    int indexCols[] = {1};
    bool isUnique = false;
    int numAttrs = sizeof(indexCols) / sizeof(int);
    m_utTableHandler->CreateIndex(indexCols, numAttrs, isUnique);

    /* Construct index scan handler. */
    IndexScanHandler indexScan;
    txn->Start();
    indexScan.InitIndexScanHandler(m_utTableHandler->GetIndexRel(), m_utTableHandler->GetIndexInfo(), 1, 0);
    txn->SetSnapshotCsn();
    indexScan.InitSnapshot(txn->GetSnapshot());
    indexScan.BeginScan();
    IndexScanDesc indexScanDesc = indexScan.GetScanDesc();
    EXPECT_NE(indexScanDesc, nullptr);

    /* Test repeatNumber1 */
    ScanKeyData keyInfos[1];
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Datum(repeatNumber1), SCAN_ORDER_EQUAL, keyInfos, 1);
    indexScan.ReScan(keyInfos);
    /* just get one index tuple */
    bool found;
    EXPECT_EQ(indexScan.GetNextTuple(ScanDirection::FORWARD_SCAN_DIRECTION, &found), DSTORE_SUCC);
    EXPECT_TRUE(found);
    EXPECT_NE(*indexScan.GetResultHeapCtid(), INVALID_ITEM_POINTER);
    int numOfTuples = 1;
    indexScan.MarkPosition();
    while (STORAGE_FUNC_SUCC(indexScan.GetNextTuple(ScanDirection::FORWARD_SCAN_DIRECTION, &found)) && found) {
        numOfTuples++;
    }
    EXPECT_EQ(numOfTuples, 3);
    indexScan.RestorePosition();
    /* After restorepos, merge join operator will get next tuple and has stored the first matched tuple */
    int numOfMatchTuple = 1;
    while (STORAGE_FUNC_SUCC(indexScan.GetNextTuple(ScanDirection::FORWARD_SCAN_DIRECTION, &found)) && found) {
        numOfMatchTuple++;
    }
    EXPECT_EQ(numOfMatchTuple, numOfTuples);
    
    /* Test repeatNumber2 */
    g_storageInstance->GetCacheHashMgr()->GenerateScanKey(INT2OID, Datum(repeatNumber2), SCAN_ORDER_EQUAL, keyInfos, 1);
    indexScan.ReScan(keyInfos);
    /* enable read index page from cr buffer fault injection point */
    FAULT_INJECTION_ACTIVE(DstoreTransactionFI::READ_PAGE_FROM_CR_BUFFER, FI_GLOBAL);
    /* just get one index tuple */
    EXPECT_EQ(indexScan.GetNextTuple(ScanDirection::FORWARD_SCAN_DIRECTION, &found), DSTORE_SUCC);
    EXPECT_TRUE(found);
    EXPECT_NE(*indexScan.GetResultHeapCtid(), INVALID_ITEM_POINTER);
    numOfTuples = 1;
    indexScan.MarkPosition();
    while (STORAGE_FUNC_SUCC(indexScan.GetNextTuple(ScanDirection::FORWARD_SCAN_DIRECTION, &found)) && found) {
        numOfTuples++;
    }
    EXPECT_EQ(numOfTuples, 1001);
    indexScan.RestorePosition();
    numOfMatchTuple = 1;
    while (STORAGE_FUNC_SUCC(indexScan.GetNextTuple(ScanDirection::FORWARD_SCAN_DIRECTION, &found)) && found) {
        numOfMatchTuple++;
    }
    EXPECT_EQ(numOfMatchTuple, numOfTuples);
    /* disable read index page from cr buffer fault injection point */
    FAULT_INJECTION_INACTIVE(DstoreTransactionFI::READ_PAGE_FROM_CR_BUFFER, FI_GLOBAL);

    indexScan.EndScan();
    txn->Commit();
}
