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

#ifndef SRC_GAUSSKERNEL_INCLUDE_INDEX_DSTORE_BTREE_SCAN_H
#define SRC_GAUSSKERNEL_INCLUDE_INDEX_DSTORE_BTREE_SCAN_H

#include "tuple/dstore_index_tuple.h"
#include "index/dstore_btree.h"
#include "common/algorithm/dstore_tuplesort.h"

namespace DSTORE {

using IndexScanDesc = struct IndexScanDescData*;

const StrategyNumber SCAN_ORDER_INVALID = 0;
const StrategyNumber SCAN_ORDER_LESS = 1;
const StrategyNumber SCAN_ORDER_LESSEQUAL = 2;
const StrategyNumber SCAN_ORDER_EQUAL = 3;
const StrategyNumber SCAN_ORDER_GREATEREQUAL = 4;
const StrategyNumber SCAN_ORDER_GREATER = 5;
const uint16 MAINTAIN_ORDER = 6;
const uint16 SORT_SUPPORT = 7;
const uint16 SCAN_PROC_NUM = 1;
const uint16 SORT_PROC_NUM = 2;
inline uint16 ConvertToSqlProcNum(uint16 procNum)
{
    StorageAssert(procNum == MAINTAIN_ORDER || procNum == SORT_SUPPORT);
    return (procNum == MAINTAIN_ORDER ? SCAN_PROC_NUM : SORT_PROC_NUM);
}

const StrategyNumber MAX_STRATEGY_NUM = 5;

/*
 * Only used when we are in show any tuples mode of index only scan to denote that the insert or delete transaction of
 * the index tuple is freeze. Note that it does NOT mean a valid transaction with xid = 0.
 */
const Xid FROZEN_XACT_ID = Xid(0);

inline StrategyNumber CommuteStrategyNumber(StrategyNumber strategy)
{
    return static_cast<StrategyNumber>(MAX_STRATEGY_NUM + 1 - strategy);
}

struct BtrLastUsedInfo {
    bool isValid;
    PageId lastUsedPage;
    PageId lastUsedParentPage;
};

const uint16 LOCAL_CR_PAGE_NUM = 2;
struct BtrScanContext {
    BufferDesc *baseBufDesc;
    BufferDesc *crBufDesc;
    BtrPage localCrPage[LOCAL_CR_PAGE_NUM]; /* the first page for normal, the second page is for cr extend. */

    PageId currPage;
    PageId nextPage;

    /* if lastUsedInfo is valid, no need rescan */
    BtrLastUsedInfo lastUsedInfo;
    ItemPointerData prevMatchedHeapCtid;
    Oid prevMatchedTableOid;

    bool moreLeft;
    bool moreRight;

    int firstItem;
    int lastItem;
    int itemIndex;

    IndexTuple *matchItems[MAX_CTID_PER_BTREE_PAGE];

    Xid* matchInsertXids;
    Xid* matchDeleteXids;

    PageId GetLastUsedPage()
    {
        return lastUsedInfo.lastUsedPage;
    }
    PageId GetLastUsedParentPage()
    {
        return lastUsedInfo.lastUsedParentPage;
    }
};

/* We need one of these for each SCAN_KEY_SEARCHARRAY scan key */
struct BtrArrayCondInfo {
    int checkingKeyIdx;  /* index of current IN/ANY array condition in m_scanKeyForCheck */
    Oid elemType;        /* type of current array element. it may be different with the column type. */
    int numElem;         /* number of elements in current IN/ANY array value */
    int curElem;         /* index of current element in elemValues. */
    int markElem;        /* indx of marked element in elemValues. */
    Datum *elemValues;   /* values of the IN/ANY array condition */
};

class BtreeScan : public Btree {
public:
    BtreeScan(StorageRelation indexRel, IndexInfo *indexInfo);
    ~BtreeScan() override;
    RetStatus BeginScan(IndexScanDesc scan);
    RetStatus PointGetUnique(ScanKey scanKey, ItemPointerData *heapCtid);
    RetStatus GetNextTuple(IndexScanDesc scan, ScanDirection dir, bool *found);
    void EndScan();
    RetStatus ReScan(IndexScanDesc scan);
    RetStatus SetShowAnyTuples(bool showAnyTuples);
    void DumpScanPage(Datum &fileId, Datum &blockId, Datum &data);
    inline void InitSnapshot(Snapshot snapshot)
    {
        m_snapshot.SetSnapshotType(snapshot->snapshotType);
        m_snapshot.SetCsn(snapshot->snapshotCsn);
        m_snapshot.SetCid(snapshot->currentCid);
    }

    /* Return m_scanKeyForCheck and m_numberOfKeys. */
    ScanKey GetScanKeyInfo(int &numberOfKeys)
    {
        numberOfKeys = m_numberOfKeys;
        return m_scanKeyForCheck;
    }

    RetStatus ResetArrCondInfo(int numKeys, Datum **values, bool **isnulls, int *numElem);

    void MarkPosition();
    void RestorePosition();

#ifdef UT
    void GetProcessedScanKeyInfo(bool &keysConflictFlag, int &numberOfKeys, int &numArrayKeys, ScanKey &skey,
                                 bool &isCrExtend);
#endif

protected:
    RetStatus SearchBtree(BufferDesc **leafBuf, bool strictlyGreaterThanKey, bool forceUpdate = false,
                          bool needWriteLock = false, bool needCheckCreatedXid = false) final;
    RetStatus StepRightIfNeeded(BufferDesc **pageBuf, LWLockMode access, bool strictlyGreaterThanKey,
                                bool needCheckCreatedXid = false) final;

private:
    bool m_anyValidScanKey;
    bool m_keysConflictFlag;
    int m_numberOfKeys; /* number of m_scanKeyForCheck */
    int m_numberOfKeysIncludeArr; /* number of m_scanKeyIncludeArry */
    int m_numArrCond; /* number of IN/ANY array condition in where clause */
    bool m_showAnyTuples;
    BtrArrayCondInfo *m_arrCondInfo;

    bool m_checkTupleMatchFastFlag;
    bool m_uniqueTupleSearch;
    Datum m_fastcheckKeys[INDEX_MAX_KEY_NUM];

    BtrScanContext  m_currentStatus;
    ScanKey m_scanKeyForCheck;     /* used for check keys on leaf scan. */
    ScanKey m_scanKeyIncludeArry; /* scanKeys that array sankey not process */
    SnapshotData m_snapshot;
    int m_markItemIndex;
    BtrScanContext m_markStatus;

    void InitScanContext()
    {
        m_currentStatus.baseBufDesc = INVALID_BUFFER_DESC;
        m_currentStatus.crBufDesc = INVALID_BUFFER_DESC;
        m_currentStatus.currPage = INVALID_PAGE_ID;
        m_currentStatus.nextPage = INVALID_PAGE_ID;
        m_currentStatus.lastUsedInfo = {false, INVALID_PAGE_ID, INVALID_PAGE_ID};
        m_currentStatus.prevMatchedHeapCtid = INVALID_ITEM_POINTER;
        m_currentStatus.prevMatchedTableOid = DSTORE_INVALID_OID;
        m_currentStatus.moreLeft = false;
        m_currentStatus.moreRight = false;
        m_currentStatus.firstItem = 0;
        m_currentStatus.lastItem = 0;
        m_currentStatus.itemIndex = 0;
        m_currentStatus.matchInsertXids = nullptr;
        m_currentStatus.matchDeleteXids = nullptr;
        /* matchItems is initialized in hear, which is protected by itemIndex. */
    }

    inline BtrPage *GetCurrCrPage()
    {
        StorageAssert(m_currentStatus.currPage.IsValid());
        return (m_currentStatus.crBufDesc != INVALID_BUFFER_DESC) ?
            static_cast<BtrPage *>(m_currentStatus.crBufDesc->GetPage()) : m_currentStatus.localCrPage;
    }

    /* Process the scankey passed by outside. */
    void ProcessScanKey(ScanKey inkeys, int inkeysNum, ScanKey outkeys, int &outkeysNum, bool checkArrKey);
    bool CollectScanKeyForOneAttr(ScanKey cur, ScanKey (&stratBucket)[MAX_STRATEGY_NUM]);
    void MergeEqualStratScanKeyForOneAttr(ScanKey (&stratBucket)[MAX_STRATEGY_NUM]);
    void MergeScanKeyForOneAttr(ScanKey (&stratBucket)[MAX_STRATEGY_NUM]);
    void SaveValidScanKey(ScanKey skey, ScanKey outkeys, bool markStop) const;
    void ProcecssScanKeyForOneAttr(ScanKey (&stratBucket)[MAX_STRATEGY_NUM], const ScanKey &outkeys,
                                   int &newNumberOfKeys, int &numberOfEqualCols, bool markStop);
    void ProcessSingleScanKey(ScanKey skey, ScanKey outkeys, int16 *indoption);
    bool TransferScanKeyStrategy(ScanKey skey, int16 *indoption) const;
    void MarkScanKeyPossibleStopScan(ScanKey skey) const;
    bool CompareScankeyArgs(ScanKey leftarg, ScanKey rightarg, ScanKey operatorKey, bool *result) const;
    void CompareScankeyArgsHavingNulls(ScanKey leftarg, ScanKey rightarg, ScanKey operatorKey, bool *result) const;
    bool TryMakeFastCheckKeys(ScanKey outkeys, int keysNum);
    ScanKey SelectScanFuncForArgs(Oid leftType, Oid rightType, ScanKey operatorKey, bool &needFreeKey) const;

    /* Process IN/ANY array conditions */
    RetStatus InitArrayCondition();
    Datum FindExtremeArrayElement(ScanKey skey, Datum *values, int nElems) const;
    RetStatus RearrangeArrayKeysAndDedup(BtrArrayCondInfo *curArrKeyInfo, TuplesortMgr *sortMgr, ScanKey checkingKey);
    RetStatus GetFirstArrCondition(ScanDirection dir);
    bool StepToNextArrCondition(ScanDirection dir);
    int MakeArrPositioningKey(ScanKey skey, int arrKeyIdx, int checkingKeyIdx);
    /* Make the scankey to descend to leaf. */
    bool MakePositioningKeys(ScanDirection dir);
    bool ConstructNotNullScanKey(ScanKey notNullKey, ScanKey normalKey, ScanDirection dir) const;
    bool TryMakeFastCompareKeys();
    void CleanLastUsedInfo();

    /* The main flow of btree scan. */
    RetStatus GetNextTupleInternal(IndexScanDesc scan, ScanDirection dir, bool *found);
    RetStatus SearchBtreeFromInternalPage(BufferDesc *parentBuf, BufferDesc **leafBuf, bool strictlyGreaterThanKey);
    RetStatus SearchFromLastAccessedPage(bool strictlyGreaterThanKey);
    RetStatus DescendToLeaf(ScanDirection dir);
    RetStatus DownToEdge(ScanDirection dir, OffsetNumber *offnum);
    RetStatus DownToLoc(ScanDirection dir, OffsetNumber *offnum);
    inline void InitializeScanDirection(ScanDirection dir);
    RetStatus ScanOnLeaf(ScanDirection dir, bool *hasMore);
    RetStatus WalkRightOnLeaf(bool *hasMore);
    RetStatus WalkLeftOnLeaf(bool *hasMore);
    RetStatus FindNextLeftPage(bool &hasLeft);
    RetStatus MakeLeafCrPage();
    bool FilterOutMatchTuple(ScanDirection dir, OffsetNumber offnum);
    void MarkContinueScan(BtrPage* page, bool *continuescan, ScanDirection dir);
    bool NeedContinueScan(ScanKey skey, bool attrIsNull, ScanDirection dir) const;
    bool CheckTuple(IndexTuple *tuple, uint16 tupnatts, ScanDirection dir, bool *continuescan);
    Xid GetItupInsertXidFromUndo(BtrPage *page, IndexTuple *tuple, uint8 insertTdId);
    Xid GetItupDeleteXidFromUndo(BtrPage *page, IndexTuple *tuple, uint8 &insertTdId);
    void GetItupInsertAndDeleteXids(BtrPage* page, IndexTuple *tuple, Xid &insertXid, Xid &deleteXid);
    bool SaveItupIfNeeded(int itemIndex, BtrPage *page, OffsetNumber offnum, ScanDirection dir, bool &continuescan);
    inline bool CheckTupleFast(IndexTuple *tuple, int tupnatts, bool *continuescan) const;
    inline bool HasCachedResults(ScanDirection dir) const;
    inline bool IsScanPosValid() const;
    inline void UnpinScanPosIfPinned();
    inline void InvalidateScanPos();
    inline void MakePositioningKeysFillFmgrInfo(ScanKey &chosen, AttrNumber curAttr, FmgrInfo &fmgrInfo);
    void CopyCurToMarkBeforeStepPage();
    bool CheckCommittedTuple(BtrPage* page, IndexTuple *itup) const;
    bool IsUniqueTupleSearch();
};
}

#endif /* SRC_GAUSSKERNEL_INCLUDE_INDEX_STORAGE_BTREE_SCAN_H_  */
