//--------------------------------------------------------------------------------------------------
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
//--------------------------------------------------------------------------------------------------

#ifndef YB_YQL_PGGATE_PG_STATEMENT_H_
#define YB_YQL_PGGATE_PG_STATEMENT_H_

#include "yb/yql/pggate/pg_env.h"
#include "yb/yql/pggate/pg_session.h"

namespace yb {
namespace pggate {

// Statement types.
// - Might use it for error reporting or debugging or if different operations share the same
//   CAPI calls.
// - TODO(neil) Remove StmtOp if we don't need it.
enum class StmtOp {
  STMT_NOOP = 0,
  STMT_CREATE_DATABASE,
  STMT_DROP_DATABASE,
  STMT_CREATE_SCHEMA,
  STMT_DROP_SCHEMA,
  STMT_CREATE_TABLE,
  STMT_DROP_TABLE,
  STMT_INSERT,
  STMT_UPDATE,
  STMT_DELETE,
  STMT_SELECT,
};

class PgStatement {
 public:
  // Public types.
  typedef std::shared_ptr<PgStatement> SharedPtr;
  typedef std::shared_ptr<const PgStatement> SharedPtrConst;

  typedef std::unique_ptr<PgStatement> UniPtr;
  typedef std::unique_ptr<const PgStatement> UniPtrConst;

  //------------------------------------------------------------------------------------------------
  // Constructors.
  // pg_session is the session that this statement belongs to. If PostgreSQL cancels the session
  // while statement is running, pg_session::sharedptr can still be accessed without crashing.
  PgStatement(PgSession::SharedPtr pg_session, StmtOp stmt_op);
  virtual ~PgStatement();

  //------------------------------------------------------------------------------------------------
  static bool IsValidStmt(PgStatement::SharedPtr stmt, StmtOp op) {
    return (stmt != nullptr && stmt->stmt_op_ == op);
  }

  static bool IsValidHandle(PgStatement *stmt, StmtOp op) {
    return (stmt != nullptr && stmt->stmt_op_ == op);
  }

  // API for error reporting.
  YBCPgErrorCode GetError(const char **error_text);
  bool has_error() {
    return status_.ok();
  }

 protected:
  // YBSession that this statement belongs to.
  PgSession::SharedPtr pg_session_;

  // Statement type.
  StmtOp stmt_op_;

  // Execution status.
  Status status_;
  string errmsg_;
};

}  // namespace pggate
}  // namespace yb

#endif // YB_YQL_PGGATE_PG_STATEMENT_H_
