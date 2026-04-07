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

#ifndef DSTORE_PERF_CATALOG_H
#define DSTORE_PERF_CATALOG_H

#include "dstore_perf_unit.h"
#include "perfcounter/dstore_skip_list.h"
#include "common/memory/dstore_mctx.h"
#include "lock/dstore_lwlock.h"
#include "common/dstore_datatype.h"

namespace DSTORE {
constexpr uint8 PERF_COUNTER_ALL_UNIT_TASK_INDEX = 0xFF;

class PerfCatalog {
public:
    static PerfCatalog &GetInstance()
    {
        static PerfCatalog instance;
        return instance;
    }
    ~PerfCatalog() noexcept
    {
        if (m_perfUnits != nullptr) {
            DELETE_AND_RESET(m_perfUnits);
        }
        if (m_perfCatalogMemCtx != nullptr) {
            DstoreMemoryContextDelete(m_perfCatalogMemCtx);
        }
        m_perfCatalogMemCtx = nullptr;
    }

    DISALLOW_COPY_AND_MOVE(PerfCatalog);

    bool Init();
    bool Destroy();
    bool RegisterPerfUnit(PerfUnit *perfUnit, PerfId &id);
    bool UnregisterPerfUnit(PerfId id);
    int32 Dump(char *dumpBuf, uint8 taskIndex = PERF_COUNTER_ALL_UNIT_TASK_INDEX) const;
    void Reset(uint8 taskIndex = PERF_COUNTER_ALL_UNIT_TASK_INDEX);

    static bool RunDump(uint8 taskIndex);

    inline PerfId GetNewCatalogId()
    {
        return m_catalogIdGenerator.fetch_add(1);
    }

private:
    PerfCatalog() = default;

private:
    std::atomic<PerfId> m_catalogIdGenerator{PERF_STAT_INVALID_ID};
    SkipList<PerfId, PerfUnit *> *m_perfUnits{nullptr};
    DstoreMemoryContext m_perfCatalogMemCtx{nullptr};
    uint32 m_bufferSize{PERF_DUMPBUFFER_INIT_SIZE};
};
}  // namespace DSTORE
#endif /* STORAGE_PERF_CATALOG_H */
