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
 * Description: The item for performance monitor.
 */

#ifndef DSTORE_PERF_ITEM_H
#define DSTORE_PERF_ITEM_H

#include <atomic>
#include <chrono>
#include <limits>
#include <sys/time.h>
#include "common/memory/dstore_mctx.h"
#include "dstore_perf_stat_base.h"

namespace DSTORE {
constexpr uint32 MINUTES_ONE_HOUR = 60u;
constexpr uint32 SECONDS_ONE_MINUTE = 60u;
constexpr uint32 SECONDS_ONE_HOUR = SECONDS_ONE_MINUTE * MINUTES_ONE_HOUR;
constexpr uint32 MILLISECONDS_ONE_SECOND = 1000u;
constexpr uint32 MICROSECONDS_ONE_MILLISECOND = 1000u;
constexpr uint32 MICROSECONDS_ONE_SECONDS = MILLISECONDS_ONE_SECOND * MICROSECONDS_ONE_MILLISECOND;
constexpr uint32 NANOSECONDS_ONE_MICROSECONDS = 1000u;
constexpr uint64 NANOSECONDS_ONE_MILLISECOND = NANOSECONDS_ONE_MICROSECONDS * MICROSECONDS_ONE_MILLISECOND;
class EventStat : public PerfStatBase {
public:
    EventStat() : PerfStatBase()
    {
        Init();
    }

    ~EventStat() override = default;

    inline void ReportEvent(bool ignoreCheckLevel = false)
    {
        if (!ignoreCheckLevel && m_level < PerfGlobalLevel::GetInstance().GetPerfLevel()) {
            return;
        }

        (void)m_counter.fetch_add(1);
    }

    inline uint64 GetEventsCount() const
    {
        return m_counter.load();
    }

    int32 Dump(char *dumpBuf, int32 bufIndex, const char *parent) const override;
    inline uint64 GetTotalCount() const
    {
        return m_counter + m_hisCounter;
    }

    void Init();
    void Reset() override;

    float Frequency() const;

private:
    std::atomic<uint64> m_counter{0};
    std::chrono::steady_clock::time_point m_startTime{};
    std::atomic<uint64> m_hisCounter{0};
};

class LatestNumberStat : public PerfStatBase {
public:
    LatestNumberStat() : PerfStatBase()
    {
        Init();
    }

    ~LatestNumberStat() override = default;

    inline void ReportLatestNumber(uint64 latestNumber)
    {
        if (m_level < PerfGlobalLevel::GetInstance().GetPerfLevel()) {
            return;
        }

        (void)m_counter.fetch_add(1);
        m_lastLatestNum.store(latestNumber);

        uint64 minLatestNum = m_minLatestNum.load();
        while (minLatestNum > latestNumber && !(m_minLatestNum.compare_exchange_weak(minLatestNum, latestNumber))) {
        }
        uint64 maxLatestNum = m_maxLatestNum.load();
        while (maxLatestNum < latestNumber && !(m_maxLatestNum.compare_exchange_weak(maxLatestNum, latestNumber))) {
        }
    }

    int32 Dump(char *dumpBuf, int32 bufIndex, const char *parent) const override;
    void Init();
    void Reset() override;

private:
    std::atomic<uint64> m_minLatestNum{0};
    std::atomic<uint64> m_maxLatestNum{0};
    std::atomic<uint64> m_lastLatestNum{0};
    std::atomic<uint64> m_counter{0};
};

using LsnStat = LatestNumberStat;

template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value, T>::type>
class NumericFeatureStat : public PerfStatBase {
public:
    NumericFeatureStat() : PerfStatBase()
    {
        Init();
    }

    ~NumericFeatureStat() override = default;

    inline void ReportFeature(T value, bool ignoreCheckLevel = false)
    {
        if (!ignoreCheckLevel && m_level < PerfGlobalLevel::GetInstance().GetPerfLevel()) {
            return;
        }

        (void)m_counter.fetch_add(1);
        (void)m_sum.fetch_add(value);

        T min = m_min.load();
        while (min > value && !(m_min.compare_exchange_weak(min, value))) {
        }
        T max = m_max.load();
        while (max < value && !(m_max.compare_exchange_weak(max, value))) {
        }
    }

    int32 Dump(char *dumpBuf, int32 bufIndex, const char *parent) const override
    {
        StorageReleasePanic(PERFLOG_NUMERIC_FORMAT<T> == nullptr, MODULE_HEAP, ErrMsg("Current format unknown"));
        T min = (m_min.load() == std::numeric_limits<T>::max() ? 0 : m_min.load());
        T max = (m_max.load() == std::numeric_limits<T>::lowest() ? 0 : m_max.load());

        int32 offset = bufIndex;
        offset = PerfLogMemcpy(dumpBuf, offset, PERF_ITEM_INDENT, m_indentLevel);
        if (parent != nullptr) {
            offset = PerfLogSprintf(dumpBuf, offset, PERFLOG_NUMERIC_FORMAT_WITH_PARENT<T>, m_name, m_id, parent,
                                    Average(), min, max, m_sum.load(), m_counter.load());
        } else {
            offset = PerfLogSprintf(dumpBuf, offset, PERFLOG_NUMERIC_FORMAT<T>, m_name, m_id, Average(), min, max,
                                    m_sum.load(), m_counter.load());
        }

        return offset - bufIndex;
    }

    inline void Init()
    {
        m_counter.store(0);
        m_min.store(std::numeric_limits<T>::max());
        m_max.store(std::numeric_limits<T>::lowest());
        m_sum.store(0);
        SetInited();
    }

    void Reset() override
    {
        uint64 counter = m_counter.load();
        m_counter -= counter;
        m_hisCounter += counter;
        m_min.store(std::numeric_limits<T>::max());
        m_max.store(std::numeric_limits<T>::lowest());
        T sum = m_sum.load();
        m_sum -= sum;
        m_hisSum += sum;
    }

    inline T GetMin() const
    {
        return m_min.load();
    }

    inline T GetMax() const
    {
        return m_max.load();
    }

    inline T GetSum() const
    {
        return m_sum.load();
    }

    inline T GetTotalSum() const
    {
        return m_sum + m_hisSum;
    }

    inline uint64 GetCount() const
    {
        return m_counter.load();
    }

    inline uint64 GetTotalCount() const
    {
        return m_counter + m_hisCounter;
    }

    inline uint64 GetAverage() const
    {
        return Average();
    }

protected:
    T Average() const
    {
        uint64 counter = m_counter.load();
        if (counter == 0) {
            return 0;
        }
        return m_sum.load() / counter;
    }

protected:
    std::atomic<T> m_min{0};
    std::atomic<T> m_max{0};
    std::atomic<T> m_sum{0};
    std::atomic<T> m_hisSum{0};
    std::atomic<uint64> m_counter{0};
    std::atomic<uint64> m_hisCounter{0};
};

template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value, T>::type>
class JobRateStat : public NumericFeatureStat<T> {
public:
    JobRateStat() : NumericFeatureStat<T>()
    {
        m_startTime = std::chrono::steady_clock::now();
    }

    ~JobRateStat() override = default;

    int32 Dump(char *dumpBuf, int32 bufIndex, const char *parent = nullptr) const override
    {
        float rate = GetRate();
        int32 offset = NumericFeatureStat<T>::Dump(dumpBuf, bufIndex, parent);
        offset += bufIndex;
        /* Replace '\n' with ' ' on previous character. */
        offset = this->PerfLogSprintf(dumpBuf, offset - 1, " rate=%.4f/sec\n", rate);

        return offset - bufIndex;
    }

    void Reset() override
    {
        NumericFeatureStat<T>::Reset();
        m_startTime = std::chrono::steady_clock::now();
    }

    inline float GetRate() const
    {
        std::chrono::milliseconds timeInterval =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_startTime);
        float rate = 0;
        if (timeInterval.count() != 0) {
            rate = (static_cast<float>(this->m_sum.load()) / static_cast<float>(timeInterval.count())) *
                   MILLISECONDS_ONE_SECOND;
        }
        return rate;
    }

private:
    std::chrono::steady_clock::time_point m_startTime{};
};

class LatencyStat : public NumericFeatureStat<uint64> {
public:
    LatencyStat() = default;
    ~LatencyStat() override = default;

    int32 Dump(char *dumpBuf, int32 bufIndex, const char *parent) const override;

    class Timer {
    public:
        explicit Timer(LatencyStat *stat, UNUSE_PARAM bool startNow = true, PerfLevel level = PerfLevel::RELEASE,
            bool ignoreCheckLevel = false)
            : m_stat(stat), m_level(level), m_ignoreCheckLevel(ignoreCheckLevel)
        {
#ifndef UT
            if (startNow) {
                Start();
            }
#endif
        }

        Timer() = default;

        ~Timer()
        {
            if (m_started) {
                End();
            }
        }

        DISALLOW_COPY_AND_MOVE(Timer);

        inline void Bind(LatencyStat *stat) noexcept
        {
            m_stat = stat;
        }

        inline void Start(UNUSE_PARAM bool enablePerf = true)
        {
#ifndef UT
            if (!enablePerf || m_started) {
                return;
            }

            if (unlikely(m_ignoreCheckLevel || m_level >= PerfGlobalLevel::GetInstance().GetPerfLevel())) {
                m_startTime = std::chrono::steady_clock::now();
                m_started = true;
            }
            m_latency = 0;
#endif
        }

        inline void End(UNUSE_PARAM LatencyStat *stat = nullptr) noexcept
        {
#ifndef UT
            if (!m_ignoreCheckLevel && m_level < PerfGlobalLevel::GetInstance().GetPerfLevel()) {
                return;
            }

            if (stat != nullptr) {
                Bind(stat);
            }

            if (m_started) {
                m_started = false;

                if (m_stat != nullptr) {
                    std::chrono::steady_clock::time_point endTime = std::chrono::steady_clock::now();
                    std::chrono::nanoseconds timeInterval =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - m_startTime);
                    m_latency = static_cast<uint64>(timeInterval.count());
                    m_stat->ReportFeature(m_latency, m_ignoreCheckLevel);
                }
            }
#endif
        }

        inline uint64 GetLatency() const
        {
            return m_latency;
        }

        inline uint64 GetLatencyUs() const
        {
            return m_latency / NANOSECONDS_ONE_MICROSECONDS;
        }

        inline uint64 GetCurrLatency() const
        {
            if (!m_started) {
                return 0;
            }
            std::chrono::steady_clock::time_point endTime = std::chrono::steady_clock::now();
            std::chrono::nanoseconds timeInterval =
                std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - m_startTime);
            return static_cast<uint64>(timeInterval.count());
        }

        uint64 GetCurrLatencyUs() const
        {
            return GetCurrLatency() / NANOSECONDS_ONE_MICROSECONDS;
        }

    private:
        LatencyStat *m_stat{nullptr};
        bool m_started{false};
        std::chrono::steady_clock::time_point m_startTime{};
        uint64 m_latency{0};
        PerfLevel m_level{PerfLevel::RELEASE};
        bool m_ignoreCheckLevel{false};
    };
};

template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value, T>::type>
class RangePartitionStat : public PerfStatBase {
public:
    RangePartitionStat() = default;
    ~RangePartitionStat() override
    {
        if (m_counters != nullptr) {
            Destroy();
        }
    }

    DISALLOW_COPY_AND_MOVE(RangePartitionStat);

    bool Init(DstoreMemoryContext memCtxPtr, uint32 bucketNum, T min, T max)
    {
        if (bucketNum == 0) {
            ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK, ErrMsg("RangePartitionStat init failed. Bucket num is 0."));
            return false;
        }

        if (max <= min) {
            ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK,
                   ErrMsg("RangePartitionStat init failed. Bucket max is not greater than min."));
            return false;
        }
        m_step = (max - min) / bucketNum;
        if (m_step == 0) {
            ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK,
                   ErrMsg("RangePartitionStat init failed. Bucket step is less than 1."));
            return false;
        }
        m_min = min;
        /*
         * It doesn't matter if m_max != max.
         * The Object (>= m_max) will fall into the last extra bucket.
         */
        m_max = m_step * bucketNum + m_min;
        /* There are bucket[< m_min] and bucket[>= m_max] except normal buckets. */
        m_bucketNum = bucketNum + RANGE_PARTITION_EXTRA_BUCKETS;
        m_counters = NEW_ARRAY(memCtxPtr) std::atomic<uint32>[m_bucketNum];
        if (m_counters == nullptr) {
            ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK,
                   ErrMsg("RangePartitionStat init failed. Can not allocate memory for m_counters."));
            return false;
        }
        SetInited();
        Reset();
        return true;
    }

    void Destroy() noexcept
    {
        DELETE_ARRAY_AND_RESET(m_counters);
    }

    inline void ReportFeature(T value)
    {
        if (m_level < PerfGlobalLevel::GetInstance().GetPerfLevel()) {
            return;
        }

        m_counters[GetBucketIndex(value)].fetch_add(1);
        m_totalNum.fetch_add(1);
    }

    inline T GetBucketIndex(T value) const
    {
        if (unlikely(value < m_min)) {
            return 0;
        }

        if (unlikely(value >= m_max)) {
            return m_bucketNum - 1;
        }

        return (value - m_min) / m_step + 1;
    }

    int32 Dump(char *dumpBuf, int32 bufIndex, const char *parent) const override
    {
        static_assert(PERFLOG_RANGEPART_ORIGIN_FORMAT<T> != nullptr);

        int32 offset = bufIndex;

        /* Item Header */
        offset = PerfLogMemcpy(dumpBuf, offset, PERF_ITEM_INDENT, m_indentLevel);
        if (parent != nullptr) {
            offset = PerfLogSprintf(dumpBuf, offset, "%s id=%u, parent=%s\n", m_name, m_id, parent);
        } else {
            offset = PerfLogSprintf(dumpBuf, offset, "%s id=%u\n", m_name, m_id);
        }

        /* bucket[0] */
        offset = PerfLogMemcpy(dumpBuf, offset, PERF_ITEM_INDENT, m_indentLevel + 1);
        offset = PerfLogSprintf(dumpBuf, offset, PERFLOG_RANGEPART_ORIGIN_FORMAT<T>, 0, m_min, m_counters[0].load());

        /* bucket[normal] */
        for (uint32 i = 1; i < m_bucketNum - 1; i++) {
            offset = PerfLogMemcpy(dumpBuf, offset, PERF_ITEM_INDENT, m_indentLevel + 1);
            offset = PerfLogSprintf(dumpBuf, offset, PERFLOG_RANGEPART_NORMAL_FORMAT<T>, i, (i - 1) * m_step + m_min,
                                    i * m_step + m_min, m_counters[i].load());
        }

        /* bucket[m_bucketNum - 1] */
        offset = PerfLogMemcpy(dumpBuf, offset, PERF_ITEM_INDENT, m_indentLevel + 1);
        offset = PerfLogSprintf(dumpBuf, offset, PERFLOG_RANGEPART_END_FORMAT<T>, m_bucketNum - 1, m_max,
                                m_counters[m_bucketNum - 1].load());

        T min;
        T max;
        uint32 count;
        (void)GetPercentileRange(STATIS_PERCENTILE_95TH, min, max, count);
        /* Percentile95 */
        offset = PerfLogMemcpy(dumpBuf, offset, PERF_ITEM_INDENT, m_indentLevel);
        offset = PerfLogSprintf(dumpBuf, offset, PERFLOG_RANGEPART_PERCENTILE_FORMAT<T>, STATIS_PERCENTILE_95TH, min,
                                max, count);

        /* Item Content */
        offset = PerfLogMemcpy(dumpBuf, offset, PERF_ITEM_INDENT, m_indentLevel);
        offset = PerfLogSprintf(dumpBuf, offset, "Counter quantity: " FMT_U64 "\n", m_totalNum.load());

        return offset - bufIndex;
    }

    void Reset() override
    {
        for (uint32 i = 0; i < m_bucketNum; i++) {
            m_counters[i].store(0);
        }

        m_totalNum.store(0);
    }

    bool GetPercentileRange(uint32 percentile, T &min, T &max, uint32 &count) const
    {
        constexpr uint32 hundredPercent = 100;
        if (percentile == 0 || percentile > hundredPercent) {
            ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK, ErrMsg("The percentile value must between (0, 100]."));

            return false;
        }
        /* Fetch the range max value as the percentile value. */
        uint64 destCount = percentile * m_totalNum.load() / hundredPercent;
        uint64 currSum = 0;
        uint32 index = 0;
        for (uint32 i = 0; i < m_bucketNum; i++) {
            currSum += m_counters[i].load();
            if (currSum >= destCount) {
                index = i;
                break;
            }
        }
        min = GetRangeMin(index);
        max = GetRangeMax(index);
        count = m_counters[index].load();
        return true;
    }

private:
    T GetRangeMin(uint32 index) const
    {
        if (index == 0) {
            return std::numeric_limits<T>::min();
        }
        /* The index 0 is not follow the m_step */
        return static_cast<T>(index - 1) * m_step + m_min;
    }

    T GetRangeMax(uint32 index) const
    {
        if (index == m_bucketNum - 1) {
            return std::numeric_limits<T>::max();
        }

        return static_cast<T>(index) * m_step + m_min;
    }

private:
    T m_min{0};
    T m_max{0};
    T m_step{0};
    uint32 m_bucketNum{0};
    std::atomic<uint32> *m_counters{nullptr};
    std::atomic<uint64> m_totalNum{0};
};

template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value, T>::type>
class ListPartitionStat : public PerfStatBase {
public:
    ListPartitionStat() = default;
    ~ListPartitionStat() override = default;

    DISALLOW_COPY_AND_MOVE(ListPartitionStat);

    template <typename... Args>
    bool Init(DstoreMemoryContext memCtxPtr, uint32 bucketNum, Args... args)
    {
        if (bucketNum == 0 || bucketNum != sizeof...(args)) {
            ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK,
                   ErrMsg("ListPartitionStat init failed. The Bucket number disaccord."));
            return false;
        }

        m_enumKeys = NEW_ARRAY(memCtxPtr) T[bucketNum];
        if (m_enumKeys == nullptr) {
            ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK,
                   ErrMsg("RangePartitionStat init failed. Can not allocate memory for m_enumKeys."));
            return false;
        }
        m_bucketNum = 0;
        SetEnum(args...);

        /* There is bucket[others] except normal buckets. */
        m_bucketNum++;
        m_counters = NEW_ARRAY(memCtxPtr) std::atomic<uint32>[m_bucketNum];
        if (m_counters == nullptr) {
            DELETE_ARRAY_AND_RESET(m_enumKeys);
            ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK,
                   ErrMsg("RangePartitionStat init failed. Can not allocate memory for m_counters."));
            return false;
        }

        Reset();
        SetInited();
        return true;
    }

    void Destroy()
    {
        DELETE_ARRAY_AND_RESET(m_enumKeys);
        DELETE_ARRAY_AND_RESET(m_counters);
    }

    inline void ReportFeature(T value)
    {
        if (m_level < PerfGlobalLevel::GetInstance().GetPerfLevel()) {
            return;
        }

        /* Default index: bucket[others] */
        int32 index = m_bucketNum - 1;
        for (int32 i = 0; i < m_bucketNum - 1; i++) {
            if (m_enumKeys[i] >= value) {
                index = i;
                break;
            }
        }
        m_counters[index].fetch_add(1);
        m_totalNum.fetch_add(1);
    }

    int32 Dump(char *dumpBuf, int32 bufIndex, const char *parent) const override
    {
        static_assert(PERFLOG_LISTPART_FORMAT<T> != nullptr);

        int32 offset = bufIndex;

        /* Item Header */
        offset = PerfLogMemcpy(dumpBuf, offset, PERF_ITEM_INDENT, m_indentLevel);
        if (parent != nullptr) {
            offset = PerfLogSprintf(dumpBuf, offset, "%s id=%u, parent=%s\n", m_name, m_id, parent);
        } else {
            offset = PerfLogSprintf(dumpBuf, offset, "%s id=%u\n", m_name, m_id);
        }

        for (int32 i = 0; i < m_bucketNum - 1; i++) {
            /* bucket[normal] */
            offset = PerfLogMemcpy(dumpBuf, offset, PERF_ITEM_INDENT, m_indentLevel + 1);
            offset = PerfLogSprintf(dumpBuf, offset, PERFLOG_LISTPART_FORMAT<T>, m_enumKeys[i], m_counters[i].load());
        }

        /* bucket[others] */
        offset = PerfLogMemcpy(dumpBuf, offset, PERF_ITEM_INDENT, m_indentLevel + 1);
        offset = PerfLogSprintf(dumpBuf, offset, "bucket 'others': cnt=%u\n", m_counters[m_bucketNum - 1].load());

        /* Item Content */
        offset = PerfLogMemcpy(dumpBuf, offset, PERF_ITEM_INDENT, m_indentLevel);
        offset = PerfLogSprintf(dumpBuf, offset, "Counter quantity: " FMT_U64 "\n", m_totalNum.load());

        return offset - bufIndex;
    }

    void Reset() override
    {
        for (int32 i = 0; i < m_bucketNum; i++) {
            m_counters[i].store(0);
        }

        m_totalNum.store(0);
    }

private:
    /* Unpack args... */
    template <typename... Args>
    inline void SetEnum(T enumValue, Args... args)
    {
        m_enumKeys[m_bucketNum++] = enumValue;
        SetEnum(args...);
    }

    inline void SetEnum(T enumValue)
    {
        m_enumKeys[m_bucketNum++] = enumValue;
    }

private:
    int32 m_bucketNum{0};
    T *m_enumKeys{nullptr};
    std::atomic<uint32> *m_counters{nullptr};
    std::atomic<uint64> m_totalNum{0};
};

template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value, T>::type>
class TimePartitionStat : public PerfStatBase {
public:
    TimePartitionStat() = default;
    ~TimePartitionStat() override
    {
        Destroy();
    }

    DISALLOW_COPY_AND_MOVE(TimePartitionStat);

    void Destroy() noexcept
    {
        DELETE_ARRAY_AND_RESET(m_counters);
    }

    bool Init(UNUSE_PARAM DstoreMemoryContext memCtxPtr, uint32 bucketNum, uint32 timePartitionLengthMs)
    {
        if (bucketNum == 0 || timePartitionLengthMs < bucketNum) {
            ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK,
                   ErrMsg("TimePartitionStat init failed. The timePartitionLengthUs less than bucketNum."));
            return false;
        }

        m_timePartitionLengthMs = timePartitionLengthMs;
        m_bucketNum = bucketNum;
        /* round up */
        m_timeInterval = (m_timePartitionLengthMs + m_bucketNum - 1) / m_bucketNum;
        m_counters = NEW_ARRAY(memCtxPtr) NumericFeatureStat<T>[m_bucketNum];
        if (m_counters == nullptr) {
            ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK,
                   ErrMsg("TimePartitionStat init failed. Can not allocate memory for m_counters."));
            return false;
        }

        for (uint32 i = 0; i < m_bucketNum; ++i) {
            m_counters[i].SetPerfLevel(this->m_level);
        }
        m_overallCounter.SetPerfLevel(this->m_level);

        Reset();
        SetInited();
        return true;
    }

    inline void ReportFeature(T value)
    {
        if (m_level < PerfGlobalLevel::GetInstance().GetPerfLevel()) {
            return;
        }
        m_overallCounter.ReportFeature(value);
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        uint64 timeIntervalMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_startTime).count();
        if (timeIntervalMs >= m_timePartitionLengthMs) {
            return;
        }
        uint64 bucketIndex = timeIntervalMs / m_timeInterval;
        m_counters[bucketIndex].ReportFeature(value);
    }

    int32 Dump(char *dumpBuf, int32 bufIndex, const char *parent) const override
    {
        static_assert(PERFLOG_TIMEPART_NUMERIC_FORMAT<T> != nullptr);
        int32 offset = bufIndex;
        offset = PerfLogMemcpy(dumpBuf, offset, PERF_ITEM_INDENT, m_indentLevel);
        if (m_overallCounter.GetCount() == 0) {
            if (parent != nullptr) {
                offset = PerfLogSprintf(dumpBuf, offset, "%s id=%u, parent=%s: interval=%u (ms), no stat collected.\n",
                                        m_name, m_id, parent, m_timeInterval);
            } else {
                offset = PerfLogSprintf(dumpBuf, offset, "%s id=%u: interval=%u (ms), no stat collected.\n", m_name,
                                        m_id, m_timeInterval);
            }
            return offset - bufIndex;
        } else {
            if (parent != nullptr) {
                offset = PerfLogSprintf(dumpBuf, offset, "%s id=%u, parent=%s: interval=%u (ms)\n", m_name, m_id,
                                        parent, m_timeInterval);
            } else {
                offset = PerfLogSprintf(dumpBuf, offset, "%s id=%u: interval=%u (ms)\n", m_name, m_id, m_timeInterval);
            }
        }
        for (uint32 i = 0; i < m_bucketNum; ++i) {
            T avg = (m_counters[i].GetCount() == 0) ? 0 : m_counters[i].GetSum() / m_counters[i].GetCount();
            offset = PerfLogMemcpy(dumpBuf, offset, PERF_ITEM_INDENT, m_indentLevel + 1);
            offset = PerfLogSprintf(dumpBuf, offset, PERFLOG_TIMEPART_NUMERIC_FORMAT<T>, avg, GetMin(m_counters[i]),
                                    GetMax(m_counters[i]), m_counters[i].GetCount());
        }
        return offset - bufIndex;
    }

    void Reset() override
    {
        m_startTime = std::chrono::steady_clock::now();

        for (uint32 i = 0; i < m_bucketNum; i++) {
            m_counters[i].Reset();
        }
        m_overallCounter.Reset();
    }

private:
    T GetMin(const NumericFeatureStat<T> &stat) const
    {
        return (stat.GetMin() == std::numeric_limits<T>::max() ? 0 : stat.GetMin());
    }

    T GetMax(const NumericFeatureStat<T> &stat) const
    {
        return (stat.GetMax() == std::numeric_limits<T>::lowest() ? 0 : stat.GetMax());
    }

    T GetAvg(const NumericFeatureStat<T> &stat) const
    {
        return (stat.GetCount() == 0) ? 0 : stat.GetSum() / stat.GetCount();
    }

private:
    uint32 m_bucketNum{0};
    NumericFeatureStat<T> *m_counters{nullptr};
    NumericFeatureStat<T> m_overallCounter{};
    uint32 m_timePartitionLengthMs{0};
    std::chrono::steady_clock::time_point m_startTime{};
    uint32 m_timeInterval{MAX_UINT32};
};
}  // namespace DSTORE
#endif /* COMMON_PERF_ITEM_H */
