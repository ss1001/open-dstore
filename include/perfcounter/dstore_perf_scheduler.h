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
 * Description: The perf counter scheduler for performance monitor.
 */

#ifndef DSTORE_PERF_COUNTER_SCHEDULER_H
#define DSTORE_PERF_COUNTER_SCHEDULER_H

#include "perfcounter/dstore_perf_task.h"

namespace DSTORE {

class PerfCounterScheduler {
public:
    static PerfCounterScheduler &GetInstance()
    {
        static PerfCounterScheduler instance{};
        return instance;
    }

    ~PerfCounterScheduler()
    {
        m_perfSchedulerMemCtx = nullptr;
    }

    DISALLOW_COPY_AND_MOVE(PerfCounterScheduler);

    bool Init()
    {
        m_perfSchedulerMemCtx = DstoreAllocSetContextCreate(
            g_storageInstance->GetMemoryMgr()->GetGroupContext(MEMORY_CONTEXT_LONGLIVE), "PerfSchedulerMemoryContext",
            ALLOCSET_DEFAULT_MINSIZE, ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_INITSIZE,
            MemoryContextType::SHARED_CONTEXT);
        if (STORAGE_VAR_NULL(m_perfSchedulerMemCtx)) {
            ErrLog(DSTORE_ERROR, MODULE_FRAMEWORK,
                   ErrMsg("Create perfcounter scheduler mem ctx failed. PerfCounter is unavailable."));
            return false;
        }

        m_taskCnt.store(0);
        m_cancelTaskFlag = false;
        ErrLog(DSTORE_INFO, MODULE_FRAMEWORK, ErrMsg("Perfcounter scheduler init success."));
        return true;
    }

    bool Destroy()
    {
        if (m_perfSchedulerMemCtx == nullptr) {
            return true;
        }
        CancelAllTask();
        DstoreMemoryContextDelete(m_perfSchedulerMemCtx);
        m_perfSchedulerMemCtx = nullptr;

        ErrLog(DSTORE_INFO, MODULE_FRAMEWORK, ErrMsg("Perfcounter scheduler destory success."));
        return true;
    }

    uint8 CreateTask(const PerfSchedulerTask &callbackTask, uint32 timeIntervalMs)
    {
        uint8 taskIndex = m_taskCnt.load();
        while (((taskIndex + 1) <= PERF_COUNTER_MAX_TASK_NUM) &&
               !(m_taskCnt.compare_exchange_weak(taskIndex, static_cast<uint8>(taskIndex + 1)))) {
        }

        if (taskIndex + 1 > PERF_COUNTER_MAX_TASK_NUM) {
            ErrLog(DSTORE_ERROR, MODULE_FRAMEWORK, ErrMsg("Perfcounter scheduler can not create task any more."));
            return 0;
        }

        PerfCounterTask *task =
            DstoreNew(m_perfSchedulerMemCtx) PerfCounterTask(static_cast<uint8>(taskIndex + 1), callbackTask);
        if (task == nullptr) {
            ErrLog(DSTORE_ERROR, MODULE_FRAMEWORK,
                   ErrMsg("Perfcounter scheduler create task failed. PerfCounter is unavailable."));
            return 0;
        }

        m_bgThread[taskIndex] = new std::thread(StartBGPerfCounterTask, task, timeIntervalMs);
        if (m_bgThread[taskIndex] == nullptr) {
            delete task;
            ErrLog(
                DSTORE_ERROR, MODULE_FRAMEWORK,
                ErrMsg("Perfcounter start task failed. Perfcounter schedule task slot %d is unavailable.", taskIndex));
            return 0;
        }
        return ++taskIndex;
    }

    bool CancelTask(uint8 taskIndex)
    {
        if (taskIndex > PERF_COUNTER_MAX_TASK_NUM || taskIndex == 0) {
            ErrLog(DSTORE_INFO, MODULE_FRAMEWORK,
                   ErrMsg("Perfcounter cancel task failed, invalid task index:%hhu", taskIndex));
            return false;
        } else {
            SetCancelFlag();
            return true;
        }
    }

    void CancelAllTask()
    {
        for (uint8 taskIndex = 0; taskIndex < m_taskCnt; taskIndex++) {
            if (!PerfCounterScheduler::GetInstance().CancelTask(static_cast<uint8>(taskIndex + 1))) {
                ErrLog(DSTORE_INFO, MODULE_FRAMEWORK,
                       ErrMsg("Cancel perfcounter scheduler task[%hhu] failed.", static_cast<uint8>(taskIndex + 1)));
            }
            if (m_bgThread[taskIndex] != nullptr) {
                if (m_bgThread[taskIndex]->joinable()) {
                    m_bgThread[taskIndex]->join();
                }
                delete m_bgThread[taskIndex];
                m_bgThread[taskIndex] = nullptr;
            }
        }
    }

    void SetCancelFlag()
    {
        m_cancelTaskFlag = true;
    }

    bool GetCancelFlag()
    {
        return m_cancelTaskFlag;
    }

private:
    PerfCounterScheduler() = default;

    static void StartBGPerfCounterTask(PerfCounterTask *task, uint32 timeIntervalMs)
    {
        InitSignalMask();
        timeval curTime;
        uint8 perfCounterTimeInterval = 30;
        if (static_cast<uint8>(g_storageInstance->GetGuc()->perfCounterInterval) != 0) {
            perfCounterTimeInterval = static_cast<uint8>(g_storageInstance->GetGuc()->perfCounterInterval);
        }
        ErrLog(DSTORE_INFO, MODULE_FRAMEWORK, ErrMsg("Current perfCounter interval %u.", perfCounterTimeInterval));
        (void)g_storageInstance->CreateThreadAndRegister(INVALID_PDB_ID, false, "PerfScheduler", true,
                                                         ThreadMemoryLevel::THREADMEM_HIGH_PRIORITY);
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeIntervalMs));
            if (PerfCounterScheduler::GetInstance().GetCancelFlag()) {
                break;
            }
            (void)gettimeofday(&curTime, NULL);
            if ((curTime.tv_sec) % perfCounterTimeInterval == 0) {
                task->Call();
            }
        }
        g_storageInstance->UnregisterThread();
    }

private:
    DstoreMemoryContext m_perfSchedulerMemCtx{nullptr};
    std::atomic<uint8> m_taskCnt{0};
    std::atomic<bool> m_cancelTaskFlag{false};
    uint64 m_taskId[PERF_COUNTER_MAX_TASK_NUM];
    std::thread *m_bgThread[PERF_COUNTER_MAX_TASK_NUM];
};
}  // namespace DSTORE
#endif /* STORAGE_PERF_COUNTER_SCHEDULER_H */
