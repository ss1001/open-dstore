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

#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <fstream>
#include "tpcc_server/tpcc_test_server.h"
#include "framework/dstore_instance_interface.h"
#include "framework/dstore_thread_interface.h"
#include "framework/dstore_vfs_interface.h"
#include "framework/dstore_config_interface.h"
#include "common/log/dstore_log.h"
#include "table_handler.h"
#include "cjson/cJSON.h"
#include "common/memory/dstore_mctx.h"
#include "framework/dstore_instance.h"
#include "framework/dstore_session_interface.h"
#include "config/dstore_vfs_config.h"
#include "pdb/dstore_pdb_interface.h"

using namespace DSTORE;
 
const std::string GUC_CONFIG_PATH("guc.json");
static StorageGUC g_guc;
static bool g_isReadGuc = false;

int RemoveDir(const char *dirPath)
{
    char *curDir = (char *)".";
    char *upperDir = (char *)"..";
    DIR *dirHandle = nullptr;
    dirent *dirContext = nullptr;
    struct stat dirStat;
    char subDirPath[VFS_FILE_PATH_MAX_LEN];
    if (access(dirPath, F_OK) != 0) {
        return 0;
    }

    if (stat(dirPath, &dirStat) < 0) {
        assert(0);
    }
    if (S_ISDIR(dirStat.st_mode)) {
        dirHandle = opendir(dirPath);
        while ((dirContext = readdir(dirHandle)) != nullptr) {
            if ((strcmp(dirContext->d_name, curDir) == 0) || strcmp(dirContext->d_name, upperDir) == 0) {
                continue;
            }
            errno_t rc = sprintf_s(subDirPath, VFS_FILE_PATH_MAX_LEN, "%s/%s", dirPath, dirContext->d_name);
            storage_securec_check_ss(rc);
            RemoveDir(subDirPath);
        }
        closedir(dirHandle);
        rmdir(dirPath);
    } else {
        remove(dirPath);
    }
    return 0;
}

void LoadGucConfig()
{
    std::ifstream configFile(GUC_CONFIG_PATH);
    if (!configFile.is_open()) {
        std::cout << "Could not open config file: " << GUC_CONFIG_PATH << std::endl;
        exit(1);
    }
    g_isReadGuc = true;
    std::string configMsg((std::istreambuf_iterator<char>(configFile)), std::istreambuf_iterator<char>());
    std::cout << "--------------------"
              << "GUC Config in " << GUC_CONFIG_PATH << "--------------------" << std::endl;

    
    cJSON *configJson = cJSON_Parse(configMsg.c_str());
    g_guc.selfNodeId = cJSON_GetObjectItem(configJson, "selfNodeId")->valueint;
    std::cout << "selfNodeId: " << g_guc.selfNodeId << std::endl;

    g_guc.buffer = cJSON_GetObjectItem(configJson, "buffer")->valueint;
    std::cout << "buffer: " << g_guc.buffer << std::endl;

    g_guc.bufferLruPartition = cJSON_GetObjectItem(configJson, "bufferLruPartition")->valueint;
    std::cout << "bufferLruPartition: " << g_guc.bufferLruPartition << std::endl;

    g_guc.checkpointTimeout = cJSON_GetObjectItem(configJson, "checkpointTimeout")->valueint;
    std::cout << "checkpointTimeout: " << g_guc.checkpointTimeout << std::endl;

    g_guc.defaultIsolationLevel = cJSON_GetObjectItem(configJson, "defaultIsolationLevel")->valueint;
    std::cout << "defaultIsolationLevel: " << g_guc.defaultIsolationLevel << std::endl;

    g_guc.maintenanceWorkMem = cJSON_GetObjectItem(configJson, "maintenanceWorkMem")->valueint;
    std::cout << "maintenanceWorkMem: " << g_guc.maintenanceWorkMem << std::endl;

    char *tmpStrValue = cJSON_GetObjectItem(configJson, "dataDir")->valuestring;
    g_guc.dataDir = (strlen(tmpStrValue) == 0 ? nullptr : tmpStrValue);
    std::cout << "dataDir: " << (g_guc.dataDir == nullptr ? "nullptr" : g_guc.dataDir) << std::endl;

    g_guc.ncores = cJSON_GetObjectItem(configJson, "ncores")->valueint;
    std::cout << "ncores: " << g_guc.ncores << std::endl;

    g_guc.logMinMessages = cJSON_GetObjectItem(configJson, "logMinMessages")->valueint;
    std::cout << "logMinMessages: " << g_guc.logMinMessages << std::endl;

    g_guc.foldPeriod = cJSON_GetObjectItem(configJson, "foldPeriod")->valueint;
    std::cout << "foldPeriod: " << g_guc.foldPeriod << std::endl;

    g_guc.foldThreshold = cJSON_GetObjectItem(configJson, "foldThreshold")->valueint;
    std::cout << "foldThreshold: " << g_guc.foldThreshold << std::endl;

    g_guc.foldLevel = cJSON_GetObjectItem(configJson, "foldLevel")->valueint;
    std::cout << "foldLevel: " << g_guc.foldLevel << std::endl;

    g_guc.csnAssignmentIncrement = cJSON_GetObjectItem(configJson, "csnAssignmentIncrement")->valueint;
    std::cout << "csnAssignmentIncrement: " << g_guc.csnAssignmentIncrement << std::endl;

    tmpStrValue = cJSON_GetObjectItem(configJson, "moduleLoggingConfigure")->valuestring;
    g_guc.moduleLoggingConfigure = (strlen(tmpStrValue) == 0 ? nullptr : tmpStrValue);
    std::cout << "moduleLoggingConfigure: "
              << (g_guc.moduleLoggingConfigure == nullptr ? "nullptr" : g_guc.moduleLoggingConfigure) << std::endl;

    g_guc.lockHashTableSize = cJSON_GetObjectItem(configJson, "lockHashTableSize")->valueint;
    std::cout << "lockHashTableSize: " << g_guc.lockHashTableSize << std::endl;

    g_guc.lockTablePartitionNum = cJSON_GetObjectItem(configJson, "lockTablePartitionNum")->valueint;
    std::cout << "lockTablePartitionNum: " << g_guc.lockTablePartitionNum << std::endl;

    g_guc.enableLazyLock = cJSON_GetObjectItem(configJson, "enableLazyLock")->valueint;
    std::cout << "enableLazyLock: " << ((g_guc.enableLazyLock == 1) ? "true" : "false") << std::endl;

    tmpStrValue = cJSON_GetObjectItem(configJson, "vfsTenantIsolationConfigPath")->valuestring;
    g_guc.vfsTenantIsolationConfigPath = (strlen(tmpStrValue) == 0 ? nullptr : tmpStrValue);
    std::cout << "vfsTenantIsolationConfigPath: "
              << (g_guc.vfsTenantIsolationConfigPath == nullptr ? "nullptr" : g_guc.vfsTenantIsolationConfigPath)
              << std::endl;

    g_guc.updateCsnMinInterval = cJSON_GetObjectItem(configJson, "updateCsnMinInterval")->valueint;
    std::cout << "updateCsnMinInterval: " << g_guc.updateCsnMinInterval << std::endl;

    g_guc.numObjSpaceMgrWorkers = cJSON_GetObjectItem(configJson, "numObjSpaceMgrWorkers")->valueint;
    std::cout << "numObjSpaceMgrWorkers: " << g_guc.numObjSpaceMgrWorkers << std::endl;

    g_guc.minFreePagePercentageThreshold1 =
        cJSON_GetObjectItem(configJson, "minFreePagePercentageThreshold1")->valueint;
    std::cout << "minFreePagePercentageThreshold1: " << g_guc.minFreePagePercentageThreshold1 << std::endl;

    g_guc.minFreePagePercentageThreshold2 =
        cJSON_GetObjectItem(configJson, "minFreePagePercentageThreshold2")->valueint;
    std::cout << "minFreePagePercentageThreshold2: " << g_guc.minFreePagePercentageThreshold2 << std::endl;

    g_guc.probOfExtensionThreshold = cJSON_GetObjectItem(configJson, "probOfExtensionThreshold")->valueint;
    std::cout << "probOfExtensionThreshold: " << g_guc.probOfExtensionThreshold << std::endl;

    g_guc.recoveryWorkerNum = cJSON_GetObjectItem(configJson, "recoveryWorkerNum")->valueint;
    std::cout << "recoveryWorkerNum: " << g_guc.recoveryWorkerNum << std::endl;

    g_guc.synchronousCommit = cJSON_GetObjectItem(configJson, "synchronousCommit")->valueint;
    std::cout << "synchronousCommit: " << ((g_guc.synchronousCommit == 1) ? "true" : "false") << std::endl;

    g_guc.walStreamCount = cJSON_GetObjectItem(configJson, "walStreamCount")->valueint;
    std::cout << "walStreamCount: " << g_guc.walStreamCount << std::endl;

    g_guc.walFileNumber = cJSON_GetObjectItem(configJson, "walFileNumber")->valueint;
    std::cout << "walFileNumber: " << g_guc.walFileNumber << std::endl;

    tmpStrValue = cJSON_GetObjectItem(configJson, "walFileSize")->valuestring;
    g_guc.walFileSize = std::stoll(tmpStrValue);
    std::cout << "walFileSize: " << g_guc.walFileSize << std::endl;

    g_guc.walBuffers = cJSON_GetObjectItem(configJson, "walBuffers")->valueint;
    std::cout << "walBuffers: " << g_guc.walBuffers << std::endl;

    tmpStrValue = cJSON_GetObjectItem(configJson, "walReadBufferSize")->valuestring;
    g_guc.walReadBufferSize = std::stoll(tmpStrValue);
    std::cout << "walReadBufferSize: " << g_guc.walReadBufferSize << std::endl;

    tmpStrValue = cJSON_GetObjectItem(configJson, "walRedoBufferSize")->valuestring;
    g_guc.walRedoBufferSize = std::stoll(tmpStrValue);
    std::cout << "walRedoBufferSize: " << g_guc.walRedoBufferSize << std::endl;

    g_guc.walwriterCpuBind = cJSON_GetObjectItem(configJson, "walwriterCpuBind")->valueint;
    std::cout << "walwriterCpuBind: " << g_guc.walwriterCpuBind << std::endl;

    g_guc.redoBindCpuAttr = cJSON_GetObjectItem(configJson, "redoBindCpuAttr")->valuestring;
    std::cout << "redoBindCpuAttr: " << g_guc.redoBindCpuAttr << std::endl;

    g_guc.numaNodeNum = cJSON_GetObjectItem(configJson, "numaNodeNum")->valueint;
    std::cout << "numaNodeNum: " << static_cast<int>(g_guc.numaNodeNum) << std::endl;

    g_guc.disableBtreePageRecycle = cJSON_GetObjectItem(configJson, "disableBtreePageRecycle")->valueint;
    std::cout << "disableBtreePageRecycle: " << ((g_guc.disableBtreePageRecycle == 1) ? "true" : "false") << std::endl;

    g_guc.deadlockTimeInterval = cJSON_GetObjectItem(configJson, "deadlockTimeInterval")->valueint;
    std::cout << "deadlockTimeInterval: " << g_guc.deadlockTimeInterval << std::endl;

    g_guc.recycleFsmTimeInterval = cJSON_GetObjectItem(configJson, "recycleFsmTimeInterval")->valueint;
    std::cout << "recycleFsmTimeInterval: " << g_guc.recycleFsmTimeInterval << std::endl;

    g_guc.probOfUpdateFsmTimestamp = cJSON_GetObjectItem(configJson, "probOfUpdateFsmTimestamp")->valueint;
    std::cout << "probOfUpdateFsmTimestamp: " << g_guc.probOfUpdateFsmTimestamp << std::endl;

    g_guc.probOfRecycleFsm = cJSON_GetObjectItem(configJson, "probOfRecycleFsm")->valueint;
    std::cout << ".probOfRecycleFsm: " << g_guc.probOfRecycleFsm << std::endl;

    g_guc.probOfRecycleBtree = cJSON_GetObjectItem(configJson, "probOfRecycleBtree")->valueint;
    std::cout << ".probOfRecycleBtree: " << g_guc.probOfRecycleBtree << std::endl;

    g_guc.distLockMaxRingSize = cJSON_GetObjectItem(configJson, "distLockMaxRingSize")->valueint;
    std::cout << "distLockMaxRingSize: " << g_guc.distLockMaxRingSize << std::endl;

    g_guc.csnMode = static_cast<DSTORE::CsnMode>(cJSON_GetObjectItem(configJson, "csnMode")->valueint);
    std::cout << "csnMode: " << static_cast<int>(g_guc.csnMode) << std::endl;

    g_guc.ctrlPlanePort = cJSON_GetObjectItem(configJson, "ctrlPlanePort")->valueint;
    std::cout << "ctrlPlanePort: " << g_guc.ctrlPlanePort << std::endl;

    g_guc.rdmaGidIndex = cJSON_GetObjectItem(configJson, "rdmaGidIndex")->valueint;
    std::cout << "rdmaGidIndex: " << static_cast<int>(g_guc.rdmaGidIndex) << std::endl;

    g_guc.rdmaIbPort = cJSON_GetObjectItem(configJson, "rdmaIbPort")->valueint;
    std::cout << "rdmaIbPort: " << static_cast<int>(g_guc.rdmaIbPort) << std::endl;

    g_guc.pdReadAuthResetPeriod = cJSON_GetObjectItem(configJson, "pdReadAuthResetPeriod")->valueint;
    std::cout << "pdReadAuthResetPeriod: " << g_guc.pdReadAuthResetPeriod << std::endl;

    g_guc.csnThreadBindCpu = cJSON_GetObjectItem(configJson, "csnThreadBindCpu")->valueint;
    std::cout << "csnThreadBindCpu: " << g_guc.csnThreadBindCpu << std::endl;

    tmpStrValue = cJSON_GetObjectItem(configJson, "commConfigStr")->valuestring;
    g_guc.commConfigStr = (strlen(tmpStrValue) == 0 ? nullptr : tmpStrValue);
    std::cout << "commConfigStr: " << (g_guc.commConfigStr == nullptr ? "nullptr" : g_guc.commConfigStr) << std::endl;

    g_guc.commThreadMin = cJSON_GetObjectItem(configJson, "commThreadMin")->valueint;
    std::cout << "commThreadMin: " << g_guc.commThreadMin << std::endl;
    g_guc.commThreadMax = cJSON_GetObjectItem(configJson, "commThreadMax")->valueint;
    std::cout << "commThreadMax: " << g_guc.commThreadMax << std::endl;
    g_guc.clusterId = cJSON_GetObjectItem(configJson, "clusterId")->valueint;
    std::cout << "clusterId: " << g_guc.clusterId << std::endl;

    tmpStrValue = cJSON_GetObjectItem(configJson, "memberView")->valuestring;
    g_guc.memberView = (strlen(tmpStrValue) == 0 ? nullptr : tmpStrValue);
    std::cout << "memberView: " << (g_guc.memberView == nullptr ? "nullptr" : g_guc.memberView) << std::endl;

    tmpStrValue = cJSON_GetObjectItem(configJson, "commProtocolTypeStr")->valuestring;
    g_guc.commProtocolTypeStr = (strlen(tmpStrValue) == 0 ? nullptr : tmpStrValue);
    std::cout << "commProtocolTypeStr: "
              << (g_guc.commProtocolTypeStr == nullptr ? "nullptr" : g_guc.commProtocolTypeStr) << std::endl;

    g_guc.commProtocolType = cJSON_GetObjectItem(configJson, "commProtocolType")->valueint;
    std::cout << "commProtocolType: " << g_guc.commProtocolType << std::endl;

    g_guc.globalClockAdjustWaitTimeUs = cJSON_GetObjectItem(configJson, "globalClockAdjustWaitTimeUs")->valueint;
    std::cout << "globalClockAdjustWaitTimeUs: " << g_guc.globalClockAdjustWaitTimeUs << std::endl;

    g_guc.globalClockSyncIntervalMs = cJSON_GetObjectItem(configJson, "globalClockSyncIntervalMs")->valueint;
    std::cout << "globalClockSyncIntervalMs: " << g_guc.globalClockSyncIntervalMs << std::endl;

    g_guc.gclockOverlapWaitTimeOptimization =
        cJSON_GetObjectItem(configJson, "gclockOverlapWaitTimeOptimization")->valueint;
    std::cout << "gclockOverlapWaitTimeOptimization: "
              << ((g_guc.gclockOverlapWaitTimeOptimization == 1) ? "true" : "false") << std::endl;

    g_guc.enableQuickStartUp = cJSON_GetObjectItem(configJson, "enableQuickStartUp")->valueint;
    std::cout << "enableQuickStartUp: " << ((g_guc.enableQuickStartUp == 1) ? "true" : "false") << std::endl;

    g_guc.defaultHeartbeatTimeoutInterval =
        cJSON_GetObjectItem(configJson, "defaultHeartbeatTimeoutInterval")->valueint;
    std::cout << "defaultHeartbeatTimeoutInterval: " << g_guc.defaultHeartbeatTimeoutInterval << std::endl;

    g_guc.defaultWalSizeThreshold = cJSON_GetObjectItem(configJson, "defaultWalSizeThreshold")->valueint;
    std::cout << "defaultWalSizeThreshold: " << g_guc.defaultWalSizeThreshold << std::endl;

    g_guc.bgDiskWriterSlaveNum = cJSON_GetObjectItem(configJson, "bgDiskWriterSlaveNum")->valueint;
    std::cout << "bgDiskWriterSlaveNum: " << static_cast<int>(g_guc.bgDiskWriterSlaveNum) << std::endl;

    g_guc.bgPageWriterSleepMilliSecond = cJSON_GetObjectItem(configJson, "bgPageWriterSleepMilliSecond")->valueint;
    std::cout << "bgPageWriterSleepMilliSecond: " << static_cast<int>(g_guc.bgPageWriterSleepMilliSecond) << std::endl;

    g_guc.walThrottlingSize = cJSON_GetObjectItem(configJson, "walThrottlingSize")->valueint;
    std::cout << "walThrottlingSize: " << static_cast<int>(g_guc.walThrottlingSize) << std::endl;

    g_guc.maxIoCapacityKb = cJSON_GetObjectItem(configJson, "maxIoCapacityKb")->valueint;
    std::cout << "maxIoCapacityKb: " << static_cast<int>(g_guc.maxIoCapacityKb) << std::endl;

    tmpStrValue = cJSON_GetObjectItem(configJson, "bgWalWriterMinBytes")->valuestring;
    g_guc.bgWalWriterMinBytes = std::stoll(tmpStrValue);
    std::cout << "bgWalWriterMinBytes: " << g_guc.bgWalWriterMinBytes << std::endl;

    tmpStrValue = cJSON_GetObjectItem(configJson, "walEachWriteLenghthLimit")->valuestring;
    g_guc.walEachWriteLenghthLimit = std::stoll(tmpStrValue);
    std::cout << "walEachWriteLenghthLimit: " << g_guc.walEachWriteLenghthLimit << std::endl;

    cJSON *item = cJSON_GetObjectItem(configJson, "enablePointGetFastPath");
    g_guc.SetEnablePointGetFastPath((item != nullptr) ? item->valueint : true);
    std::cout << "enablePointGetFastPath: " << g_guc.IsPointGetFastPathEnabled() << std::endl;

    g_guc.tenantConfig = new TenantConfig;
    assert(g_guc.tenantConfig != nullptr);
    assert(memset_s(g_guc.tenantConfig, sizeof(TenantConfig), 0, sizeof(TenantConfig)) == EOK);
    tmpStrValue = cJSON_GetObjectItem(configJson, "startConfigPath")->valuestring;
    if (strlen(tmpStrValue) > 0) {
        RetStatus ret = TenantConfigInterface::GetTenantConfig(tmpStrValue, g_guc.tenantConfig);
        StorageReleasePanic(STORAGE_FUNC_FAIL(ret), DSTORE::MODULE_FRAMEWORK, ErrMsg("GetTenantConfig fail."));
    }
    configFile.close();
    std::cout << "tmpStrValue = " << tmpStrValue << std::endl;
    std::cout << "config.storageConfig.clientLibPath = " << g_guc.tenantConfig->storageConfig.clientLibPath << std::endl;
    std::cout << std::endl;
}

void TpccStorageInstance::Init()
{
    LoadGucConfig();

    char utTopDir[VFS_FILE_PATH_MAX_LEN] = {'0'};
    getcwd(utTopDir, VFS_FILE_PATH_MAX_LEN);

    char dataDir[VFS_FILE_PATH_MAX_LEN] = {'0'};
    __attribute__((__unused__)) int rc = sprintf_s(dataDir, VFS_FILE_PATH_MAX_LEN, "%s/tpccdir/", utTopDir);
    storage_securec_check_ss(rc);

    char dstoreDir[VFS_FILE_PATH_MAX_LEN] = {'0'};
    rc = sprintf_s(dstoreDir, VFS_FILE_PATH_MAX_LEN, "%s/%s/", dataDir, DSTORE::BASE_DIR);
    storage_securec_check_ss(rc);

    char pdbMetaDataPath[VFS_FILE_PATH_MAX_LEN] = {'0'};
    rc = sprintf_s(pdbMetaDataPath, VFS_FILE_PATH_MAX_LEN, "%s/%s/", dataDir, "metadata");
    storage_securec_check_ss(rc);

    char walPath[VFS_FILE_PATH_MAX_LEN] = {'0'};
    rc = sprintf_s(walPath, VFS_FILE_PATH_MAX_LEN, "%s/%s/", dataDir, "dstore_wal");
    storage_securec_check_ss(rc);

    RemoveDir(dataDir);
    __attribute__((__unused__)) int ret = mkdir(dataDir, 0777);
    assert(ret != -1);
    chdir(dataDir);
    ret = mkdir(dstoreDir, 0777);
    assert(ret != -1);

    bool flag = VfsInterface::ModuleInitialize();
    StorageReleasePanic(!flag, DSTORE::MODULE_FRAMEWORK, ErrMsg("ModuleInitialize fail."));
    VfsInterface::SetupTenantIsoland(g_guc.tenantConfig, dstoreDir);
    RetStatus retStatus = VfsInterface::CreateTenantDefaultVfs(g_guc.tenantConfig);
    StorageReleasePanic(STORAGE_FUNC_FAIL(retStatus), DSTORE::MODULE_FRAMEWORK, ErrMsg("CreateTenantDefaultVfs fail."));

    ret = mkdir(pdbMetaDataPath, 0777);
    assert(ret != -1);
    ret = mkdir(walPath, 0777);
    assert(ret != -1);

    StorageGUC guc = g_guc;
    if (guc.dataDir == nullptr) {
        guc.dataDir = dataDir;
    }
    guc.recoveryWorkerNum = 1;

    std::string tpccLogFileName = dataDir;
    tpccLogFileName = tpccLogFileName + "/tpcc.log";
    InitLogAdapterInstance(guc.logMinMessages, tpccLogFileName.c_str(), guc.foldPeriod, guc.foldThreshold,
                           guc.foldLevel);

    g_instance = StorageInstanceInterface::Create(DSTORE::StorageInstanceType::SINGLE);
    g_instance->InitWorkingVersionNum(&GRAND_VERSION_NUM);

    SetDefaultPdbId(PDB_TEMPLATE1_ID);
    ThreadContextInterface *thrd = ThreadContextInterface::Create();
    if (STORAGE_VAR_NULL(thrd)) {
        ErrLog(DSTORE_ERROR, MODULE_COMMON, ErrMsg("Failed to create thread context for tpcc thread."));
        return;
    }
    (void)thrd->InitializeBasic();
    StorageSession *sc = CreateStorageSession(1ULL);
    thrd->AttachSessionToThread(sc);
    (void)g_instance->Bootstrap(&guc);
    (void)thrd->InitStorageContext(g_defaultPdbId);
    g_instance->AddVisibleThread(thrd, g_defaultPdbId);
    CreateTemplateTablespace(DSTORE::g_defaultPdbId);
    CreateUndoMapSegment(DSTORE::g_defaultPdbId);

    (void)thrd->InitTransactionRuntime(g_defaultPdbId, nullptr, nullptr);
    InitVfsClientHandles();
}

void TpccStorageInstance::Start(Oid allocMaxRelOid, DSTORE::StorageInstanceType storageType, NodeId selfNodeId)
{
    /* in tpcctest, we does not create rootpdb in bootstrap, use template run tpccteset */
    storageType = DSTORE::StorageInstanceType::SINGLE;
    SetDefaultPdbId(PDB_TEMPLATE1_ID);
    char dataDir[VFS_FILE_PATH_MAX_LEN] = {'0'};
    getcwd(dataDir, VFS_FILE_PATH_MAX_LEN);
    if (strstr(dataDir, "tpccdir") == NULL) {
        __attribute__((__unused__)) int rc = strcat_s(dataDir, VFS_FILE_PATH_MAX_LEN, "/tpccdir");
        assert(rc == 0);
    }
    if (!g_isReadGuc) {
        LoadGucConfig();
    }
    StorageGUC guc = g_guc;
    if (guc.dataDir == nullptr) {
        guc.dataDir = dataDir;
    }
    chdir(guc.dataDir);
    guc.selfNodeId = selfNodeId;

    std::string tpccLogFileName = guc.dataDir;
    tpccLogFileName = tpccLogFileName + "/node_" + std::to_string(selfNodeId) + ".log";
    InitLogAdapterInstance(guc.logMinMessages, tpccLogFileName.c_str(), guc.foldPeriod, guc.foldThreshold,
                           guc.foldLevel);
    g_instance = StorageInstanceInterface::Create(storageType);
    g_instance->InitWorkingVersionNum(&GRAND_VERSION_NUM);
    ThreadContextInterface *thrd = ThreadContextInterface::Create();
    if (STORAGE_VAR_NULL(thrd)) {
        ErrLog(DSTORE_ERROR, MODULE_COMMON, ErrMsg("Failed to create thread context for tpcc thread start."));
        return;
    }
    (void)thrd->InitializeBasic();
    StorageSession *sc = CreateStorageSession(1ULL);
    thrd->AttachSessionToThread(sc);
    (void)g_instance->StartupInstance(&guc);
    (void)thrd->InitStorageContext(g_defaultPdbId);

    (void)thrd->InitTransactionRuntime(g_defaultPdbId, nullptr, nullptr);
    simulator = new StorageTableContext(g_instance, allocMaxRelOid);
}
void TpccStorageInstance::InitFinished()
{
    StoragePdbInterface::FlushAllDirtyPages(g_defaultPdbId);
    StorageSession *sc = thrd->GetSession();
    g_instance->UnregisterThread();
    g_instance->BootstrapDestroy();
    g_instance->BootstrapResDestroy();
    StorageInstanceInterface::DestoryInstance();
    g_instance = nullptr;
    CleanUpSession(sc);
    StopLogAdapterInstance();
}

void TpccStorageInstance::Stop(uint32_t nodeNum, bool *state)
{
    StoragePdbInterface::FlushAllDirtyPages(g_defaultPdbId);
    StorageSession *sc = thrd->GetSession();
    thrd->DetachSessionFromThread();
    DestroyDataSimulationContext();
    /* STEP1: Stop to accept new connection. */
    g_instance->StopAcceptNewConnection();

    state[g_storageInstance->GetGuc()->selfNodeId - 1] = true;
    WaitAllNodeReady(nodeNum, state);

    g_instance->ShutdownInstance();
    StorageInstanceInterface::DestoryInstance();
    g_instance = nullptr;
    CleanUpSession(sc);
    StopLogAdapterInstance();
    if (g_guc.tenantConfig) {
        delete g_guc.tenantConfig;
        g_guc.tenantConfig = nullptr;
    }
}

void TpccStorageInstance::DestroyDataSimulationContext()
{
    if (simulator != nullptr) {
        simulator->Destory();
        delete simulator;
        simulator = nullptr;
    }
}

void WaitAllNodeReady(uint32_t nodeNum, bool *state)
{
    if (nodeNum == 1 && state != nullptr) {
        state[0] = true;
        return;
    }
    const int sleepTime = 1000;
    while (true) {
        uint32_t i = 0;
        for (; i < nodeNum; ++i) {
            if (!state[i]) {
                break;
            }
        }
        if (i == nodeNum && state[nodeNum - 1]) {
            break;
        }
        (void)usleep(sleepTime);
    }
}
