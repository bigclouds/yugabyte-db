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

#ifndef YB_YQL_PGGATE_PG_DDL_H_
#define YB_YQL_PGGATE_PG_DDL_H_

#include "yb/yql/pggate/pg_statement.h"

namespace yb {
namespace pggate {

//--------------------------------------------------------------------------------------------------
// CREATE DATABASE
//--------------------------------------------------------------------------------------------------
class PgCreateDatabase : public PgStatement {
 public:
  // Public types.
  typedef std::shared_ptr<PgCreateDatabase> SharedPtr;
  typedef std::shared_ptr<const PgCreateDatabase> SharedPtrConst;

  typedef std::unique_ptr<PgCreateDatabase> UniPtr;
  typedef std::unique_ptr<const PgCreateDatabase> UniPtrConst;

  // Constructors.
  PgCreateDatabase(PgSession::SharedPtr pg_session, const char *database_name);
  virtual ~PgCreateDatabase();

  // Execute.
  YBCPgError Exec();

 private:
  const char *database_name_;
};

class PgDropDatabase : public PgStatement {
 public:
  // Public types.
  typedef std::shared_ptr<PgDropDatabase> SharedPtr;
  typedef std::shared_ptr<const PgDropDatabase> SharedPtrConst;

  typedef std::unique_ptr<PgDropDatabase> UniPtr;
  typedef std::unique_ptr<const PgDropDatabase> UniPtrConst;

  // Constructors.
  PgDropDatabase(PgSession::SharedPtr pg_session, const char *database_name, bool if_exist);
  virtual ~PgDropDatabase();

  // Execute.
  YBCPgError Exec();

 private:
  const char *database_name_;
  bool if_exist_;
};

//--------------------------------------------------------------------------------------------------
// CREATE SCHEMA
//--------------------------------------------------------------------------------------------------

class PgCreateSchema : public PgStatement {
 public:
  // Public types.
  typedef std::shared_ptr<PgCreateSchema> SharedPtr;
  typedef std::shared_ptr<const PgCreateSchema> SharedPtrConst;

  typedef std::unique_ptr<PgCreateSchema> UniPtr;
  typedef std::unique_ptr<const PgCreateSchema> UniPtrConst;

  // Constructors.
  PgCreateSchema(PgSession::SharedPtr pg_session,
                 const char *database_name,
                 const char *schema_name,
                 bool if_not_exist);
  virtual ~PgCreateSchema();

  // Execute.
  YBCPgError Exec();

 private:
  const char *database_name_;
  const char *schema_name_;
  bool if_not_exist_;
};

class PgDropSchema : public PgStatement {
 public:
  // Public types.
  typedef std::shared_ptr<PgDropSchema> SharedPtr;
  typedef std::shared_ptr<const PgDropSchema> SharedPtrConst;

  typedef std::unique_ptr<PgDropSchema> UniPtr;
  typedef std::unique_ptr<const PgDropSchema> UniPtrConst;

  // Constructors.
  PgDropSchema(PgSession::SharedPtr pg_session,
               const char *database_name,
               const char *schema_name,
               bool if_exist);
  virtual ~PgDropSchema();

  // Execute.
  YBCPgError Exec();

 private:
  const char *database_name_;
  const char *schema_name_;
  bool if_exist_;
};

//--------------------------------------------------------------------------------------------------
// CREATE TABLE
//--------------------------------------------------------------------------------------------------

class PgCreateTable : public PgStatement {
 public:
  // Public types.
  typedef std::shared_ptr<PgCreateTable> SharedPtr;
  typedef std::shared_ptr<const PgCreateTable> SharedPtrConst;

  typedef std::unique_ptr<PgCreateTable> UniPtr;
  typedef std::unique_ptr<const PgCreateTable> UniPtrConst;

  // Constructors.
  PgCreateTable(PgSession::SharedPtr pg_session,
                const char *database_name,
                const char *schema_name,
                const char *table_name,
                bool if_not_exist);
  virtual ~PgCreateTable();

  YBCPgError AddColumn(const char *col_name, int col_order, int col_type, bool is_hash,
                       bool is_range);

  // Execute.
  YBCPgError Exec();

 private:
  client::YBTableName table_name_;
  bool if_not_exist_;
  client::YBSchemaBuilder schema_builder_;
};

class PgDropTable : public PgStatement {
 public:
  // Public types.
  typedef std::shared_ptr<PgDropTable> SharedPtr;
  typedef std::shared_ptr<const PgDropTable> SharedPtrConst;

  typedef std::unique_ptr<PgDropTable> UniPtr;
  typedef std::unique_ptr<const PgDropTable> UniPtrConst;

  // Constructors.
  PgDropTable(PgSession::SharedPtr pg_session,
              const char *database_name,
              const char *schema_name,
              const char *table_name,
              bool if_exist);
  virtual ~PgDropTable();

  // Execute.
  YBCPgError Exec();

 private:
  client::YBTableName table_name_;
  bool if_exist_;
};

}  // namespace pggate
}  // namespace yb

#endif // YB_YQL_PGGATE_PG_DDL_H_
