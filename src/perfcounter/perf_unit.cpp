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
 * Description: The unit for performance monitor.
 */

#include "perfcounter/dstore_perf_unit.h"
#include "perfcounter/dstore_perf_catalog.h"

namespace DSTORE {
bool PerfUnit::InitUnit(const char *name, DstoreMemoryContext memCtxPtr, uint8 taskIndex, PerfUnit *parent,
                        bool printItemParent)
{
    if (name == nullptr || memCtxPtr == nullptr) {
        ErrLog(DSTORE_ERROR, MODULE_FRAMEWORK, ErrMsg("Init PerfUnit failed. Unit name or MemctxPtr is nullptr."));
        return false;
    }

    m_perfUnitMemCtx = memCtxPtr;
    m_freeOnCatalog = false;
    m_printItemParent = printItemParent;

    char *dupName = nullptr;
    if (!StrDupUnitName(name, dupName)) {
        CleanUnit();
        return false;
    }
    m_name = dupName;
    m_taskIndex = taskIndex;

    if (parent == nullptr) {
        if (!PerfCatalog::GetInstance().RegisterPerfUnit(this, m_id)) {
            CleanUnit();
            ErrLog(DSTORE_ERROR, MODULE_FRAMEWORK,
                   ErrMsg("Init PerfUnit %s failed. PerfCounter maybe is unavailable.", m_name));
            return false;
        }
    } else {
        m_id = PerfCatalog::GetInstance().GetNewCatalogId();
        /* m_unitLevel only used for PerfLog's indent. */
        uint8 level = static_cast<uint8>(parent->GetUnitLevel() + 1);
        m_unitLevel = (level < PERF_UNIT_MAX_LEVEL ? level : PERF_UNIT_MAX_LEVEL);

        if (!RegisterToParent(parent)) {
            CleanUnit();
            ErrLog(DSTORE_ERROR, MODULE_FRAMEWORK,
                   ErrMsg("%s Register ParentPerfUnit failed. PerfCounter maybe is unavailable.", m_name));
            return false;
        }
    }

    m_validOnCatalog = true;
    ErrLog(DSTORE_INFO, MODULE_FRAMEWORK, ErrMsg("PerfUnit %s init success.", m_name));
    return true;
}

bool PerfUnit::DestroyUnit()
{
    if (m_perfUnitMemCtx == nullptr) {
        ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK, ErrMsg("Destroy PerfUnit, but it not initialized."));
        return true;
    }

    if (!m_childUnit.empty()) {
        ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK,
               ErrMsg("Destroy Parent PerfUnit %s, but Child PerfUnit not destroyed."
                      "Child PerfUnit becomes free on PerfCatalog.",
                      m_name));
        FreeAllChilds();
    }

    if (m_parentUnit == nullptr && !m_freeOnCatalog) {
        if (!PerfCatalog::GetInstance().UnregisterPerfUnit(m_id)) {
            ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK,
                   ErrMsg("Destroy PerfUnit %s failed. PerfCounter maybe is unavailable.", m_name));
            return false;
        }
    } else {
        UnRegisterFromParent();
    }

    ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK, ErrMsg("PerfUnit %s destory success.", m_name));
    CleanUnit();
    m_validOnCatalog = false;

    return true;
}

void PerfUnit::CleanUnit()
{
    m_perfUnitMemCtx = nullptr;
}

void PerfUnit::RegisterPerfItem(PerfStatBase *stat, const char *name, PerfLevel level)
{
    stat->SetName(name);
    stat->SetPerfLevel(level);
    StorageReleasePanic(!stat->IsInited(), MODULE_FRAMEWORK, ErrMsg("perf stat not inited:%s, ", stat->GetName()));
    if (level < PerfGlobalLevel::GetInstance().GetPerfLevel()) {
        return;
    }

    PerfId id = PerfCatalog::GetInstance().GetNewCatalogId();
    stat->SetId(id);
    stat->SetIndentLevel(static_cast<uint8>(m_unitLevel + 1));

    std::unique_lock<std::mutex> waitLock(m_perfItemsMutex);
    (void)m_perfItems.insert(stat);
}

void PerfUnit::UnRegisterPerfItem(PerfStatBase *stat, PerfLevel level)
{
    if (level < PerfGlobalLevel::GetInstance().GetPerfLevel()) {
        return;
    }
    std::unique_lock<std::mutex> waitLock(m_perfItemsMutex);
    (void)m_perfItems.erase(stat);
}

bool PerfUnit::RegisterToParent(PerfUnit *parent)
{
    if (!parent->RegisterChild(this)) {
        return false;
    }

    m_parentUnit = parent;
    return true;
}

bool PerfUnit::RegisterChild(PerfUnit *child)
{
    std::unique_lock<std::mutex> waitChildLock(m_childUnitMutex);
    m_childUnit.push_back(child);
    return true;
}

void PerfUnit::UnRegisterFromParent()
{
    if (!m_freeOnCatalog) {
        m_parentUnit->UnRegisterChild(this);
        m_parentUnit = nullptr;
    }
}

void PerfUnit::UnRegisterChild(PerfUnit *child)
{
    std::unique_lock<std::mutex> waitChildLock(m_childUnitMutex);
    for (std::list<PerfUnit *>::const_iterator iter = m_childUnit.begin(); iter != m_childUnit.end(); ++iter) {
        if (*iter == child) {
            (void)m_childUnit.erase(iter);
            break;
        }
    }
}

void PerfUnit::FreeAllChilds()
{
    std::unique_lock<std::mutex> waitChiledLock(m_childUnitMutex);
    for (std::list<PerfUnit *>::const_iterator iter = m_childUnit.begin(); iter != m_childUnit.end(); ++iter) {
        (*iter)->FreeOnCatalog();
    }
}

int32 PerfUnit::Dump(char *dumpBuf, int32 bufIndex, const char *rootName)
{
    int32 unitOffset = bufIndex;

    /* PerfUnit indentation */
    if (m_unitLevel > 0) {
        unitOffset = PerfLogMemcpy(dumpBuf, unitOffset, PERF_ITEM_INDENT, m_unitLevel);
    }
    unitOffset = PerfLogSprintf(dumpBuf, unitOffset, "%s id=%u\n", m_name, m_id);

    int32 itemOffset = 0;
    {
        std::unique_lock<std::mutex> waitLock(m_perfItemsMutex);
        for (PerfItemList::const_iterator iter = m_perfItems.begin(); iter != m_perfItems.end(); ++iter) {
            if (unitOffset + PERF_ITEM_LOG_MAX_SIZE > static_cast<int32>(PerfDumpBuf::GetInstance().DumpBufSize())) {
                return PERF_DUMPBUFER_OUT_OF_MEMORY;
            }

            if (m_printItemParent) {
                itemOffset = (*iter)->Dump(dumpBuf, unitOffset, rootName);
            } else {
                itemOffset = (*iter)->Dump(dumpBuf, unitOffset, nullptr);
            }
            unitOffset += itemOffset;
        }
    }

    {
        std::unique_lock<std::mutex> waitChildLock(m_childUnitMutex);
        if (!m_childUnit.empty()) {
            unitOffset = PerfLogMemcpy(dumpBuf, unitOffset, PERF_ITEM_INDENT, m_unitLevel + 1);
            unitOffset = PerfLogSprintf(dumpBuf, unitOffset, "[%s: id=%u]'s child PerfUnit:\n", m_name, m_id);

            int32 childOffset = 0;
            for (std::list<PerfUnit *>::const_iterator iter = m_childUnit.begin(); iter != m_childUnit.end(); ++iter) {
                childOffset = (*iter)->Dump(dumpBuf, unitOffset, rootName);
                if (childOffset == PERF_DUMPBUFER_OUT_OF_MEMORY) {
                    return PERF_DUMPBUFER_OUT_OF_MEMORY;
                }
                unitOffset += childOffset;
            }
        }
    }

    return unitOffset - bufIndex;
}

void PerfUnit::Reset()
{
    {
        std::unique_lock<std::mutex> waitLock(m_perfItemsMutex);
        for (PerfItemList::iterator iter = m_perfItems.begin(); iter != m_perfItems.end(); ++iter) {
            (*iter)->Reset();
        }
    }

    {
        std::unique_lock<std::mutex> waitChildLock(m_childUnitMutex);
        if (!m_childUnit.empty()) {
            for (std::list<PerfUnit *>::iterator iter = m_childUnit.begin(); iter != m_childUnit.end(); ++iter) {
                (*iter)->Reset();
            }
        }
    }
}

bool PerfUnit::StrDupUnitName(UNUSE_PARAM const char *unitName, char *&dupName)
{
    dupName = Dstorepstrdup(unitName);
    if (dupName == nullptr) {
        ErrLog(DSTORE_ERROR, MODULE_FRAMEWORK, ErrMsg("Dump PerfUnit name %s failed", unitName));
        return false;
    }
    return true;
}
}  // namespace DSTORE
