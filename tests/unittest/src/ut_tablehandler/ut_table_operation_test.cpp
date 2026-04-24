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
 * Description: CloudNativeDatabase UTTableOperationTest(ut table Operation test class)
 */

#include "ut_tablehandler/ut_table_operation_test.h"
#include "ut_mock/ut_instance_mock.h"
#include "ut_tablehandler/ut_table.h"
#include "ut_tablehandler/ut_table_operation.h"
#include "table_handler.h"
#include "table_data_generator.h"
#include "transaction/dstore_transaction_interface.h"
#include <thread>

static void FillIndexValues(const TableInfo &tableInfo, int rowIdx, int rowNum, Datum *indexValues)
{
    for (uint8 colIdx = 0; colIdx < tableInfo.indexDesc->indexAttrNum; ++colIdx) {
        indexValues[colIdx] = Int32GetDatum((rowIdx * rowNum) + tableInfo.indexDesc->indexCol[colIdx]);
    }
}

static int LockTupleForTest(const TableInfo &tableInfo, const char *indexName, Datum *indexValues,
                            HeapTuple **tuple)
{
    DstoreTableHandler *tableHandler = simulator->GetTableHandler(tableInfo.relation.name, indexName);
    StorageAssert(tableHandler != nullptr);
    TransactionInterface::StartTrxCommand();
    TransactionInterface::SetSnapShot();
    int ret = tableHandler->LockTuple(tableInfo.indexDesc->indexCol, indexValues, tuple,
                                      tableInfo.indexDesc->indexAttrNum);
    if (ret == 0) {
        TransactionInterface::CommitTrxCommand();
    } else {
        TransactionInterface::AbortTrx();
    }
    delete tableHandler;
    return ret;
}

static int DeleteForTest(const TableInfo &tableInfo, const char *indexName, Datum *indexValues)
{
    DstoreTableHandler *tableHandler = simulator->GetTableHandler(tableInfo.relation.name, indexName);
    StorageAssert(tableHandler != nullptr);
    TransactionInterface::StartTrxCommand();
    TransactionInterface::SetSnapShot();
    int ret = tableHandler->Delete(tableInfo.indexDesc->indexCol, indexValues, tableInfo.indexDesc->indexAttrNum);
    if (ret == 0) {
        TransactionInterface::CommitTrxCommand();
    } else {
        TransactionInterface::AbortTrx();
    }
    delete tableHandler;
    return ret;
}

void UTTableOperationTest::SetUp()
{
    DSTORETEST::SetUp();

    MockStorageInstance *instance = DstoreNew(m_ut_memory_context) MockStorageInstance();
    instance->Install(&DSTORETEST::m_guc, m_ut_memory_context);
    instance->Startup(&DSTORETEST::m_guc);

    m_utTableOperate = DstoreNew(m_ut_memory_context) UTTableOperation(instance);
    m_utTableOperate->Initialize();
}

void UTTableOperationTest::TearDown()
{
    StopInstance();
    DSTORETEST::TearDown();
}

void UTTableOperationTest::RestartInstance()
{
    StorageAssert(m_utTableOperate == nullptr);
    MockStorageInstance *instance = DstoreNew(m_ut_memory_context) MockStorageInstance();
    instance->Startup(&DSTORETEST::m_guc);
    m_utTableOperate = DstoreNew(m_ut_memory_context) UTTableOperation(instance);
    m_utTableOperate->Initialize();
}

void UTTableOperationTest::StopInstance()
{
    if(m_utTableOperate != nullptr) {
        m_utTableOperate->Destroy();
        delete m_utTableOperate;
        m_utTableOperate = nullptr;
    }
    (void)g_storageInstance->GetBufferMgr()->FlushAll(true);
    MockStorageInstance *instance = (MockStorageInstance *)g_storageInstance;
    instance->Shutdown();
    delete instance;
}

void UTTableOperationTest::CheckAllTable()
{
    for (TableNameType i = TABLE_1; i < TABLE_MAX_CNT; i = TableNameType(i + 1)) {
        CheckTable(i);
    }
}

void UTTableOperationTest::CheckTable(uint8_t tableNameType)
{
     if (tableNameType < TABLE_1 || tableNameType >= TABLE_MAX_CNT) {
        StorageAssert(0);
    }

    TableInfo tableInfo = TABLE_CACHE[tableNameType];
    StorageRelation relation = m_utTableOperate->GetTable(static_cast<TableNameType>(tableNameType));
    ASSERT_TRUE(relation != nullptr);
    EXPECT_NE(relation->relOid, DSTORE_INVALID_OID);
    ASSERT_TRUE(relation->index == nullptr);
    ASSERT_TRUE(relation->indexInfo == nullptr);
    ASSERT_TRUE(relation->btreeSmgr == nullptr);
    ASSERT_TRUE(relation->tableSmgr != nullptr);

    ASSERT_TRUE(relation->rel != nullptr);
    EXPECT_EQ(relation->rel->parttype, tableInfo.relation.partType);
    EXPECT_EQ(relation->rel->relkind, tableInfo.relation.relKind);
    EXPECT_EQ(relation->rel->relpersistence, tableInfo.relation.persistenceLevel);

    ASSERT_TRUE(relation->attr != nullptr);
    EXPECT_EQ(relation->attr->natts, tableInfo.relation.attrNum);
    EXPECT_EQ(relation->attr->initdefvals, nullptr);
    for (int j = 0; j < relation->attr->natts; ++j) {
        ASSERT_TRUE(relation->attr->attrs[j] != nullptr);
        EXPECT_EQ(relation->attr->attrs[j]->attnum, j);
        EXPECT_EQ(relation->attr->attrs[j]->atttypid, tableInfo.colDesc[j].type);
        EXPECT_EQ(relation->attr->attrs[j]->attlen, tableInfo.colDesc[j].len);
        EXPECT_EQ(relation->attr->attrs[j]->attnotnull, !tableInfo.colDesc[j].canBeNull);
        EXPECT_EQ(relation->attr->attrs[j]->attbyval, tableInfo.colDesc[j].isByVal);
        EXPECT_EQ(relation->attr->attrs[j]->atthasdef, tableInfo.colDesc[j].isHaveDefVal);
        EXPECT_EQ(relation->attr->attrs[j]->attalign, tableInfo.colDesc[j].align);
        EXPECT_EQ(relation->attr->attrs[j]->attstorage, tableInfo.colDesc[j].storageType);
    }
    m_utTableOperate->DestroyRelation(relation);
}

void UTTableOperationTest::CheckIndex(uint8_t tableNameType)
{
     if (tableNameType < TABLE_1 || tableNameType >= TABLE_MAX_CNT) {
        StorageAssert(0);
    }

    TableInfo tableInfo = TABLE_CACHE[tableNameType];
    StorageRelation relation = m_utTableOperate->GetIndex(static_cast<TableNameType>(tableNameType));
    ASSERT_TRUE(relation != nullptr);
    EXPECT_NE(relation->relOid, DSTORE_INVALID_OID);
    
    ASSERT_TRUE(relation->index != nullptr);
    ASSERT_TRUE(relation->indexInfo != nullptr);
    ASSERT_TRUE(relation->btreeSmgr != nullptr);
    EXPECT_EQ(relation->tableSmgr, nullptr);

    ASSERT_TRUE(relation->rel != nullptr);
    EXPECT_EQ(relation->rel->parttype, tableInfo.relation.partType);
    EXPECT_EQ(relation->rel->relkind, SYS_RELKIND_INDEX);
    EXPECT_EQ(relation->rel->relpersistence, tableInfo.relation.persistenceLevel);

    ASSERT_TRUE(relation->attr != nullptr);
    EXPECT_EQ(relation->attr->natts, tableInfo.indexDesc->indexAttrNum);
    EXPECT_EQ(relation->attr->initdefvals, nullptr);
    for (int j = 0; j < tableInfo.indexDesc->indexAttrNum; ++j) {
        ASSERT_TRUE(relation->attr->attrs[j] != nullptr);
        EXPECT_EQ(relation->attr->attrs[j]->attnum, j + 1);
        EXPECT_EQ(relation->attr->attrs[j]->atttypid, tableInfo.colDesc[tableInfo.indexDesc->indexCol[j]].type);
        EXPECT_EQ(relation->attr->attrs[j]->attlen, tableInfo.colDesc[tableInfo.indexDesc->indexCol[j]].len);
        EXPECT_EQ(relation->attr->attrs[j]->attnotnull, !tableInfo.colDesc[tableInfo.indexDesc->indexCol[j]].canBeNull);
        EXPECT_EQ(relation->attr->attrs[j]->attbyval, tableInfo.colDesc[tableInfo.indexDesc->indexCol[j]].isByVal);
        EXPECT_EQ(relation->attr->attrs[j]->atthasdef, tableInfo.colDesc[tableInfo.indexDesc->indexCol[j]].isHaveDefVal);
        EXPECT_EQ(relation->attr->attrs[j]->attalign, tableInfo.colDesc[tableInfo.indexDesc->indexCol[j]].align);
        EXPECT_EQ(relation->attr->attrs[j]->attstorage, tableInfo.colDesc[tableInfo.indexDesc->indexCol[j]].storageType);
    }
    EXPECT_EQ(relation->indexInfo->indnatts, tableInfo.indexDesc->indexAttrNum);
    EXPECT_NE(relation->indexInfo->indrelid, DSTORE_INVALID_OID);
    EXPECT_EQ(relation->indexInfo->indexrelid, relation->relOid);
    EXPECT_EQ(relation->indexInfo->indisunique, tableInfo.indexDesc->isUnique);
    m_utTableOperate->DestroyRelation(relation);
}

TEST_F(UTTableOperationTest, CreateTableTest)
{
    for (TableNameType i = TABLE_1; i < TABLE_MAX_CNT; i = TableNameType(i + 1)) {
        int ret = m_utTableOperate->CreateTable(i);
        EXPECT_EQ(ret, 0);
    }
}

TEST_F(UTTableOperationTest, GetTableTest)
{
    int ret = m_utTableOperate->CreateAllTable();
    EXPECT_EQ(ret, 0);
    CheckAllTable();
}

TEST_F(UTTableOperationTest, CheckTableTest)
{
    int ret = m_utTableOperate->CreateAllTable();
    EXPECT_EQ(ret, 0);
    StopInstance();
    RestartInstance();
    ret = m_utTableOperate->RecoveryAllTable();
    EXPECT_EQ(ret, 0);

    for (TableNameType i = TABLE_1; i < TABLE_MAX_CNT; i = TableNameType(i + 1)) {
        int ret = m_utTableOperate->CheckTable(i);
        EXPECT_EQ(ret, 0);
        CheckTable(static_cast<uint8_t>(i));
    }
}

TEST_F(UTTableOperationTest, InsertTest)
{
    const int testTupleRowCount = 10;
    int ret = m_utTableOperate->CreateAllTable();
    EXPECT_EQ(ret, 0);
    for (TableNameType i = TABLE_1; i < TABLE_MAX_CNT; i = TableNameType(i + 1)) {
        ret = m_utTableOperate->Insert(i, testTupleRowCount);
        EXPECT_EQ(ret, 0);
    }
}

TEST_F(UTTableOperationTest, CreateIndexTest)
{
    int ret = m_utTableOperate->CreateAllTable();
    EXPECT_EQ(ret, 0);
    for (TableNameType i = TABLE_1; i < TABLE_MAX_CNT; i = TableNameType(i + 1)) {
        ret = m_utTableOperate->CreateIndex(i);
        EXPECT_EQ(ret, 0);
    }
}

TEST_F(UTTableOperationTest, CheckIndexTest)
{
    int ret = m_utTableOperate->CreateAllTable();
    EXPECT_EQ(ret, 0);
    ret = m_utTableOperate->CreateAllIndex();
    EXPECT_EQ(ret, 0);
    StopInstance();
    RestartInstance();
    ret = m_utTableOperate->RecoveryAllTable();
    EXPECT_EQ(ret, 0);

    ret = m_utTableOperate->RecoveryAllIndex();
    EXPECT_EQ(ret, 0);

    for (TableNameType i = TABLE_1; i < TABLE_MAX_CNT; i = TableNameType(i + 1)) {
        int ret = m_utTableOperate->CheckIndex(i);
        EXPECT_EQ(ret, 0);
        CheckIndex(static_cast<uint8_t>(i));
    }
}

TEST_F(UTTableOperationTest, ScanWithInsertWithIndexTest)
{
    int ret = m_utTableOperate->CreateAllTable();
    EXPECT_EQ(ret, 0);
    ret = m_utTableOperate->CreateAllIndex();
    EXPECT_EQ(ret, 0);

    const int testTupleRowCount = 10;
    for (TableNameType i = TABLE_1; i < TABLE_MAX_CNT; i = TableNameType(i + 1)) {
        ret = m_utTableOperate->InsertAndScan(i, testTupleRowCount);
        EXPECT_EQ(ret, 0);
    }
}

TEST_F(UTTableOperationTest, ScanWithInsertWithoutIndexTest)
{
    int ret = m_utTableOperate->CreateAllTable();
    EXPECT_EQ(ret, 0);
    const int testTupleRowCount = 10;
    for (TableNameType i = TABLE_1; i < TABLE_MAX_CNT; i = TableNameType(i + 1)) {
        ret = m_utTableOperate->Insert(i, testTupleRowCount);
        EXPECT_EQ(ret, 0);
    }
    ret = m_utTableOperate->CreateAllIndex();
    EXPECT_EQ(ret, 0);
    for (TableNameType i = TABLE_1; i < TABLE_MAX_CNT; i = TableNameType(i + 1)) {
        ret = m_utTableOperate->Scan(i, testTupleRowCount);
        EXPECT_EQ(ret, 0);
    }
}

TEST_F(UTTableOperationTest, DeleteTest)
{
    int ret = m_utTableOperate->CreateAllTable();
    EXPECT_EQ(ret, 0);

    const int testTupleRowCount = 10;
    for (TableNameType i = TABLE_1; i < TABLE_MAX_CNT; i = TableNameType(i + 1)) {
        ret = m_utTableOperate->Insert(i, testTupleRowCount);
        EXPECT_EQ(ret, 0);
    }

    ret = m_utTableOperate->CreateAllIndex();
    EXPECT_EQ(ret, 0);

    for (TableNameType i = TABLE_1; i < TABLE_MAX_CNT; i = TableNameType(i + 1)) {
        ret = m_utTableOperate->Delete(i, testTupleRowCount);
        EXPECT_EQ(ret, 0);
    }
}

TEST_F(UTTableOperationTest, UpdateTest)
{
    int ret = m_utTableOperate->CreateAllTable();
    EXPECT_EQ(ret, 0);
    const int testTupleRowCount = 10;
    for (TableNameType i = TABLE_1; i < TABLE_MAX_CNT; i = TableNameType(i + 1)) {
        ret = m_utTableOperate->Insert(i, testTupleRowCount);
        EXPECT_EQ(ret, 0);
    }

    ret = m_utTableOperate->CreateAllIndex();
    EXPECT_EQ(ret, 0);

    for (TableNameType i = TABLE_1; i < TABLE_MAX_CNT; i = TableNameType(i + 1)) {
        ret = m_utTableOperate->UpdateAndCheck(i, testTupleRowCount);
        EXPECT_EQ(ret, 0);
    }
}

TEST_F(UTTableOperationTest, LockTuplePointGetUniqueFastPathTest)
{
    const int testTupleRowCount = 5;
    int ret = m_utTableOperate->CreateAllTable();
    EXPECT_EQ(ret, 0);
    ret = m_utTableOperate->Insert(TABLE_1, testTupleRowCount);
    EXPECT_EQ(ret, 0);
    ret = m_utTableOperate->CreateAllIndex();
    EXPECT_EQ(ret, 0);

    g_storageInstance->GetGuc()->SetEnablePointGetFastPath(true);
    TableInfo tableInfo = TABLE_CACHE[TABLE_1];
    char *indexName =
        TableDataGenerator::GenerateIndexName(tableInfo.relation.name, tableInfo.indexDesc->indexCol,
                                              tableInfo.indexDesc->indexAttrNum);
    Datum indexValues[tableInfo.indexDesc->indexAttrNum];
    FillIndexValues(tableInfo, 0, testTupleRowCount, indexValues);

    HeapTuple *tuple = nullptr;
    ret = LockTupleForTest(tableInfo, indexName, indexValues, &tuple);
    EXPECT_EQ(ret, 0);
    ASSERT_TRUE(tuple != nullptr);
    DstorePfreeExt(tuple);
    DstorePfreeExt(indexName);
}

TEST_F(UTTableOperationTest, LockTupleMultiColumnUniqueFallbackTest)
{
    const int testTupleRowCount = 5;
    int ret = m_utTableOperate->CreateAllTable();
    EXPECT_EQ(ret, 0);
    ret = m_utTableOperate->Insert(TABLE_3, testTupleRowCount);
    EXPECT_EQ(ret, 0);
    ret = m_utTableOperate->CreateAllIndex();
    EXPECT_EQ(ret, 0);

    g_storageInstance->GetGuc()->SetEnablePointGetFastPath(true);
    TableInfo tableInfo = TABLE_CACHE[TABLE_3];
    char *indexName =
        TableDataGenerator::GenerateIndexName(tableInfo.relation.name, tableInfo.indexDesc->indexCol,
                                              tableInfo.indexDesc->indexAttrNum);
    Datum indexValues[tableInfo.indexDesc->indexAttrNum];
    FillIndexValues(tableInfo, 0, testTupleRowCount, indexValues);

    HeapTuple *tuple = nullptr;
    ret = LockTupleForTest(tableInfo, indexName, indexValues, &tuple);
    EXPECT_EQ(ret, 0);
    ASSERT_TRUE(tuple != nullptr);
    DstorePfreeExt(tuple);
    DstorePfreeExt(indexName);
}

TEST_F(UTTableOperationTest, DeletePointGetUniqueFastPathAndMissTest)
{
    const int testTupleRowCount = 5;
    int ret = m_utTableOperate->CreateAllTable();
    EXPECT_EQ(ret, 0);
    ret = m_utTableOperate->Insert(TABLE_1, testTupleRowCount);
    EXPECT_EQ(ret, 0);
    ret = m_utTableOperate->CreateAllIndex();
    EXPECT_EQ(ret, 0);

    g_storageInstance->GetGuc()->SetEnablePointGetFastPath(true);
    TableInfo tableInfo = TABLE_CACHE[TABLE_1];
    char *indexName =
        TableDataGenerator::GenerateIndexName(tableInfo.relation.name, tableInfo.indexDesc->indexCol,
                                              tableInfo.indexDesc->indexAttrNum);
    Datum indexValues[tableInfo.indexDesc->indexAttrNum];
    FillIndexValues(tableInfo, 0, testTupleRowCount, indexValues);

    ret = DeleteForTest(tableInfo, indexName, indexValues);
    EXPECT_EQ(ret, 0);
    ret = DeleteForTest(tableInfo, indexName, indexValues);
    EXPECT_EQ(ret, -1);
    DstorePfreeExt(indexName);
}

TEST_F(UTTableOperationTest, DeleteMultiColumnUniqueFallbackTest)
{
    const int testTupleRowCount = 5;
    int ret = m_utTableOperate->CreateAllTable();
    EXPECT_EQ(ret, 0);
    ret = m_utTableOperate->Insert(TABLE_3, testTupleRowCount);
    EXPECT_EQ(ret, 0);
    ret = m_utTableOperate->CreateAllIndex();
    EXPECT_EQ(ret, 0);

    g_storageInstance->GetGuc()->SetEnablePointGetFastPath(true);
    TableInfo tableInfo = TABLE_CACHE[TABLE_3];
    char *indexName =
        TableDataGenerator::GenerateIndexName(tableInfo.relation.name, tableInfo.indexDesc->indexCol,
                                              tableInfo.indexDesc->indexAttrNum);
    Datum indexValues[tableInfo.indexDesc->indexAttrNum];
    FillIndexValues(tableInfo, 0, testTupleRowCount, indexValues);

    ret = DeleteForTest(tableInfo, indexName, indexValues);
    EXPECT_EQ(ret, 0);
    DstorePfreeExt(indexName);
}

void *InsertDatas(TableNameType type, int rowNum, UTTableOperation *tableOpration)
{
    create_thread_and_register();
    ut_init_transaction_runtime();
    int ret = tableOpration->Insert(type, rowNum);
    EXPECT_EQ(ret, 0);
    unregister_thread();
    return nullptr;
}

void *Scan(TableNameType type, int rowNum, UTTableOperation *tableOpration)
{
    create_thread_and_register();
    ut_init_transaction_runtime();
    tableOpration->Scan(type, rowNum);
    unregister_thread();
    return nullptr;
}
void *Update(TableNameType type, int rowNum, UTTableOperation *tableOpration)
{
    create_thread_and_register();
    ut_init_transaction_runtime();
    tableOpration->Update(type, rowNum);
    unregister_thread();
    return nullptr;
}

void *Delete(TableNameType type, int rowNum, UTTableOperation *tableOpration)
{
    create_thread_and_register();
    ut_init_transaction_runtime();
    tableOpration->Delete(type, rowNum);
    unregister_thread();
    return nullptr;
}

TEST_F(UTTableOperationTest, ConcurrentTest)
{
    const int testTupleRowCount = 10;
    const int threadNum = TABLE_MAX_CNT;
    std::thread threads[threadNum];
    std::thread transcationThread[threadNum * 3];

    int ret = m_utTableOperate->CreateAllTable();
    EXPECT_EQ(ret, 0);
    for (int i = 0; i < threadNum; ++i) {
        threads[i] = std::thread(InsertDatas, static_cast<TableNameType>(i), testTupleRowCount, m_utTableOperate);
    }
    for (int i = 0; i < threadNum; i++) {
        threads[i].join();
    }
    ret = m_utTableOperate->CreateAllIndex();
    EXPECT_EQ(ret, 0);
    for (int i = 0; i < threadNum; ++i) {
        transcationThread[i] = std::thread(Scan, static_cast<TableNameType>(i), testTupleRowCount, m_utTableOperate);
    }

    for (int i = 0; i < threadNum; ++i) {
        transcationThread[threadNum + i] =
            std::thread(Update, static_cast<TableNameType>(i), testTupleRowCount, m_utTableOperate);
    }

    for (int i = 0; i < threadNum; ++i) {
        transcationThread[threadNum * 2 + i] =
            std::thread(Delete, static_cast<TableNameType>(i), testTupleRowCount, m_utTableOperate);
    }
    for (int i = 0; i < threadNum * 3; i++) {
        transcationThread[i].join();
    }
}
