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
 * Description: The unit for performance monitoring unit.
 */

#ifndef DSTORE_PERF_UNIT_H
#define DSTORE_PERF_UNIT_H

#include <list>
#include <unordered_set>
#include "dstore_perf_item.h"

#define DECLARE_PERF_ITEM(type, var, itemName, level) \
    type var                                          \
    {}
#define CLOSE_PERFITEM(type, var, itemName, level) ((var).SetPerfLevel(PsCommon::PerfLevel::CLOSE))
#define REGIST_PERFITEM(type, var, itemName, level) RegisterPerfItem(&(var), #itemName, level)
#define RESET_PERFITEM(type, var, itemName, level) ((var).Reset())

namespace DSTORE {
constexpr uint8 PERF_UNIT_INVALID_TASK_INDEX = 0;
constexpr uint8 PERF_UNIT_DEFAULT_TASK_INDEX = 1;
constexpr uint8 PERF_UNIT_ROOT_LEVEL = 0;
constexpr uint8 PERF_UNIT_MAX_LEVEL = 4;
/* PERF_UNIT_ROOT_LEVEL not required indent. */
constexpr char PERF_UNIT_INDENT[PERF_UNIT_MAX_LEVEL] = {'\t', '\t', '\t', '\t'};
constexpr uint8 PERF_MAX_NAME_LEN = 64;


using PerfItemList = std::unordered_set<PerfStatBase *>;
class PerfUnit {
public:
    explicit PerfUnit(bool resetAfterDump = true) : m_resetAfterDump(resetAfterDump)
    {}

    virtual ~PerfUnit() noexcept
    {
        if (m_validOnCatalog) {
            ErrLog(DSTORE_WARNING,
                   MODULE_FRAMEWORK, ErrMsg("PerfUnit-%p destruct without DestroyUnit called", this));
        }
        m_name = nullptr;
        m_parentUnit = nullptr;
        m_perfUnitMemCtx = nullptr;
    }

    DISALLOW_COPY_AND_MOVE(PerfUnit);

    int32 Dump(char *dumpBuf, int32 bufIndex, const char *rootName);

    inline bool ResetAfterDump() const
    {
        return m_resetAfterDump;
    }

    void Reset();

    inline uint8 GetUnitLevel() const
    {
        return m_unitLevel;
    }

    /*
     * Free child PerfUnit without Catalog.
     * Occurs in destroying parent PerfUnit, but child PerfUnit is not destroyed.
     */
    inline void FreeOnCatalog()
    {
        m_parentUnit = nullptr;
        m_freeOnCatalog = true;
    }

    inline bool OnScheduler(uint8 taskIndex) const
    {
        return m_taskIndex == taskIndex;
    }

    inline const char *GetUnitName() const
    {
        return m_name;
    }

    DstoreMemoryContext m_perfUnitMemCtx{nullptr};
protected:
    bool InitUnit(const char *name, DstoreMemoryContext memCtxPtr, uint8 taskIndex = PERF_UNIT_INVALID_TASK_INDEX,
                  PerfUnit *parent = nullptr, bool printItemParent = false);
    bool DestroyUnit();
    void RegisterPerfItem(PerfStatBase *stat, const char *name, PerfLevel level);
    void UnRegisterPerfItem(PerfStatBase *stat, PerfLevel level);

private:
    bool RegisterToParent(PerfUnit *parent);
    bool RegisterChild(PerfUnit *child);
    void UnRegisterFromParent();
    void UnRegisterChild(PerfUnit *child);
    void FreeAllChilds();
    void CleanUnit();
    bool StrDupUnitName(const char *unitName, char *&dupName);

    template <class... Args>
    inline int32 PerfLogSprintf(char *dumpBuf, int32 bufOffset, const char *format, Args... args) const
    {
        int32 rc =
            sprintf_s(dumpBuf + bufOffset, PerfDumpBuf::GetInstance().DumpBufSize() - static_cast<uint32>(bufOffset),
                      format, args...);
        if (unlikely((rc) == -1)) {
            ErrLog(DSTORE_WARNING,
                   MODULE_FRAMEWORK, ErrMsg("Write dumpBuf error, buffer space may be insufficient."));
            return bufOffset;
        }
        return bufOffset + rc;
    }

    inline int32 PerfLogMemcpy(char *dumpBuf, int32 bufOffset, const char *format, int32 length) const
    {
        errno_t rc = memcpy_s(dumpBuf + bufOffset, PerfDumpBuf::GetInstance().DumpBufSize() -
            static_cast<uint32>(bufOffset), format, static_cast<uint32>(length));
        storage_securec_check(rc, "\0", "\0");
        return bufOffset + length;
    }

private:
    PerfId m_id{PERF_STAT_INVALID_ID};
    const char *m_name{nullptr};
    PerfItemList m_perfItems{};
    std::mutex m_perfItemsMutex{};
    PerfUnit *m_parentUnit{nullptr};
    std::list<PerfUnit *> m_childUnit{};
    std::mutex m_childUnitMutex{};
    uint8 m_unitLevel{PERF_UNIT_ROOT_LEVEL};
    bool m_freeOnCatalog{false};
    uint8 m_taskIndex{PERF_UNIT_INVALID_TASK_INDEX};
    bool m_resetAfterDump{true};
    bool m_printItemParent{false};
    bool m_validOnCatalog{false};
};

class DstorePerfUnit : public PerfUnit {
public:
    bool Destroy()
    {
        if (!DestroyUnit()) {
            return false;
        }
        for (uint8 i = 0; i < m_msgPerfItemNum; ++i) {
            UnRegisterPerfItem(m_msgPerfIterms[i], PerfLevel::PERF_DEBUG);
        }
        DstorePfreeExt(m_msgPerfIterms);
        return true;
    }

    LatencyStat **CreateGroupPerfItem(const char *groupName, uint8 num, PerfLevel level)
    {
        errno_t rc = 0;
        char name[PERF_MAX_NAME_LEN] = {0};
        LatencyStat **stats = static_cast<LatencyStat**>(DstoreMemoryContextAllocZero(m_perfUnitMemCtx,
            num * sizeof(LatencyStat*)));
        StorageReleasePanic(stats == nullptr, MODULE_BUFMGR, ErrMsg("Alloc memory for stats failed"));
        for (uint8 i = 0; i < num; ++i) {
            stats[i] = DstoreNew(m_perfUnitMemCtx) LatencyStat();
            StorageReleasePanic(stats[i] == nullptr, MODULE_BUFMGR, ErrMsg("Alloc memory for stats %hu failed", i));
            rc = snprintf_s(name, PERF_MAX_NAME_LEN, PERF_MAX_NAME_LEN - 1, "%s%hu", groupName, i);
            storage_securec_check_ss(rc);
            char *dupName = Dstorepstrdup(name);
            if (unlikely(!dupName)) {
                ErrLog(DSTORE_PANIC, MODULE_FRAMEWORK, ErrMsg("Dump PerfItem name %s failed", name));
            }
            stats[i]->Init();
            RegisterPerfItem(stats[i], dupName, level);
        }
        return stats;
    }

    void CreateMsgPerfItems(const char* name, uint8 num, PerfLevel level = DSTORE::PerfLevel::PERF_DEBUG)
    {
        m_msgPerfIterms = CreateGroupPerfItem(name, num, level);
        if (unlikely(!m_msgPerfIterms)) {
            ErrLog(DSTORE_PANIC,
                   MODULE_FRAMEWORK, ErrMsg("Alloc memory for CreateMsgPerfItems(%s) failed.", name));
            return;
        }
        m_msgPerfItemNum = num;
    }

    LatencyStat &GetMsgPerfItems(uint8 index)
    {
        if (unlikely(index >= m_msgPerfItemNum)) {
            ErrLog(DSTORE_PANIC, MODULE_FRAMEWORK, ErrMsg("Wrong Index for msg perf item(%hu).", index));
        }
        return *m_msgPerfIterms[index];
    }

private:
    uint8 m_msgPerfItemNum = 0;
    LatencyStat** m_msgPerfIterms = nullptr;
};
}  // namespace DSTORE
#endif
