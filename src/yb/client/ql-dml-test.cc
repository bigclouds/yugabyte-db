// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include <thread>

#include "yb/client/ql-dml-test-base.h"
#include "yb/client/table_handle.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"

#include "yb/util/backoff_waiter.h"
#include "yb/util/curl_util.h"
#include "yb/util/jsonreader.h"
#include "yb/util/random.h"
#include "yb/util/random_util.h"
#include "yb/util/tostring.h"
#include "yb/util/tsan_util.h"

#include "yb/yql/cql/ql/util/statement_result.h"

DECLARE_bool(mini_cluster_reuse_data);
DECLARE_bool(rocksdb_disable_compactions);
DECLARE_int32(yb_num_shards_per_tserver);
DECLARE_int64(db_block_cache_size_bytes);

namespace yb {
namespace client {

using yb::ql::RowsResult;

// Verify all column values of a row. We use a macro instead of a function so that EXPECT_EQ can
// show the caller's line number should the test fails.
#define EXPECT_ROW_VALUES(row, h1, h2, r1, r2, c1, c2) \
  do {                                                 \
    EXPECT_EQ(row.column(0).int32_value(), h1);        \
    EXPECT_EQ(row.column(1).string_value(), h2);       \
    EXPECT_EQ(row.column(2).int32_value(), r1);        \
    EXPECT_EQ(row.column(3).string_value(), r2);       \
    EXPECT_EQ(row.column(4).int32_value(), c1);        \
    EXPECT_EQ(row.column(5).string_value(), c2);       \
  } while (false)

namespace {

const std::vector<std::string> kAllColumns = {"h1", "h2", "r1", "r2", "c1", "c2"};

}

class QLDmlTest : public QLDmlTestBase {
 public:
  QLDmlTest() {
  }

  void SetUp() override {
    QLDmlTestBase::SetUp();

    if (!FLAGS_mini_cluster_reuse_data) {
      YBSchemaBuilder b;
      b.AddColumn("h1")->Type(INT32)->HashPrimaryKey()->NotNull();
      b.AddColumn("h2")->Type(STRING)->HashPrimaryKey()->NotNull();
      b.AddColumn("r1")->Type(INT32)->PrimaryKey()->NotNull();
      b.AddColumn("r2")->Type(STRING)->PrimaryKey()->NotNull();
      b.AddColumn("c1")->Type(INT32);
      b.AddColumn("c2")->Type(STRING);

      ASSERT_OK(table_.Create(kTableName, CalcNumTablets(3), client_.get(), &b));
    } else {
      ASSERT_OK(table_.Open(kTableName, client_.get()));
    }
  }

  // Insert a full, single row, equivalent to the insert statement below. Return a YB write op that
  // has been applied.
  //   insert into t values (h1, h2, r1, r2, c1, c2);
  shared_ptr<YBqlWriteOp> InsertRow(
      const shared_ptr<YBSession>& session,
      const int32 h1, const string& h2,
      const int32 r1, const string& r2,
      const int32 c1, const string& c2) {

    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, h1);
    QLAddStringHashValue(req, h2);
    QLAddInt32RangeValue(req, r1);
    QLAddStringRangeValue(req, r2);
    table_.AddInt32ColumnValue(req, "c1", c1);
    table_.AddStringColumnValue(req, "c2", c2);
    CHECK_OK(session->Apply(op));
    return op;
  }

  // Select the specified columns of a row using a primary key, equivalent to the select statement
  // below. Return a YB read op that has been applied.
  //   select <columns...> from t where h1 = <h1> and h2 = <h2> and r1 = <r1> and r2 = <r2>;
  shared_ptr<YBqlReadOp> SelectRow(
      const shared_ptr<YBSession>& session,
      const vector<string>& columns,
      const int32 h1, const string& h2,
      const int32 r1, const string& r2) {

    const shared_ptr<YBqlReadOp> op = table_.NewReadOp();
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, h1);
    QLAddStringHashValue(req, h2);
    auto* const condition = req->mutable_where_expr()->mutable_condition();
    condition->set_op(QL_OP_AND);
    table_.AddInt32Condition(condition, "r1", QL_OP_EQUAL, r1);
    table_.AddStringCondition(condition, "r2", QL_OP_EQUAL, r2);
    table_.AddColumns(columns, req);
    CHECK_OK(session->Apply(op));
    return op;
  }

  std::shared_ptr<YBqlReadOp> SelectRow() {
    return SelectRow(NewSession(), kAllColumns, 1, "a", 2, "b");
  }

  __attribute__ ((warn_unused_result)) testing::AssertionResult VerifyRow(
      const shared_ptr<YBSession>& session,
      int32 h1, const std::string& h2,
      int32 r1, const std::string& r2,
      int32 c1, const std::string& c2) {
    auto op = SelectRow(session, {"c1", "c2"}, h1, h2, r1, r2);

    VERIFY_EQ(QLResponsePB::YQL_STATUS_OK, op->response().status());
    auto rowblock = RowsResult(op.get()).GetRowBlock();
    VERIFY_EQ(1, rowblock->row_count());
    const auto& row = rowblock->row(0);
    VERIFY_EQ(c1, row.column(0).int32_value());
    VERIFY_EQ(c2, row.column(1).string_value());

    return testing::AssertionSuccess();
  }

  void AddAllColumns(QLReadRequestPB* req) {
    table_.AddColumns(kAllColumns, req);
  }

  TableHandle table_;
};

TEST_F(QLDmlTest, TestInsertUpdateAndSelect) {
  {
    // Test inserting a row.
    // insert into t values (1, 'a', 2, 'b', 3, 'c');
    const shared_ptr<YBSession> session(NewSession());
    const shared_ptr<YBqlWriteOp> op = InsertRow(session, 1, "a", 2, "b", 3, "c");
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    // Test selecting a row.
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow();

    // Expect 1, 'a', 2, 'b', 3, 'c' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "b", 3, "c");
  }

  {
    // Test updating the row.
    // update t set c1 = 4, c2 = 'd' where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_UPDATE);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    QLAddInt32RangeValue(req, 2);
    QLAddStringRangeValue(req, "b");
    table_.AddInt32ColumnValue(req, "c1", 4);
    table_.AddStringColumnValue(req, "c2", "d");
    const shared_ptr<YBSession> session(NewSession());
    CHECK_OK(session->Apply(op));

    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    // Test selecting the row back, but flush manually and using async API (inside FlushSession).
    // select c1, c2 from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBSession> session = NewSession();
    CHECK_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));
    const shared_ptr<YBqlReadOp> op = SelectRow(session, {"c1", "c2"}, 1, "a", 2, "b");
    EXPECT_OK(session->Flush());

    // Expect 4, 'd' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    const auto& row = rowblock->row(0);
    EXPECT_EQ(row.column(0).int32_value(), 4);
    EXPECT_EQ(row.column(1).string_value(), "d");
  }

  auto used_tracker = MemTracker::GetRootTracker()->FindChild("server")->FindChild("Read Buffer")->
                      FindChild("Inbound RPC")->FindChild("Used");

  ASSERT_EQ(0, used_tracker->peak_consumption());
}

TEST_F(QLDmlTest, TestInsertWrongSchema) {
  const shared_ptr<YBSession> session(NewSession());
  CHECK_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));

  // Move to schema version 1 by altering table
  gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  table_alterer->AddColumn("c3")->Type(INT32)->NotNull();
  EXPECT_OK(table_alterer->timeout(MonoDelta::FromSeconds(60))->Alter());

  // The request created has schema version 0 by default
  const shared_ptr<YBqlWriteOp> op = InsertRow(session, 1, "a", 2, "b", 3, "c");

  EXPECT_OK(session->Flush());
  EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_SCHEMA_VERSION_MISMATCH);
}

namespace {

std::string RandomValueAt(int32_t idx, size_t len = 2000) {
  Random rng(idx);
  return RandomHumanReadableString(len, &rng);
}

std::string StrRangeFor(int32_t idx) {
  return "range_" + std::to_string(1000000 + idx);
}

class QLDmlRangeFilterBase: public QLDmlTest {
 public:
  void SetUp() override {
    FLAGS_rocksdb_disable_compactions = true;
    FLAGS_yb_num_shards_per_tserver = 1;
    QLDmlTest::SetUp();
  }
};

size_t CountIterators(MiniCluster* cluster) {
  size_t result = 0;

  for (int i = 0; i != cluster->num_tablet_servers(); ++i) {
    std::vector<std::shared_ptr<tablet::TabletPeer>> peers;
    cluster->mini_tablet_server(i)->server()->tablet_manager()->GetTabletPeers(&peers);
    for (const auto& peer : peers) {
      auto statistics = peer->tablet()->rocksdb_statistics();
      auto value = statistics->getTickerCount(rocksdb::NO_TABLE_CACHE_ITERATORS);
      result += value;
    }
  }

  return result;
}

constexpr int32_t kHashInt = 42;
const std::string kHashStr = "all_records_have_same_id";

} // namespace

TEST_F_EX(QLDmlTest, RangeFilter, QLDmlRangeFilterBase) {
  constexpr size_t kTotalLines = NonTsanVsTsan(25000ULL, 5000ULL);
  auto session = NewSession();
  if (!FLAGS_mini_cluster_reuse_data) {
    ASSERT_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));
    for(int32_t i = 0; i != kTotalLines;) {
      const shared_ptr<YBqlWriteOp> op = InsertRow(session,
                                                   kHashInt,
                                                   kHashStr,
                                                   i,
                                                   StrRangeFor(i),
                                                   -i,
                                                   RandomValueAt(i));
      ASSERT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
      ++i;
      if (i % 100 == 0) {
        ASSERT_OK(session->Flush());
      }
      if (i % 5000 == 0) {
        LOG(WARNING) << "Inserted " << i << " rows";
      }
    }
    ASSERT_OK(session->Flush());
    session = NewSession();
    LOG(WARNING) << "Finished creating DB";
    constexpr int32_t kProbeStep = 997;
    for(int32_t idx = 0; idx < kTotalLines; idx += kProbeStep) {
      ASSERT_VERIFY(VerifyRow(session,
                              kHashInt,
                              kHashStr,
                              idx,
                              StrRangeFor(idx),
                              -idx,
                              RandomValueAt(idx)));
    }
    LOG(WARNING) << "Preliminary check done";
    ASSERT_OK(cluster_->FlushTablets());
    std::this_thread::sleep_for(1s);
  }
  FLAGS_db_block_cache_size_bytes = -2;
  ASSERT_OK(cluster_->RestartSync());
  {
    constexpr size_t kTotalProbes = 1000;
    Random rng(GetRandomSeed32());
    size_t old_iterators = CountIterators(cluster_.get());
    for (size_t i = 0; i != kTotalProbes; ) {
      int32_t idx = rng.Uniform(kTotalLines);
      ASSERT_VERIFY(VerifyRow(session,
                              kHashInt,
                              kHashStr,
                              idx,
                              StrRangeFor(idx),
                              -idx,
                              RandomValueAt(idx)));

      size_t new_iterators = CountIterators(cluster_.get());
      ASSERT_EQ(old_iterators + 1, new_iterators);
      old_iterators = new_iterators;
      ++i;
      if (i % 100 == 0) {
        LOG(WARNING) << "Checked " << i << " rows";
      }
    }
  }
}

TEST_F(QLDmlTest, TestInsertMultipleRows) {
  {
    const shared_ptr<YBSession> session(NewSession());
    CHECK_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));

    // Test inserting 2 rows.
    // insert into t values (1, 'a', 2, 'b', 3, 'c');
    // insert into t values (1, 'a', 2, 'd', 4, 'e');
    const shared_ptr<YBqlWriteOp> op1 = InsertRow(session, 1, "a", 2, "b", 3, "c");
    const shared_ptr<YBqlWriteOp> op2 = InsertRow(session, 1, "a", 2, "d", 4, "e");

    CHECK_OK(session->Flush());
    EXPECT_EQ(op1->response().status(), QLResponsePB::YQL_STATUS_OK);
    EXPECT_EQ(op2->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    // Test selecting the first row back.
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = table_.NewReadOp();
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    auto* const condition = req->mutable_where_expr()->mutable_condition();
    condition->set_op(QL_OP_AND);
    table_.AddInt32Condition(condition, "r1", QL_OP_EQUAL, 2);
    table_.AddStringCondition(condition, "r2", QL_OP_EQUAL, "b");
    AddAllColumns(req);

    const shared_ptr<YBSession> session(NewSession());
    CHECK_OK(session->Apply(op));

    // Expect 1, 'a', 2, 'b', 3, 'c' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    {
      unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
      EXPECT_EQ(rowblock->row_count(), 1);
      EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "b", 3, "c");
    }

    // Test reusing the read op and updating where clause to select the other row.
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'd';
    req->mutable_where_expr()->mutable_condition()->mutable_operands()->RemoveLast();
    table_.AddStringCondition(
       req->mutable_where_expr()->mutable_condition(), "r2", QL_OP_EQUAL, "d");
    CHECK_OK(session->Apply(op));

    // Expect 1, 'a', 2, 'd', 4, 'e' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    {
      unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
      EXPECT_EQ(rowblock->row_count(), 1);
      EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "d", 4, "e");
    }
  }
}

TEST_F(QLDmlTest, TestSelectMultipleRows) {
  const auto session = NewSession();
  CHECK_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));
  {

    // insert into t values (1, 'a', 2, 'b', 3, 'c');
    // insert into t values (1, 'a', 2, 'd', 4, 'e');
    const shared_ptr<YBqlWriteOp> op1 = InsertRow(session, 1, "a", 2, "b", 3, "c");
    const shared_ptr<YBqlWriteOp> op2 = InsertRow(session, 1, "a", 2, "d", 4, "e");

    CHECK_OK(session->Flush());
    EXPECT_EQ(op1->response().status(), QLResponsePB::YQL_STATUS_OK);
    EXPECT_EQ(op2->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    // Test selecting 2 rows with an OR condition.
    // select * from t where h1 = 1 and h2 = 'a' and r2 = 'b' or r2 = 'd';
    const shared_ptr<YBqlReadOp> op = table_.NewReadOp();
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    auto* const condition = req->mutable_where_expr()->mutable_condition();
    condition->set_op(QL_OP_OR);
    table_.AddStringCondition(condition, "r2", QL_OP_EQUAL, "b");
    table_.AddStringCondition(condition, "r2", QL_OP_EQUAL, "d");
    AddAllColumns(req);

    CHECK_OK(session->SetFlushMode(YBSession::AUTO_FLUSH_SYNC));
    CHECK_OK(session->Apply(op));

    // Expect 1, 'a', 2, 'b', 3, 'c' and 1, 'a', 2, 'd', 4, 'e' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 2);
    EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "b", 3, "c");
    EXPECT_ROW_VALUES(rowblock->row(1), 1, "a", 2, "d", 4, "e");
  }

  {
    // Test selecting 2 rows with AND + OR column conditions.
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and (r2 = 'b' or r2 = 'd');
    const shared_ptr<YBqlReadOp> op = table_.NewReadOp();
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    auto* condition = req->mutable_where_expr()->mutable_condition();
    condition->set_op(QL_OP_AND);
    table_.AddInt32Condition(condition, "r1", QL_OP_EQUAL, 2);
    condition = condition->add_operands()->mutable_condition();
    condition->set_op(QL_OP_OR);
    table_.AddStringCondition(condition, "r2", QL_OP_EQUAL, "b");
    table_.AddStringCondition(condition, "r2", QL_OP_EQUAL, "d");
    AddAllColumns(req);

    CHECK_OK(session->Apply(op));

    // Expect 1, 'a', 2, 'b', 3, 'c' and 1, 'a', 2, 'd', 4, 'e' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 2);
    EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "b", 3, "c");
    EXPECT_ROW_VALUES(rowblock->row(1), 1, "a", 2, "d", 4, "e");
  }
}

TEST_F(QLDmlTest, TestSelectWithoutConditionWithLimit) {
  {
    // Insert 100 rows.
    // insert into t values (1, 'a', 2, 'b', 3, 'c');
    // insert into t values (1, 'a', 3, 'b', 4, 'c');
    // insert into t values (1, 'a', 4, 'b', 5, 'c');
    // ...
    // insert into t values (1, 'a', 101, 'b', 102, 'c');
    const shared_ptr<YBSession> session(NewSession());
    CHECK_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));
    vector<shared_ptr<YBqlWriteOp>> ops;
    for (int32_t i = 0; i < 100; i++) {
      ops.push_back(InsertRow(session, 1, "a", 2 + i, "b", 3 + i, "c"));
    }
    EXPECT_OK(session->Flush());
    for (const auto op : ops) {
      EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    }
  }

  {
    // Test selecting multiple rows with a row limit.
    // select * from t where h1 = 1 and h2 = 'a' limit 5;
    const shared_ptr<YBqlReadOp> op = table_.NewReadOp();
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    AddAllColumns(req);

    req->set_limit(5);
    const shared_ptr<YBSession> session(NewSession());
    CHECK_OK(session->Apply(op));

    // Expect 5 rows:
    //   1, 'a', 2, 'b', 3, 'c'
    //   1, 'a', 3, 'b', 4, 'c'
    //   1, 'a', 4, 'b', 5, 'c'
    //   1, 'a', 5, 'b', 6, 'c'
    //   1, 'a', 6, 'b', 7, 'c'
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 5);
    for (int32_t i = 0; i < 5; i++) {
      EXPECT_ROW_VALUES(rowblock->row(i), 1, "a", 2 + i, "b", 3 + i, "c");
    }
  }
}

TEST_F(QLDmlTest, TestUpsert) {
  const shared_ptr<YBSession> session(NewSession());
  {
    // Test upserting a row (update as insert).
    // update t set c1 = 3 where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlWriteOp> op(table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT));
    auto* const req = op->mutable_request();
    req->set_hash_code(0);
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    QLAddInt32RangeValue(req, 2);
    QLAddStringRangeValue(req, "b");
    table_.AddInt32ColumnValue(req, "c1", 3);
    CHECK_OK(session->Apply(op));

    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow();

    // Expect 1, 'a', 2, 'b', 3, null returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    const auto& row = rowblock->row(0);
    EXPECT_EQ(row.column(0).int32_value(), 1);
    EXPECT_EQ(row.column(1).string_value(), "a");
    EXPECT_EQ(row.column(2).int32_value(), 2);
    EXPECT_EQ(row.column(3).string_value(), "b");
    EXPECT_EQ(row.column(4).int32_value(), 3);
    EXPECT_TRUE(row.column(5).IsNull());
  }

  {
    // Test upsert to "insert" an additional column ("c2").
    // update t set c2 = 'c' where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    QLAddInt32RangeValue(req, 2);
    QLAddStringRangeValue(req, "b");
    table_.AddStringColumnValue(req, "c2", "c");
    CHECK_OK(session->Apply(op));

    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow();

    // Expect 1, 'a', 2, 'b', 3, 'c' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "b", 3, "c");
  }
}

TEST_F(QLDmlTest, TestDelete) {
  const shared_ptr<YBSession> session(NewSession());
  {
    // insert into t values (1, 'a', 2, 'b', 3, 'c');
    const shared_ptr<YBqlWriteOp> op = InsertRow(session, 1, "a", 2, "b", 3, "c");
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    // Test deleting a column ("c1").
    // delete c1 from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_DELETE);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    QLAddInt32RangeValue(req, 2);
    QLAddStringRangeValue(req, "b");
    table_.SetColumn(req->add_column_values(), "c1");
    CHECK_OK(session->Apply(op));

    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    // select c1, c2 from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow(session, {"c1", "c2"}, 1, "a", 2, "b");

    // Expect null, 'c' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    const auto& row = rowblock->row(0);
    EXPECT_TRUE(row.column(0).IsNull());
    EXPECT_EQ(row.column(1).string_value(), "c");
  }

  {
    // Test deleting the whole row.
    // delete from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_DELETE);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    QLAddInt32RangeValue(req, 2);
    QLAddStringRangeValue(req, "b");
    CHECK_OK(session->Apply(op));

    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    // select c1, c2 from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow(session, {"c1", "c2"}, 1, "a", 2, "b");

    // Expect no row returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 0);
  }
}

TEST_F(QLDmlTest, TestConditionalInsert) {
  const shared_ptr<YBSession> session(NewSession());
  {
    // insert into t values (1, 'a', 2, 'b', 3, 'c');
    const shared_ptr<YBqlWriteOp> op = InsertRow(session, 1, "a", 2, "b", 3, "c");
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow();

    // Expect 1, 'a', 2, 'b', 3, 'c' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "b", 3, "c");
  }

  {
    // Test IF NOT EXISTS when the row exists
    // insert into t values (1, 'a', 2, 'b', 4, 'd') if not exists;
    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    QLAddInt32RangeValue(req, 2);
    QLAddStringRangeValue(req, "b");
    table_.AddInt32ColumnValue(req, "c1", 4);
    table_.AddStringColumnValue(req, "c2", "d");
    auto* const condition = req->mutable_if_expr()->mutable_condition();
    condition->set_op(QL_OP_NOT_EXISTS);
    CHECK_OK(session->Apply(op));

    // Expect not applied
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    const auto& row = rowblock->row(0);
    EXPECT_EQ(rowblock->schema().column(0).name(), "[applied]");
    EXPECT_EQ(rowblock->schema().column(0).type_info()->type(), BOOL);
    EXPECT_FALSE(row.column(0).bool_value());
  }

  {
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow();

    // Expect 1, 'a', 2, 'b', 3, 'c' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "b", 3, "c");
  }

  {
    // Test IF NOT EXISTS AND a column condition when the row exists and column value is different.
    // insert into t values (1, 'a', 2, 'b', 4, 'd') if not exists or c2 = 'd';
    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    QLAddInt32RangeValue(req, 2);
    QLAddStringRangeValue(req, "b");
    table_.AddInt32ColumnValue(req, "c1", 4);
    table_.AddStringColumnValue(req, "c2", "d");
    auto* condition = req->mutable_if_expr()->mutable_condition();
    condition->set_op(QL_OP_OR);
    table_.AddCondition(condition, QL_OP_NOT_EXISTS);
    table_.AddStringCondition(condition, "c2", QL_OP_EQUAL, "d");
    req->mutable_column_refs()->add_ids(table_.ColumnId("c2"));
    CHECK_OK(session->Apply(op));

    // Expect not applied, return c2 = 'd'. Verify column names ("[applied]" and "c2") also.
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    const auto& row = rowblock->row(0);
    EXPECT_EQ(rowblock->schema().num_columns(), 6);
    EXPECT_EQ(rowblock->schema().column(0).name(), "[applied]");
    EXPECT_EQ(rowblock->schema().column(0).type_info()->type(), BOOL);
    EXPECT_EQ(rowblock->schema().column(1).name(), "h1");
    EXPECT_EQ(rowblock->schema().column(1).type_info()->type(), INT32);
    EXPECT_EQ(rowblock->schema().column(2).name(), "h2");
    EXPECT_EQ(rowblock->schema().column(2).type_info()->type(), STRING);
    EXPECT_EQ(rowblock->schema().column(3).name(), "r1");
    EXPECT_EQ(rowblock->schema().column(3).type_info()->type(), INT32);
    EXPECT_EQ(rowblock->schema().column(4).name(), "r2");
    EXPECT_EQ(rowblock->schema().column(4).type_info()->type(), STRING);
    EXPECT_EQ(rowblock->schema().column(5).name(), "c2");
    EXPECT_EQ(rowblock->schema().column(5).type_info()->type(), STRING);
    EXPECT_FALSE(row.column(0).bool_value());
    EXPECT_EQ(row.column(1).int32_value(), 1);
    EXPECT_EQ(row.column(2).string_value(), "a");
    EXPECT_EQ(row.column(3).int32_value(), 2);
    EXPECT_EQ(row.column(4).string_value(), "b");
    EXPECT_EQ(row.column(5).string_value(), "c");
  }

  {
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow();

    // Expect 1, 'a', 2, 'b', 3, 'c' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "b", 3, "c");
  }

  {
    // Test IF NOT EXISTS AND a column condition when the row exists and column value matches.
    // insert into t values (1, 'a', 2, 'b', 4, 'd') if not exists or c2 = 'c';
    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    QLAddInt32RangeValue(req, 2);
    QLAddStringRangeValue(req, "b");
    table_.AddInt32ColumnValue(req, "c1", 4);
    table_.AddStringColumnValue(req, "c2", "d");
    auto* condition = req->mutable_if_expr()->mutable_condition();
    condition->set_op(QL_OP_OR);
    table_.AddCondition(condition, QL_OP_NOT_EXISTS);
    table_.AddStringCondition(condition, "c2", QL_OP_EQUAL, "c");
    req->mutable_column_refs()->add_ids(table_.ColumnId("c2"));
    CHECK_OK(session->Apply(op));

    // Expect applied
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    const auto& row = rowblock->row(0);
    EXPECT_EQ(rowblock->schema().column(0).name(), "[applied]");
    EXPECT_EQ(rowblock->schema().column(0).type_info()->type(), BOOL);
    EXPECT_TRUE(row.column(0).bool_value());
  }

  {
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow();

    // Expect 1, 'a', 2, 'b', 3, 'c' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "b", 4, "d");
  }

  {
    // Sanity check: test regular insert to override the old row.
    // insert into t values (1, 'a', 2, 'b', 5, 'e');
    const shared_ptr<YBqlWriteOp> op = InsertRow(session, 1, "a", 2, "b", 5, "e");
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow();

    // Expect 1, 'a', 2, 'b', 5, 'e' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "b", 5, "e");
  }
}

TEST_F(QLDmlTest, TestConditionalUpdate) {
  const shared_ptr<YBSession> session(NewSession());
  {
    // insert into t values (1, 'a', 2, 'b', 3, 'c');
    const shared_ptr<YBqlWriteOp> op = InsertRow(session, 1, "a", 2, "b", 3, "c");
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow();

    // Expect 1, 'a', 2, 'b', 3, 'c' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "b", 3, "c");
  }

  {
    // Test IF NOT EXISTS when the row exists.
    // update t set c1 = 6 where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b' if not exists;
    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_UPDATE);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    QLAddInt32RangeValue(req, 2);
    QLAddStringRangeValue(req, "b");
    table_.AddInt32ColumnValue(req, "c1", 6);
    auto* const condition = req->mutable_if_expr()->mutable_condition();
    condition->set_op(QL_OP_NOT_EXISTS);
    CHECK_OK(session->Apply(op));

    // Expect not applied
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    const auto& row = rowblock->row(0);
    EXPECT_EQ(rowblock->schema().column(0).name(), "[applied]");
    EXPECT_EQ(rowblock->schema().column(0).type_info()->type(), BOOL);
    EXPECT_FALSE(row.column(0).bool_value());
  }

  {
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow();

    // Expect 1, 'a', 2, 'b', 3, 'c' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "b", 3, "c");
  }

  {
    // Test IF EXISTS when the row exists.
    // update t set c1 = 6 where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b' if exists;
    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_UPDATE);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    QLAddInt32RangeValue(req, 2);
    QLAddStringRangeValue(req, "b");
    table_.AddInt32ColumnValue(req, "c1", 6);
    auto* const condition = req->mutable_if_expr()->mutable_condition();
    condition->set_op(QL_OP_EXISTS);
    CHECK_OK(session->Apply(op));

    // Expect applied
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    const auto& row = rowblock->row(0);
    EXPECT_EQ(rowblock->schema().column(0).name(), "[applied]");
    EXPECT_EQ(rowblock->schema().column(0).type_info()->type(), BOOL);
    EXPECT_TRUE(row.column(0).bool_value());
  }

  {
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow();

    // Expect 1, 'a', 2, 'b', 6, 'c' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "b", 6, "c");
  }
}

TEST_F(QLDmlTest, TestConditionalDelete) {
  const shared_ptr<YBSession> session(NewSession());
  {
    // insert into t values (1, 'a', 2, 'b', 3, 'c');
    const shared_ptr<YBqlWriteOp> op = InsertRow(session, 1, "a", 2, "b", 3, "c");
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow();

    // Expect 1, 'a', 2, 'b', 3, 'c' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "b", 3, "c");
  }

  {
    // Test IF with a column condition when the column value is different.
    // delete c1 from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b' if c1 = 4;
    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_DELETE);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    QLAddInt32RangeValue(req, 2);
    QLAddStringRangeValue(req, "b");
    table_.SetColumn(req->add_column_values(), "c1");
    table_.SetInt32Condition(req->mutable_if_expr()->mutable_condition(), "c1", QL_OP_EQUAL, 4);
    req->mutable_column_refs()->add_ids(table_.ColumnId("c1"));
    CHECK_OK(session->Apply(op));

    // Expect not applied, return c1 = 3. Verify column names also.
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    const auto& row = rowblock->row(0);
    EXPECT_EQ(rowblock->schema().num_columns(), 6);
    EXPECT_EQ(rowblock->schema().column(0).name(), "[applied]");
    EXPECT_EQ(rowblock->schema().column(0).type_info()->type(), BOOL);
    EXPECT_EQ(rowblock->schema().column(1).name(), "h1");
    EXPECT_EQ(rowblock->schema().column(1).type_info()->type(), INT32);
    EXPECT_EQ(rowblock->schema().column(2).name(), "h2");
    EXPECT_EQ(rowblock->schema().column(2).type_info()->type(), STRING);
    EXPECT_EQ(rowblock->schema().column(3).name(), "r1");
    EXPECT_EQ(rowblock->schema().column(3).type_info()->type(), INT32);
    EXPECT_EQ(rowblock->schema().column(4).name(), "r2");
    EXPECT_EQ(rowblock->schema().column(4).type_info()->type(), STRING);
    EXPECT_EQ(rowblock->schema().column(5).name(), "c1");
    EXPECT_EQ(rowblock->schema().column(5).type_info()->type(), INT32);
    EXPECT_FALSE(row.column(0).bool_value());
    EXPECT_EQ(row.column(1).int32_value(), 1);
    EXPECT_EQ(row.column(2).string_value(), "a");
    EXPECT_EQ(row.column(3).int32_value(), 2);
    EXPECT_EQ(row.column(4).string_value(), "b");
    EXPECT_EQ(row.column(5).int32_value(), 3);
  }

  {
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow();

    // Expect 1, 'a', 2, 'b', 3, 'c' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    EXPECT_ROW_VALUES(rowblock->row(0), 1, "a", 2, "b", 3, "c");
  }

  {
    // Test IF EXISTS AND a column condition when the row exists and the column value matches.
    // delete c1 from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b' if exists and c1 = 3;
    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_DELETE);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    QLAddInt32RangeValue(req, 2);
    QLAddStringRangeValue(req, "b");
    table_.SetColumn(req->add_column_values(), "c1");
    auto* const condition = req->mutable_if_expr()->mutable_condition();
    condition->set_op(QL_OP_AND);
    table_.AddCondition(condition, QL_OP_EXISTS);
    table_.AddInt32Condition(condition, "c1", QL_OP_EQUAL, 3);
    req->mutable_column_refs()->add_ids(table_.ColumnId("c1"));
    CHECK_OK(session->Apply(op));

    // Expect applied
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    const auto& row = rowblock->row(0);
    EXPECT_EQ(rowblock->schema().column(0).name(), "[applied]");
    EXPECT_EQ(rowblock->schema().column(0).type_info()->type(), BOOL);
    EXPECT_TRUE(row.column(0).bool_value());
  }

  {
    // select * from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'b';
    const shared_ptr<YBqlReadOp> op = SelectRow();

    // Expect 1, 'a', 2, 'b', null, 'c' returned
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    const auto& row = rowblock->row(0);
    EXPECT_EQ(row.column(0).int32_value(), 1);
    EXPECT_EQ(row.column(1).string_value(), "a");
    EXPECT_EQ(row.column(2).int32_value(), 2);
    EXPECT_EQ(row.column(3).string_value(), "b");
    EXPECT_TRUE(row.column(4).IsNull());
    EXPECT_EQ(row.column(5).string_value(), "c");
  }

  {
    // Test deleting the whole row with IF EXISTS when the row does not exist (wrong "r1").
    // delete from t where h1 = 1 and h2 = 'a' and r1 = 2 and r2 = 'c' if exists;
    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_DELETE);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    QLAddInt32RangeValue(req, 2);
    QLAddStringRangeValue(req, "c");
    auto* const condition = req->mutable_if_expr()->mutable_condition();
    condition->set_op(QL_OP_EXISTS);
    CHECK_OK(session->Apply(op));

    // Expect not applied
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    const auto& row = rowblock->row(0);
    EXPECT_EQ(rowblock->schema().column(0).name(), "[applied]");
    EXPECT_EQ(rowblock->schema().column(0).type_info()->type(), BOOL);
    EXPECT_FALSE(row.column(0).bool_value());
  }
}

TEST_F(QLDmlTest, TestError) {
  const shared_ptr<YBSession> session(NewSession());
  {
    // insert into t values (1, 'a', 2, 'b', 3, 'c');
    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    QLAddInt32RangeValue(req, 2);
    QLAddStringRangeValue(req, "b");
    table_.AddInt32ColumnValue(req, "c1", 3);
    table_.AddStringColumnValue(req, "c2", "c");
    CHECK_OK(session->Apply(op));

    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }
  {
    // Test selecting with incomparable column condition (int32 column "r1" with a string value).
    // select c1, c2 from t where h1 = 1 and h2 = 'a' and r1 <> '2' and r2 <> 'b';
    const shared_ptr<YBqlReadOp> op = table_.NewReadOp();
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, 1);
    QLAddStringHashValue(req, "a");
    auto* const condition = req->mutable_where_expr()->mutable_condition();
    condition->set_op(QL_OP_AND);
    table_.AddStringCondition(condition, "r1", QL_OP_NOT_EQUAL, "2");
    table_.AddStringCondition(condition, "r2", QL_OP_NOT_EQUAL, "b");
    table_.AddColumns({"c1", "c2"}, req);

    CHECK_OK(session->Apply(op));

    // Expect values not comparable error.
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_RUNTIME_ERROR);
    EXPECT_EQ(op->response().error_message(), "values not comparable");
  }
}

TEST_F(QLDmlTest, TestSimultaneousReadAndWrite) {
  constexpr int kNumIterations = 10;
  const shared_ptr<YBSession> session(NewSession());
  ASSERT_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));
  for (int i = 0; i != kNumIterations; ++i) {
    auto write_op = InsertRow(session, 1, "a", i, "b", i * 2, "c");
    std::shared_ptr<YBqlReadOp> read_op;
    // Not reading on the first iteration, because there is no record yet.
    if (i > 0) {
      read_op = SelectRow(session, { "c1", "c2" }, 1, "a", i - 1, "b");
    }
    ASSERT_OK(session->Flush());
    ASSERT_EQ(QLResponsePB::YQL_STATUS_OK, write_op->response().status());
    if (read_op) {
      ASSERT_EQ(QLResponsePB::YQL_STATUS_OK, read_op->response().status());

      auto rowblock = RowsResult(read_op.get()).GetRowBlock();
      ASSERT_EQ(1, rowblock->row_count());
      const auto& row = rowblock->row(0);
      ASSERT_EQ((i - 1) * 2, row.column(0).int32_value());
      ASSERT_EQ("c", row.column(1).string_value());
    }
  }
}

TEST_F(QLDmlTest, OpenRecentlyCreatedTable) {
  constexpr int kNumIterations = 10;
  constexpr int kNumKeys = 100;
  const auto kOpenTimeout = 30s;
  const auto kMaxWait = 5s;

  for (int i = 0; i != kNumIterations; ++i) {
    client::YBTableName table_name(kTableName.namespace_name(), Format("table_$0", i));
    std::thread table_creation_thread([this, table_name] {
      YBSchemaBuilder builder;
      builder.AddColumn("k")->Type(INT32)->HashPrimaryKey()->NotNull();
      builder.AddColumn("v")->Type(INT32);
      TableHandle table;
      ASSERT_OK(table.Create(table_name, 9, client_.get(), &builder));
    });
    TableHandle table;
    BackoffWaiter waiter(std::chrono::steady_clock::now() + kOpenTimeout, kMaxWait);
    while (!table.Open(table_name, client_.get()).ok()) {
      ASSERT_TRUE(waiter.Wait());
    }
    auto session = NewSession();
    ASSERT_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));
    for (int k = 0; k != kNumKeys; ++k) {
      auto op = table.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
      auto* const req = op->mutable_request();
      QLAddInt32HashValue(req, k);
      table.AddInt32ColumnValue(req, "v", -k);
      ASSERT_OK(session->Apply(op));
    }
    ASSERT_OK(session->Flush());
    table_creation_thread.join();
  }
}

}  // namespace client
}  // namespace yb
