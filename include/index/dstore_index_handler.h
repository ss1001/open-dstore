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

#ifndef SRC_GAUSSKERNEL_INCLUDE_INDEX_DSTORE_INDEX_HANDLER_H
#define SRC_GAUSSKERNEL_INCLUDE_INDEX_DSTORE_INDEX_HANDLER_H

#include "index/dstore_btree_scan.h"

namespace DSTORE {

/*
IndexScanDescData is only included in Indexhandler in dstore,
which is used to:
1. store all scan description imfomation from executor, but only a subset of them is used in(supported by) btree scan.
2. defiend the results which Indexhandler should return.
*/
struct IndexScanDescData {
    int32 numberOfKeys;             /* number of index qualifier conditions */
    int32 numberOfOrderBys;         /* number of ordering operators */

    ScanKey keyData;                /* array of index qualifier descriptors */

    bool wantItup;                  /* caller requests index tuples */

    bool wantPartOid;               /* global partition index need partition oid */
    bool needRecheck;               /* T means scan keys must be rechecked */

    IndexTuple *itup;               /* index tuple returned by AM */
    TupleDesc itupDesc;             /* rowtype descriptor of xs_itup */
    ItemPointerData heapCtid;       /* result */
    Oid currPartOid;                /* result */
    bool wantXid;                   /* T means we are in show any tuples mode */
    Xid insertXid;                  /* insert xid of the returned index tuple iff in show any tuples mode */
    Xid deleteXid;                  /* delete xid of the returned index tuple iff in show any tuples mode */
};

class IndexScanHandler : public BaseObject {
public:
    IndexScanHandler();
    IndexScanHandler(const IndexScanHandler &) = delete;
    IndexScanHandler &operator=(const IndexScanHandler &) = delete;
    ~IndexScanHandler();

    RetStatus InitIndexScanHandler(StorageRelation indexRel, IndexInfo* indexInfo, int numKeys, int numOrderbys,
        bool showAnyTuples = false);
    RetStatus BeginScan();
    RetStatus PointGetUnique(ScanKey skey, ItemPointerData *heapCtid);

    RetStatus ReScan(ScanKey skey);

    void EndScan();

    RetStatus GetNextTuple(ScanDirection dir, bool *found);

    inline ItemPointer GetResultHeapCtid()
    {
        return &(m_desc.heapCtid);
    }

    inline IndexScanDesc GetScanDesc()
    {
        return &m_desc;
    }

    inline PdbId GetPdbId()
    {
        return m_pdbId;
    }

    static IndexScanHandler *Create(StorageRelation indexRel, IndexInfo* indexInfo, int numKeys, int numOrderbys,
        bool showAnyTuples = false)
    {
        IndexScanHandler *scanHandler =
            DstoreNew(g_dstoreCurrentMemoryContext) IndexScanHandler();
        if (unlikely(scanHandler == nullptr)) {
            storage_set_error(INDEX_ERROR_MEMORY_ALLOC);
            return nullptr;
        }

        if (STORAGE_FUNC_FAIL(scanHandler->InitIndexScanHandler(indexRel, indexInfo, numKeys, numOrderbys,
            showAnyTuples))) {
            DstorePfreeExt(scanHandler);
            return nullptr;
        }
        return scanHandler;
    }

    ScanKey GetScanKeyInfo(int &numberOfKeys)
    {
        return m_scan->GetScanKeyInfo(numberOfKeys);
    }

    RetStatus ResetArrCondInfo(int numKeys, Datum **values, bool **isnulls, int *numElem)
    {
        return m_scan->ResetArrCondInfo(numKeys, values, isnulls, numElem);
    }

    Oid GetPartHeapOid() const
    {
        return m_desc.wantPartOid ? m_desc.currPartOid : DSTORE_INVALID_OID;
    }

    Oid GetStorageRelOid();

    void SetStorageRelOid(Oid relOid);

    RetStatus SetShowAnyTuples(bool showAnyTuples);

    void InitSnapshot(Snapshot snapshot);

    void DumpScanPage(Datum &fileId, Datum &blockId, Datum &data);

    void MarkPosition();

    void RestorePosition();
#ifdef UT
    void GetScanStatus(bool &keysConflictFlag, int &numberOfKeys, int &numArrayKeys, ScanKey &skey, bool &isCrExtend);
#endif
    IndexScanDescData m_desc;        /* save parameters which passed into the interface */
    Oid m_storageRelOid;
    PdbId m_pdbId;
    BtreeScan *m_scan;           /* btree scan internal */
};

}
#endif
