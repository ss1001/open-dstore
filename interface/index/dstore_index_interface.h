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
 * dstore_index_interface.h
 *
 * IDENTIFICATION
 *        dstore/interface/index/dstore_index_interface.h
 *
 * ---------------------------------------------------------------------------------------
 */
#ifndef DSTORE_INDEX_INTERFACE_H
#define DSTORE_INDEX_INTERFACE_H

#include "catalog/dstore_catalog_struct.h"
#include "index/dstore_scankey.h"
#include "index/dstore_index_struct.h"
#include "page/dstore_itemptr.h"
#include "tuple/dstore_tuple_struct.h"
#include "transaction/dstore_transaction_struct.h"
#include "systable/dstore_relation.h"

namespace DSTORE {

class IndexScanHandler;
class CcindexBtrBuildHandler;
struct IndexTuple;
struct BtreeInsertAndDeleteCommonData {
    StorageRelation indexRel;
    IndexInfo *indexInfo;
    ScanKey skey;
    Datum *values;
    bool *isnull;
    ItemPointer heapCtid;
    CommandId cid;
};

void ReportIndexBufferReadStat(DSTORE::StorageRelation relation);
}  // namespace DSTORE

namespace IndexInterface {
#pragma GCC visibility push(default)
DSTORE::RetStatus Build(DSTORE::StorageRelation indexRel, DSTORE::ScanKey skey, DSTORE::IndexBuildInfo *indexBuildInfo);
DSTORE::RetStatus BuildParallel(DSTORE::StorageRelation indexRel, DSTORE::ScanKey skey,
                                DSTORE::IndexBuildInfo *indexBuildInfo, int parallelWorkers);
DSTORE::RetStatus Insert(DSTORE::BtreeInsertAndDeleteCommonData commonData, bool isCatalog, bool checkImmediate = true,
                         bool *satisfiesUnique = nullptr);
DSTORE::RetStatus Delete(DSTORE::BtreeInsertAndDeleteCommonData commonData);
DSTORE::IndexScanHandler *ScanBegin(DSTORE::StorageRelation indexRel, DSTORE::IndexInfo *indexInfo,
                                    int numKeys, int numOrderbys, bool showAnyTuples = false);
DSTORE::RetStatus ScanRescan(DSTORE::IndexScanHandler *scanHandler, DSTORE::ScanKey skey);
DSTORE::RetStatus PointGetUnique(DSTORE::StorageRelation indexRel, DSTORE::IndexInfo *indexInfo, DSTORE::ScanKey skey,
                                 DSTORE::Snapshot snapshot, DSTORE::ItemPointerData *heapCtid);
DSTORE::ScanKey GetScanKeyInfo(DSTORE::IndexScanHandler *scanHandler, int &numberOfKeys);
DSTORE::RetStatus ResetArrCondInfo(DSTORE::IndexScanHandler *scanHandler, int numKeys, DSTORE::Datum **values,
                                   bool **isnulls, int *numElem);
DSTORE::RetStatus ScanNext(DSTORE::IndexScanHandler *scanHandler, DSTORE::ScanDirection direction,
                           bool *found, bool *recheck);
DSTORE::ItemPointer GetResultHeapCtid(DSTORE::IndexScanHandler *scanHandler);
DSTORE::IndexTuple *IndexScanGetIndexTuple(DSTORE::IndexScanHandler *scanHandler);
DSTORE::TupleDesc IndexScanGetTupleDesc(DSTORE::IndexScanHandler *scanHandler);
DSTORE::RetStatus ScanSetWantItup(DSTORE::IndexScanHandler *scanHandler, bool wantItup);
void IndexScanSetSnapshot(DSTORE::IndexScanHandler *scanHandler,  DSTORE::Snapshot snapshot);
void IndexScanGetInsertAndDeleteXids(DSTORE::IndexScanHandler *scanHandler,
                                     DSTORE::Datum &insertXid, DSTORE::Datum &deleteXid);
DSTORE::IndexTuple *OnlyScanNext(DSTORE::IndexScanHandler *scanHandler, DSTORE::ScanDirection direction,
                                 DSTORE::TupleDesc *tupdesc, bool *recheck);
DSTORE::RetStatus ScanEnd(DSTORE::IndexScanHandler *scanHandler);
DSTORE::Oid GPIGetScanHeapPartOid(DSTORE::IndexScanHandler *scanHandler);
DSTORE::RetStatus BtreeLazyVacuum(DSTORE::StorageRelation indexRel, DSTORE::IndexInfo *indexInfo);
DSTORE::RetStatus BtreeGPIVacuum(DSTORE::StorageRelation indexRel, DSTORE::GPIPartOidCheckInfo *gpiCheckInfo);

void UpdateCcindexBtrBuildHandler(DSTORE::CcindexBtrBuildHandler *handler, DSTORE::StorageRelation indexRel,
                                  DSTORE::IndexBuildInfo *indexBuildInfo);
DSTORE::CcindexBtrBuildHandler *CreateCcindexBuildHandler(DSTORE::StorageRelation indexRel, DSTORE::ScanKey skey,
                                                          DSTORE::IndexBuildInfo *indexBuildInfo,
                                                          bool isLpi, bool isRebuild);
DSTORE::RetStatus WaitForTrxVisibleForAll(
    DSTORE::PdbId pdbId, DSTORE::CcindexBtrBuildHandler *ccindexBtree, uint64_t xid);
DSTORE::RetStatus BuildBtreeForCcindex(DSTORE::CcindexBtrBuildHandler *ccindexBtree);
DSTORE::RetStatus UpdateDeltaDmlForCcindex(DSTORE::BtreeInsertAndDeleteCommonData commonData,
                                           DSTORE::ItemPointer duplicateDeltaRec, bool isLpi, int retryTimes);
DSTORE::RetStatus MergeDeltaDmlForCcindex(DSTORE::CcindexBtrBuildHandler *ccindexBtree,
                                          DSTORE::StorageRelation deltaDmlIdxRel,
                                          DSTORE::IndexInfo *deltaDmlIndexInfo);
DSTORE::RetStatus DestroyCcindexBuildHandler(DSTORE::CcindexBtrBuildHandler *ccindexBtree);
DSTORE::RetStatus CheckExistence(DSTORE::BtreeInsertAndDeleteCommonData commonData, bool isLpi,
                                 DSTORE::ItemPointer deltaRec);

void DumpScanPage(DSTORE::IndexScanHandler *scanHandler, DSTORE::Datum &fileId, DSTORE::Datum &blockId,
                  DSTORE::Datum &data);
DSTORE::RetStatus MarkPosition(DSTORE::IndexScanHandler *scanHandler);
DSTORE::RetStatus RestorePosition(DSTORE::IndexScanHandler *scanHandler);

#pragma GCC visibility pop
}  // namespace IndexInterface
#endif
