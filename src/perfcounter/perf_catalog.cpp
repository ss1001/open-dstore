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
 * Description: The catalog for performance monitor.
 */

#include "perfcounter/dstore_perf_catalog.h"

namespace DSTORE {
bool PerfCatalog::Init()
{
    m_perfCatalogMemCtx =
        DstoreAllocSetContextCreate(g_storageInstance->GetMemoryMgr()->GetGroupContext(MEMORY_CONTEXT_BUFFER),
                                    "perfCatalogMemory", ALLOCSET_DEFAULT_MINSIZE, ALLOCSET_DEFAULT_INITSIZE,
                                    ALLOCSET_DEFAULT_MAXSIZE, MemoryContextType::SHARED_CONTEXT);

    SkipListErrNo errNo;
    m_perfUnits = MakeSkipList<PerfId, PerfUnit *>(&errNo, m_perfCatalogMemCtx);
    if (errNo != SkipListErrNo::SUCCESS) {
        ErrLog(DSTORE_PANIC, MODULE_FRAMEWORK,
               ErrMsg("PerfCounter create skip-list failed. Error code: %d. PerfCounter is unavailable.",
                      static_cast<int32>(errNo)));
        DstoreMemoryContextDelete(m_perfCatalogMemCtx);
        m_perfCatalogMemCtx = nullptr;
        return false;
    }

    if (!PerfDumpBuf::GetInstance().Init(m_perfCatalogMemCtx)) {
        ErrLog(DSTORE_PANIC,
               MODULE_FRAMEWORK, ErrMsg("PerfDumpBuf init failed. PerfCounter is unavailable."));
        DstoreMemoryContextDelete(m_perfCatalogMemCtx);
        m_perfCatalogMemCtx = nullptr;
        return false;
    }

    m_catalogIdGenerator.store(PERF_STAT_INITIAL_ID);
    ErrLog(DSTORE_INFO, MODULE_FRAMEWORK, ErrMsg("PerfCatalog init success."));
    return true;
}

bool PerfCatalog::Destroy()
{
    if (m_perfUnits != nullptr) {
        DELETE_AND_RESET(m_perfUnits);
    }
    if (m_perfCatalogMemCtx == nullptr) {
        return true;
    }
    PerfDumpBuf::GetInstance().Destroy();

    DstoreMemoryContextDelete(m_perfCatalogMemCtx);
    m_perfCatalogMemCtx = nullptr;

    ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK, ErrMsg("PerfCatalog destory success."));
    return true;
}

bool PerfCatalog::RegisterPerfUnit(PerfUnit *perfUnit, PerfId &id)
{
    id = m_catalogIdGenerator.fetch_add(1);

    SkipListErrNo errNo = SkipListErrNo::SUCCESS;
    if (unlikely(m_perfUnits == nullptr)) {
        return false;
    }
    m_perfUnits->Insert(&errNo, id, perfUnit);
    if (errNo != SkipListErrNo::SUCCESS) {
        ErrLog(DSTORE_ERROR, MODULE_FRAMEWORK,
               ErrMsg("PerfCounter register PerfUnit failed. Errors:%d. PerfCounter maybe is unavailable.",
                      static_cast<int32>(errNo)));
        return false;
    }

    return true;
}

bool PerfCatalog::UnregisterPerfUnit(PerfId id)
{
    SkipListErrNo errNo = SkipListErrNo::SUCCESS;
    m_perfUnits->Erase(&errNo, id);
    if (errNo != SkipListErrNo::SUCCESS) {
        ErrLog(DSTORE_ERROR, MODULE_FRAMEWORK,
               ErrMsg("PerfCounter unregister PerfUnit failed. Errors:%d. PerfCounter maybe is unavailable.",
                      static_cast<int32>(errNo)));
        return false;
    }

    return true;
}

int32 PerfCatalog::Dump(char *dumpBuf, uint8 taskIndex) const
{
    if (PerfGlobalLevel::GetInstance().GetPerfLevel() == PerfLevel::OFF) {
        return 0;
    }

    if (dumpBuf == nullptr) {
        ErrLog(DSTORE_ERROR, MODULE_FRAMEWORK, ErrMsg("PerfCounter dump failed. DumpBuffer is nullptr."));
        return 0;
    }

    int32 totalOffset = 0;
    int32 unitOffset = 0;

    m_perfUnits->AcquireSharedLockForIterator();
    for (auto iter = m_perfUnits->Begin(); iter != m_perfUnits->End(); ++iter) {
        if (taskIndex == PERF_COUNTER_ALL_UNIT_TASK_INDEX || iter.value()->OnScheduler(taskIndex)) {
            const char *rootName = iter.value()->GetUnitName();
            unitOffset = iter.value()->Dump(dumpBuf, totalOffset, rootName);
            if (unitOffset == PERF_DUMPBUFER_OUT_OF_MEMORY) {
                m_perfUnits->ReleaseSharedLockForIterator();
                ErrLog(DSTORE_ERROR, MODULE_FRAMEWORK, ErrMsg("PerfCounter has no space for dumping items."));
                return PERF_DUMPBUFER_OUT_OF_MEMORY;
            }

            totalOffset += unitOffset;
        }
    }
    m_perfUnits->ReleaseSharedLockForIterator();
    return totalOffset;
}

void PerfCatalog::Reset(uint8 taskIndex)
{
    if (PerfGlobalLevel::GetInstance().GetPerfLevel() == PerfLevel::OFF) {
        return;
    }

    m_perfUnits->AcquireSharedLockForIterator();
    for (auto iter = m_perfUnits->Begin(); iter != m_perfUnits->End(); ++iter) {
        if ((taskIndex == PERF_COUNTER_ALL_UNIT_TASK_INDEX || iter.value()->OnScheduler(taskIndex)) &&
            iter.value()->ResetAfterDump()) {
            iter.value()->Reset();
        }
    }
    m_perfUnits->ReleaseSharedLockForIterator();
}

bool PerfCatalog::RunDump(uint8 taskIndex)
{
    char *buffer = PerfDumpBuf::GetInstance().CreateDumpBuf(PerfCatalog::GetInstance().m_bufferSize);
    if (unlikely(buffer == nullptr)) {
        ErrLog(DSTORE_PANIC,
               MODULE_FRAMEWORK, ErrMsg("Failed to initialize the perfcounter dump buffer space."));
    }
    int32 totalOffset = PerfCatalog::GetInstance().Dump(buffer, taskIndex);
    if (unlikely(totalOffset == PERF_DUMPBUFER_OUT_OF_MEMORY)) {
        PerfCatalog::GetInstance().m_bufferSize =
            PerfDumpBuf::GetInstance().DumpBufSize() + PERF_DUMPBUFFER_EXTEND_SIZE;
    }

    PerfCatalog::GetInstance().Reset(taskIndex);
    if (buffer[0] != '\0') {
        ErrLog(DSTORE_LOG, MODULE_FRAMEWORK, ErrMsg("Perfcounter Output:\n%s", buffer));
    }

    return true;
}
}
