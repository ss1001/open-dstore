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

#include "framework/dstore_instance.h"
#include "buffer/dstore_buf_lru.h"
#include "common/log/dstore_log.h"
#include "errorcode/dstore_buf_error_code.h"
#include "buffer/dstore_buf_perf_unit.h"
namespace DSTORE {

namespace {
    const uint32 LRU_MAX_USAGE = 5u;
    const double LRU_SCAN_COUNT = 0.5;
}

BufLruList::BufLruList(uint32 idx, Size hotListSize)
    : mHotList(hotListSize == 0 ? 1 : hotListSize), mLruList(), mCandidateList(),
      mInvalidationList(LN_TO_BE_INVALIDATED), mExpandLwlock{}, mIndex{idx}
{}

void BufLruList::Initialize()
{
    mHotList.Initialize();
    mLruList.Initialize();
    mCandidateList.Initialize();
    mInvalidationList.Initialize();
    LWLockInitialize(&mExpandLwlock, LWLOCK_GROUP_BUF_LRU_EXPAND);
}

/*
 * Return a candidate buffer entry from candidate list and the PageId is not equal to page_id.
 * The buffer must only be pinned by ourselves and not in the Lru list and Hot list.
 * If the candididate list is empty, try to move free entry from lru list to candidate list.
 * Before return, we should:
 * (1) pin the buffer;
 * (2) hold the buffer head lock;
 */
BufferDesc *BufLruList::GetCandidateBuffer(const BufferTag &bufTag, bool ignoreDirtyPage)
{
    LruNode *lruNode = nullptr;
    BufferDesc *candidateBufferDesc = nullptr;
    static const Size maxRetryTimes = 8;

    for (Size i = 0; i < maxRetryTimes; i++) {
        /* try to pop an entry from candidate list */
        lruNode = mCandidateList.Pop();
        /* if the candidate list is not empty, check the state of node */
        if (lruNode != nullptr) {
            StorageAssert(lruNode->IsInPendingState());
            StorageAssert(lruNode->lruIndex == mIndex);
            candidateBufferDesc = lruNode->GetValue<BufferDesc>();
            /*
             * Set `need_dirty_buffer` to true to circumvent dirty buffer check inside the method, although there
             * shouldn't be any dirty pages in candidate list.
             */
            if (candidateBufferDesc->FastLockHdrIfReusable(bufTag, false, ignoreDirtyPage)) {
                candidateBufferDesc->PinUnderHdrLocked();
                return candidateBufferDesc;
            }

            mLruList.LockList();
            mLruList.AddTail(lruNode);
            mLruList.UnlockList();
            continue;
        }

        /* candidate is empty, scan lru list to get candidate buffer */
        candidateBufferDesc = ScanLruListToFindCandidateBuffer(bufTag, mLruList.Length(),
            ignoreDirtyPage);
        if (candidateBufferDesc != INVALID_BUFFER_DESC) {
            StorageAssert(candidateBufferDesc->lruNode.IsInPendingState());
            StorageAssert(candidateBufferDesc->lruNode.lruIndex == mIndex);
            return candidateBufferDesc;
        }
    }

    ErrLog(DSTORE_WARNING, MODULE_BUFMGR,
        ErrMsg("BufLruList[%u] don't have available buffer now, try next list.", mIndex));
    return INVALID_BUFFER_DESC;
}

BufferDesc *BufLruList::ScanLruListToFindCandidateBuffer(const BufferTag &bufTag, Size expectScanSize,
    bool ignoreDirtyPage)
{
    BufferDesc* candidateBufferDesc = INVALID_BUFFER_DESC;
    if (!mCandidateList.IsEmpty()) {
        return INVALID_BUFFER_DESC;
    }

    Size scanSize = mLruList.Length();
    if (scanSize > expectScanSize) {
        scanSize = expectScanSize;
    }

    LruNode *lruNode = nullptr;
    for (Size i = 0; i != scanSize; ++i) {
        /*
         * If the bufferpool has shrunk before, the hot list may contain more buffers than the max size
         * and needs a cleanup.
         */
        if (mHotList.Length() > mHotList.GetMaxSize()) {
            TryMoveOneNodeFromHotToLruList();
        }

        mLruList.LockList();
        lruNode = mLruList.PopTail();
        if (lruNode == nullptr) {
            /* The LRU list could be drained out in some extreme scenario. */
            mLruList.UnlockList();
            continue;
        }

        candidateBufferDesc = lruNode->GetValue<BufferDesc>();
        if (candidateBufferDesc->FastLockHdrIfReusable(bufTag, false, ignoreDirtyPage)) {
            mLruList.UnlockList();
            candidateBufferDesc->PinUnderHdrLocked();
            return candidateBufferDesc;
        }

        /* If we failed to reuse the buffer, it's most likely still being used. Then we move it to the list head. */
        mLruList.AddHead(lruNode);
        mLruList.UnlockList();
    }

    return INVALID_BUFFER_DESC;
}

void BufLruList::BufferAccessStat(BufferDesc *bufferDesc)
{
RETRY:
    StorageAssert(bufferDesc->lruNode.lruIndex == mIndex);
    LruNode* lruNode = &bufferDesc->lruNode;
    /* The node is in pending state means other thread is moving the node now, just return and no need to process. */
    if (lruNode->IsInPendingState()) {
        return;
    }

    /* The node is in hot queue, no need to move node. */
    if (lruNode->IsInHotList()) {
        return;
    }

    /* The node is in candidate list, move the buffer to the head of the lru list. */
    if (lruNode->IsInCandidateList()) {
        if (mCandidateList.Remove(lruNode)) {
            StorageAssert(lruNode->IsInPendingState());
            mLruList.LockList();
            lruNode->ResetUsage();
            static_cast<void>(lruNode->IncUsage());
            mLruList.AddHead(lruNode);
            mLruList.UnlockList();
            return;
        }
        /* other thread has removed the buffer from candidate list, just retry */
        goto RETRY;
    }
    if (!lruNode->IsInLruList()) {
        goto RETRY;
    }
    if (lruNode->IncUsage() < (LRU_MAX_USAGE / 2)) {
        return;
    }

    uint32 usage = lruNode->GetUsage();
    /* node not reach LRU_MAX_USAGE, move head */
    if ((usage < LRU_MAX_USAGE) && (usage > (LRU_MAX_USAGE / 2))) {
        mLruList.LockList();
        if (lruNode->IsInLruList()) {
            mLruList.MoveHead(lruNode);
        } else {
            mLruList.UnlockList();
            goto RETRY;
        }
        mLruList.UnlockList();
    } else if (usage >= LRU_MAX_USAGE) {
        /* node reach LRU_MAX_USAGE, pop it and push into hot chain */
        mLruList.LockList();
        if (lruNode->IsInLruList()) {
            mLruList.Remove(lruNode);
        } else {
            mLruList.UnlockList();
            goto RETRY;
        }
        mLruList.UnlockList();

        /* If the hot list is full, pop the coldest buffer from it to make a spot. */
        while (!mHotList.Push(lruNode)) {
            TryMoveOneNodeFromHotToLruList();
        }
    }

    return;
}

/*
 * If the buffer is in lru list and successfully removed, return true.
 * Otherwise return false.
 */
bool BufLruList::TryRemoveFromLruList(BufferDesc *bufferDesc)
{
    if (bufferDesc->lruNode.IsInLruList()) {
        mLruList.LockList();
        /* Node is not in Lru list now, just retry */
        if (!bufferDesc->lruNode.IsInLruList()) {
            mLruList.UnlockList();
            return false;
        }
        mLruList.Remove(&bufferDesc->lruNode);
        StorageAssert(bufferDesc->lruNode.IsInPendingState());
        StorageAssert(bufferDesc->lruNode.GetUsage() == 0);
        mLruList.UnlockList();
        return true;
    }
    return false;
}

void BufLruList::MoveToCandidateList(BufferDesc *bufferDesc)
{
    for (;;) {
        StorageAssert(bufferDesc->lruNode.lruIndex == mIndex);
        /*
         * buffer is already in candidate list.
         */
        if (bufferDesc->lruNode.IsInCandidateList()) {
            return;
        }

        /*
         * other thread is moving this node, we should wait it finish.
         */
        if (bufferDesc->lruNode.IsInPendingState()) {
            continue;
        }

        if (bufferDesc->lruNode.IsInHotList()) {
            if (mHotList.Remove(&bufferDesc->lruNode)) {
                StorageAssert(bufferDesc->lruNode.IsInPendingState());
                break;
            }
            continue;
        }

        if (TryRemoveFromLruList(bufferDesc)) {
            break;
        }
    }
    StorageAssert(bufferDesc->lruNode.lruIndex == mIndex);
    mCandidateList.Push(&bufferDesc->lruNode);
}

void BufLruList::TryMoveOneNodeFromHotToLruList()
{
    LruNode *lruNode = mHotList.Pop();
    /* Do nothing if mHotList is empty. */
    if (lruNode != nullptr) {
        mLruList.LockList();
        mLruList.AddHead(lruNode);
        mLruList.UnlockList();
    }
}

void BufLruList::PushBackToLru(BufferDesc *bufferDesc, bool reuseSuccess)
{
    StorageAssert(bufferDesc->lruNode.IsInPendingState());
    StorageAssert(bufferDesc->lruNode.lruIndex == mIndex);
    bufferDesc->lruNode.ResetUsage();

    mLruList.LockList();
    if (reuseSuccess) {
        static_cast<void>(bufferDesc->lruNode.IncUsage());
        mLruList.AddHead(&bufferDesc->lruNode);
    } else {
        mLruList.AddHead(&bufferDesc->lruNode);
    }
    mLruList.UnlockList();
}

void BufLruList::PushBackToCandidate(BufferDesc *bufferDesc)
{
    StorageAssert(bufferDesc->lruNode.IsInPendingState());
    StorageAssert(bufferDesc->lruNode.lruIndex == mIndex);
    mCandidateList.Push(&bufferDesc->lruNode);
}

/*
 * Append buffer descriptor(s) to candidate list.
 */
void BufLruList::AddNewBuffer(BufferDesc *bufferDesc, uint32 numBufDesc, bool isInit)
{
    if (!isInit) {
        DstoreLWLockAcquire(&mExpandLwlock, LW_EXCLUSIVE);
    }

    for (uint32 i = 0; i < numBufDesc; i++) {
        StorageAssert(bufferDesc[i].lruNode.IsInPendingState());
        bufferDesc[i].lruNode.lruIndex = mIndex;
        mCandidateList.Push(&bufferDesc[i].lruNode);
    }
    if (!isInit) {
        LWLockRelease(&mExpandLwlock);
    }
}

/*
 * Remove a buffer from candidate/LRU/hot lists and return the buffer's previous LruNodeType.
 */
LruNodeType BufLruList::Remove(BufferDesc *bufferDesc)
{
    for (;;) {
        StorageAssert(bufferDesc->lruNode.lruIndex == mIndex);
        /*
         * buffer is already in candidate list.
         */
        if (bufferDesc->lruNode.IsInCandidateList()) {
            if (mCandidateList.Remove(&bufferDesc->lruNode)) {
                StorageAssert(bufferDesc->lruNode.IsInPendingState());
                return LN_CANDIDATE;
            }
            continue;
        }

        /*
         * other thread is moving this node, we should wait it finish.
         */
        if (bufferDesc->lruNode.IsInPendingState()) {
            continue;
        }

        if (bufferDesc->lruNode.IsInHotList()) {
            if (mHotList.Remove(&bufferDesc->lruNode)) {
                StorageAssert(bufferDesc->lruNode.IsInPendingState());
                return LN_HOT;
            }
            continue;
        }

        if (TryRemoveFromLruList(bufferDesc)) {
            return LN_LRU;
        }
    }
    return LN_PENDING;
}

char* BufLruList::DumpSummaryInfo()
{
    StringInfoData dumpInfo;
    if (unlikely(!dumpInfo.init())) {
        ErrLog(DSTORE_ERROR, MODULE_BUFMGR, ErrMsg("cannot allocate memory for LruList dump info."));
        return nullptr;
    }

    Size hotSize = mHotList.Length();
    Size lruSize = mLruList.Length();
    Size candidateSize = mCandidateList.Length();
    Size dirtyCnt = 0;

    mLruList.LockList();

    ConcurrentDList::ReverseIterator r_iter = mLruList.RBegin();
    BufferDesc *buffer;
    uint64 state;
    while (r_iter != mLruList.REnd()) {
        buffer = BufLruList::GetNode(*r_iter)->GetValue<BufferDesc>();
        state = buffer->GetState(false); /* It's okay dirty read here. */
        if (state & Buffer::BUF_CONTENT_DIRTY) {
            dirtyCnt++;
        }
        ++r_iter;
    }

    mLruList.UnlockList();

    dumpInfo.append("Index:%3u hot size:%10lu lru size:%10lu lru dirty size:%10lu candidate:%10lu",
                    mIndex, hotSize, lruSize, dirtyCnt, candidateSize);

    return dumpInfo.data;
}

BufLruListArray::BufLruListArray(Size lruPartition)
    : m_buf_lru_list{nullptr}, m_lru_partitions{lruPartition}, m_lruIndex(0)
{}

void BufLruListArray::Destroy()
{
    for (uint32 i = 0; i < m_lru_partitions; i++) {
        delete m_buf_lru_list[i];
    }

    DstorePfree(m_buf_lru_list);
}

/*
 * Update the size of hot list for each LRU partition to be proportional to the bufferpool size
 * which makes sure hot lists do not take up the entire bufferpool or drain out the LRU lists.
 *
 * For bufferpool expansion, expand the hot lists after new candidate buffers are added.
 *      The hot lists will be automatically filled up by running workload.
 * For bufferpool shrink, shrink the hot lists before invalidating buffers.
 *      The buffers in hot lists are temporarily immune from being reused/evicted. After shrinking, the hot lists
 *      contain more buffers than expected and there's not enough buffers outside of hot lists for reuse.
 *      When the next buffer request comes in, GetCandidateBuffer() detects whether the hot lists need a cleanup
 *      and will move the coldest buffers back to LRU lists.
 */
void BufLruListArray::ResizeHotList(Size bufferPoolSize)
{
    Size newHotListSize = static_cast<Size>((static_cast<float>(bufferPoolSize) /
                                            static_cast<float>(m_lru_partitions)) * BUFLRU_DEFAULT_HOT_RATIO);
    for (Size i = 0; i < m_lru_partitions; i++) {
        LruHotList *hotList = m_buf_lru_list[i]->GetHotList();
        ErrLog(DSTORE_WARNING, MODULE_BUFMGR,
            ErrMsg("Resize LRU hot list from %lu to %lu for lru partition = %lu\n",
                hotList->GetMaxSize(), newHotListSize, i));
        hotList->SetMaxSize(newHotListSize);
        while (hotList->Length() > hotList->GetMaxSize()) {
            m_buf_lru_list[i]->TryMoveOneNodeFromHotToLruList();
        }
    }
}

uint32 BufLruListArray::FindBufLruList(const BufferTag *bufTag) const
{
    uint32 hashCode = tag_hash(static_cast<const void *>(bufTag), sizeof(BufferTag));
    return hashCode % m_lru_partitions;
}

BufferDesc *BufLruListArray::GetCandidateBuffer(const BufferTag *bufTag, uint64 *bufState)
{
    /* find the corresponding lru list by PageId */
    Size lruIdx = FindBufLruList(bufTag);
    BufLruList *bufLru = m_buf_lru_list[lruIdx];

    BufferDesc* candidateBufferDesc = bufLru->GetCandidateBuffer(*bufTag, false);
    if (candidateBufferDesc != INVALID_BUFFER_DESC) {
        *bufState = GsAtomicReadU64(&candidateBufferDesc->state);
        StorageAssert((*bufState & Buffer::BUF_LOCKED) != 0);
        candidateBufferDesc->UnlockHdr(*bufState);
        StorageAssert(candidateBufferDesc->lruNode.IsInPendingState());
        return candidateBufferDesc;
    }

    /* try to find candidate buffer from other lru list */
    Size nextLruIdx = (lruIdx + 1) % m_lru_partitions;
    while (nextLruIdx != lruIdx) {
        bufLru = m_buf_lru_list[nextLruIdx];
        candidateBufferDesc = bufLru->GetCandidateBuffer(*bufTag, false);
        if (candidateBufferDesc != INVALID_BUFFER_DESC) {
            *bufState = GsAtomicReadU64(&candidateBufferDesc->state);
            StorageAssert((*bufState & Buffer::BUF_LOCKED) != 0);
            candidateBufferDesc->UnlockHdr(*bufState);
            return candidateBufferDesc;
        }
        nextLruIdx = (nextLruIdx + 1) % m_lru_partitions;
    }

    /* Oops, no available buffer to reuse */
    ErrLog(DSTORE_WARNING, MODULE_BUFMGR,
        ErrMsg("Can not find free buffer after scan all BufLruList partitions"));
    storage_set_error(BUFFER_WARNING_NO_AVAILABLE_BUFFER_CAN_USE);
    return INVALID_BUFFER_DESC;
}

void BufLruListArray::PushBufferBackToLru(BufferDesc *bufferDesc, bool reuseSuccess)
{
    Size lruIdx = bufferDesc->lruNode.lruIndex;
    BufLruList *bufLru = m_buf_lru_list[lruIdx];

    bufLru->PushBackToLru(bufferDesc, reuseSuccess);
}

void BufLruListArray::PushBufferBackToCandidate(BufferDesc *bufferDesc)
{
    uint32 lruIdx = bufferDesc->lruNode.lruIndex;
    BufLruList *bufLru = m_buf_lru_list[lruIdx];

    bufLru->PushBackToCandidate(bufferDesc);
}

void BufLruListArray::BufferAccessStat(BufferDesc *bufferDesc)
{
    StorageAssert(!bufferDesc->bufTag.IsInvalid());
    Size lruIdx = bufferDesc->lruNode.lruIndex;
    BufLruList *bufLru = m_buf_lru_list[lruIdx];

    bufLru->BufferAccessStat(bufferDesc);
}

void BufLruListArray::MoveToCandidateList(BufferDesc *bufferDesc)
{
    Size lruIdx = bufferDesc->lruNode.lruIndex;
    BufLruList *bufLru = m_buf_lru_list[lruIdx];

    bufLru->MoveToCandidateList(bufferDesc);
}

void BufLruListArray::Remove(BufferDesc *bufferDesc)
{
    uint32 lruIdx = bufferDesc->lruNode.lruIndex;
    BufLruList *bufLru = m_buf_lru_list[lruIdx];

    static_cast<void>(bufLru->Remove(bufferDesc));
}

/*
 * Remove all buffers in this memchunk from candidate list, LRU list or hot list, and selectively add them
 * to invalidation list to let the invalidation workers invalidate each buffer when no one is using it.
 *
 * If the buffer was in candidate list, we assume it is never used and can be simply removed.
 * If the buffer was in LRU list or hot list, we move it to invalidation list.
 */
void BufLruListArray::RemoveMemChunkFromLru(BufferMemChunk *memChunk)
{
    StorageAssert(memChunk != nullptr);
    ErrLog(DSTORE_WARNING, MODULE_BUFMGR,
        ErrMsg("Move all buffers in memchunk #%lu to invalidation list.", memChunk->GetMemChunkId()));

#ifdef DSTORE_USE_ASSERT_CHECKING
    char bufferInfo[BUFFER_DESC_FORMAT_SIZE];
#endif
    memChunk->LockMemChunk(LW_SHARED);
    for (Size i = 0; i < memChunk->GetSize(); i++) {
        BufferDesc *bufferDesc = memChunk->GetBufferDesc(i);
#ifdef DSTORE_USE_ASSERT_CHECKING
        bufferDesc->PrintBufferDesc(bufferInfo, BUFFER_DESC_FORMAT_SIZE);
        ErrLog(DSTORE_DEBUG1, MODULE_BUFMGR, ErrMsg("Move buffer to invalidation list %s.", bufferInfo));
#endif

        LruNode *lruNode = &(bufferDesc->lruNode);
        BufLruList *bufLruList = GetLruListAt(lruNode->lruIndex);
        LruNodeType oldLruNodeType = lruNode->m_type.load();

        bufferDesc->Pin();
        StorageAssert(!lruNode->IsInPendingState());
        oldLruNodeType = bufLruList->Remove(bufferDesc);
        if (oldLruNodeType != LN_CANDIDATE) {
            bufLruList->GetInvalidationList()->PushTail(lruNode);
            StorageAssert(!lruNode->IsInPendingState());
        }
        bufferDesc->Unpin();
    }
    memChunk->UnlockMemChunk();
}

/*
 * Restore all buffers in this memchunk to LRU or candidate list.
 * This is the reverse operation for RemoveMemChunkFromLru, and it is called when bufferpool failed to shrink.
 *
 * If the buffer was not invalidated, it is in invalidation list, and it will be moved to LRU list.
 * If the buffer was invalidated, it is not in any list, and it will be added to candidate list.
 */
void BufLruListArray::RestoreMemChunkToLru(BufferMemChunk *memChunk)
{
    StorageAssert(memChunk != nullptr);
    ErrLog(DSTORE_WARNING, MODULE_BUFMGR,
        ErrMsg("Move all buffers in memchunk #%lu to LRU or candidate list.", memChunk->GetMemChunkId()));

    memChunk->LockMemChunk(LW_SHARED);
    for (Size i = 0; i < memChunk->GetSize(); i++) {
        BufferDesc *bufferDesc = memChunk->GetBufferDesc(i);
        LruNode *lruNode = &(bufferDesc->lruNode);
        BufLruList *bufLruList = GetLruListAt(lruNode->lruIndex);

        bufferDesc->Pin();
        if (lruNode->IsInInvalidationList()) {
            /* The buffer will be pushed to the tail of LRU list where it contains the coldest buffers. */
            static_cast<void>(bufLruList->GetInvalidationList()->Remove(lruNode));
            bufLruList->GetLruList()->AddTail(lruNode);
        } else {
            StorageAssert(lruNode->IsInPendingState());
            bufLruList->GetCandidateList()->Push(lruNode);
        }
        StorageAssert(!lruNode->IsInPendingState());
        bufferDesc->Unpin();
    }
    memChunk->UnlockMemChunk();
}

BufLruList *BufLruListArray::GetLruListAt(Size i)
{
    return m_buf_lru_list[i];
}

/*
 * append_cache_summary
 * This function formats and appends the given cache hit ratio info and lru summary
 * into the provided StringInfo cache_summary.
 *
 * It is a helper function for dump_cache_hit_ratio_info and dump_lru_summary.
 */
void AppendCacheSummary(StringInfo cacheSummary, const char* message, uint64 count, uint64 total)
{
    cacheSummary->append("# of %s = %lu", message, count);

    const float fullMarks = 100.0;

    if (total != 0) {
        cacheSummary->append(", %.2f", count * fullMarks / total);
        cacheSummary->AppendString("%");
    }
    cacheSummary->AppendString("\n");
}

/*
 * dump_lru_summary
 * Returns a StringInfo which contains BufLruListArray summary.
 */
StringInfo BufLruListArray::DumpLruSummary()
{
    StringInfo lruSummary = StringInfoData::make();
    if (STORAGE_VAR_NULL(lruSummary)) {
        return nullptr;
    }
    bool ret = lruSummary->init();
    if (ret == false) {
        DstorePfreeExt(lruSummary);
        return nullptr;
    }

    uint64 totalAddIntoHot = 0;
    uint64 totalRemoveFromHot = 0;
    uint64 totalAddIntoLru = 0;
    uint64 totalRemoveFromLru = 0;
    uint64 totalMoveWithinLru = 0;
    uint64 totalAddIntoCandidate = 0;
    uint64 totalRemoveFromCandidate = 0;
    uint64 totalMissInCandidate = 0;

    for (Size i = 0; i < m_lru_partitions; i++) {
        LruCounters hotCounters = GetLruListAt(i)->GetHotCounters();
        LruCounters lruCounters = GetLruListAt(i)->GetLruCounters();
        LruCounters candidateCounters = GetLruListAt(i)->GetCandidateCounters();

        /* Add the current count values of interest to the LRU summary. */
        totalAddIntoHot += hotCounters.addIntoList;
        totalRemoveFromHot += hotCounters.removeFromList;
        totalAddIntoLru += lruCounters.addIntoList;
        totalMoveWithinLru += lruCounters.moveWithinList;
        totalRemoveFromLru += lruCounters.removeFromList;
        totalAddIntoCandidate += candidateCounters.addIntoList;
        totalRemoveFromCandidate += candidateCounters.removeFromList;
        totalMissInCandidate += candidateCounters.missInList;
    }

    lruSummary->AppendString("LRU Partition Summary:\n");
    AppendCacheSummary(lruSummary, "add into hot list (total)", totalAddIntoHot);
    AppendCacheSummary(lruSummary, "remove out of hot list (total)", totalRemoveFromHot);
    lruSummary->AppendString("\n");
    AppendCacheSummary(lruSummary, "add into LRU list (total)", totalAddIntoLru);
    AppendCacheSummary(lruSummary, "move within LRU list (total)", totalMoveWithinLru);
    AppendCacheSummary(lruSummary, "remove out of LRU list (total)", totalRemoveFromLru);
    lruSummary->AppendString("\n");
    AppendCacheSummary(lruSummary, "add into candidate list (total)", totalAddIntoCandidate);
    AppendCacheSummary(lruSummary, "remove from candidate list (total)", totalRemoveFromCandidate);
    AppendCacheSummary(lruSummary, "miss in candidate list (total)", totalMissInCandidate);

    return lruSummary;
}

LruPageClean::LruPageClean(BufLruListArray *lruListArray, uint64 initCandidateListLength, BufTable *bufTable)
    : m_candidateSafePercent(0),
      m_initCandidateListLength(initCandidateListLength),
      m_lruScanDepth(0),
      m_lruListArray(lruListArray),
      m_bufTable(bufTable),
      m_isStop(false),
      m_workThread(nullptr)
{}

void LruPageClean::Init()
{
    m_candidateSafePercent = g_storageInstance->GetGuc()->candidateSafePercent;
    m_lruScanDepth = g_storageInstance->GetGuc()->GetLruScanDepth();
}

void LruPageClean::StartWorkThreads()
{
    m_workThread = new std::thread(&LruPageClean::Run, this);
}

void LruPageClean::TryCleanLruListToCandidate(bool ignoreDirtyPage, int32 &needFlushPageNum, BufLruList *bufLru,
                                              bool &pageMoved)
{
    LruNode *lruNode = nullptr;
    LruList *lru = bufLru->GetLruList();
    uint32 loopLength = DstoreMin(static_cast<uint32>(lru->Length() - 1), m_lruScanDepth);
    for (uint32 i = 0; i < loopLength; i++) {
        if (needFlushPageNum) {
            lru->LockList();
            lruNode = lru->PopTail();
            if (lruNode == nullptr) {
                lru->UnlockList();
                break;
            }
            lru->UnlockList();
            BufferDesc *lruBufferDesc = lruNode->GetValue<BufferDesc>();
            uint32 bufferHash = m_bufTable->GetHashCode(&(lruBufferDesc->bufTag));
            m_bufTable->LockBufMapping(bufferHash, LW_EXCLUSIVE);
            if (!lruBufferDesc->TryAcquireCrAssignLwlock(LW_SHARED)) {
                m_bufTable->UnlockBufMapping(bufferHash);
                lru->LockList();
                lru->AddHead(lruNode);
                lru->UnlockList();
                continue;
            }
            if (!lruBufferDesc->IsCrPage() && !lruBufferDesc->HasCrBuffer() &&
                lruBufferDesc->FastLockHdrIfReusable(INVALID_BUFFER_TAG, false, ignoreDirtyPage)) {
                lruBufferDesc->ReleaseCrAssignLwlock();
                uint64 bufState = GsAtomicReadU64(&lruBufferDesc->state);
                if ((bufState & Buffer::BUF_CONTENT_DIRTY) || (bufState & Buffer::BUF_HINT_DIRTY)) {
                    lruBufferDesc->UnlockHdr(bufState);
                    LatencyStat::Timer timer(&BufPerfUnit::GetInstance().m_backendTryFlush);
                    if (STORAGE_FUNC_FAIL(g_storageInstance->GetBufferMgr()->TryFlush(lruBufferDesc))) {
                        m_bufTable->UnlockBufMapping(bufferHash);
                        lru->LockList();
                        lru->AddHead(lruNode);
                        lru->UnlockList();
                        continue;
                    }
                    timer.End();
                    if (!lruBufferDesc->FastLockHdrIfReusable(INVALID_BUFFER_TAG, false, ignoreDirtyPage)) {
                        m_bufTable->UnlockBufMapping(bufferHash);
                        lru->LockList();
                        lru->AddHead(lruNode);
                        lru->UnlockList();
                        continue;
                    }
                }
                if (bufState & Buffer::BUF_TAG_VALID) {
                    BufferTag bufTag = lruBufferDesc->GetBufferTag();
                    m_bufTable->Remove(&bufTag, bufferHash);
                }
                m_bufTable->UnlockBufMapping(bufferHash);
                lruBufferDesc->bufTag.SetInvalid();
                lruBufferDesc->fileVersion = INVALID_FILE_VERSION;
                lruBufferDesc->pageVersionOnDisk = INVALID_PAGE_LSN;
                lruBufferDesc->UnlockHdr(bufState & Buffer::BUF_FLAG_RESET_MASK);
                bufLru->PushBackToCandidate(lruBufferDesc);
                needFlushPageNum--;
            } else {
                lruBufferDesc->ReleaseCrAssignLwlock();
                m_bufTable->UnlockBufMapping(bufferHash);
                lru->LockList();
                lru->AddHead(lruNode);
                lru->UnlockList();
            }
            pageMoved = true;
        } else {
            break;
        }
    }
    return;
}

void LruPageClean::Run()
{
    InitSignalMask();
    (void)g_storageInstance->CreateThreadAndRegister(INVALID_PDB_ID, false, "BgLruPageClean", true);
    ErrLog(DSTORE_LOG, MODULE_BUFMGR,
           ErrMsg("[LruPageClean] BgLruPageCleanThread start. Current m_initCandidateListLength %lu, partitions %d",
           m_initCandidateListLength, static_cast<int>(static_cast<int>(m_lruListArray->GetLruPartition()))));
    uint64 minSleepTime = 10000; /* 10ms */
    uint64 maxSleepTime = 1000000; /* 10s */
    uint64 currentSleepTime = 100000; /* 100ms */
    int32 candidateSafeNum = static_cast<int32>(m_initCandidateListLength * m_candidateSafePercent);
    while (!m_isStop.load(std::memory_order_acquire)) {
        bool pageMoved = false;
        for (uint32 i = 0; i < static_cast<uint32>(m_lruListArray->GetLruPartition()); i++) {
            BufLruList *bufLru = m_lruListArray->GetLruListAt(i);
            /* calculate expected num of pages in candidate list: (candidate list length * threshold from guc) */
            int32 recycleGap = candidateSafeNum - bufLru->GetCandidateList()->Length();
            int32 needFlushPageNum = DstoreMax(recycleGap, 0);
            ErrLog(DSTORE_DEBUG1, MODULE_BUFMGR,
                   ErrMsg("[LruPageClean] Current partition %u need page needFlushPageNum %d, safe line "
                          "candidateSafeNum %d, recycleGap %d, candidate length %d.",
                          i, needFlushPageNum, candidateSafeNum, recycleGap,
                          static_cast<int>(bufLru->GetCandidateList()->Length())));
            bool ignoreDirtyPage = true;
            TryCleanLruListToCandidate(ignoreDirtyPage, needFlushPageNum, bufLru, pageMoved);
            /* If the number of dirty pages in the first round is less than the required number of dirty pages, the
             * second round starts to clean dirty pages */
            if (needFlushPageNum > 0) {
                TryCleanLruListToCandidate(false, needFlushPageNum, bufLru, pageMoved);
            }

            if (pageMoved) {
                currentSleepTime = static_cast<uint32>(currentSleepTime * 0.8);
            } else {
                currentSleepTime = static_cast<uint32>(currentSleepTime * 1.2);
            }
            /* keep clamping currentSleepTime [10ms,1s] */
            currentSleepTime = DstoreMax(minSleepTime, currentSleepTime);
            currentSleepTime = DstoreMin(maxSleepTime, currentSleepTime);
        }
        GaussUsleep(currentSleepTime);
    }
    ErrLog(DSTORE_LOG, MODULE_BUFMGR, ErrMsg("[LruPageClean] LruPageClean::BgLruPageCleanThread exit."));
    g_storageInstance->UnregisterThread();
}

void LruPageClean::StopWorkThreads()
{
    m_isStop.store(true, std::memory_order_release);
    if (m_workThread != nullptr) {
        m_workThread->join();
        delete m_workThread;
        m_workThread = nullptr;
    }
}

LruPageClean::~LruPageClean()
{}
}
