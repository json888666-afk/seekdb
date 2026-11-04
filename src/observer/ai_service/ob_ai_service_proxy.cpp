/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "config_bridge.h"
#include "observer/ai_service/ob_ai_service_proxy.h"
#include "share/ob_share_util.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/ob_server_struct.h"
#include "share/ob_max_id_fetcher.h"
#include "storage/tablelock/ob_lock_inner_connection_util.h"
#include "observer/ob_inner_sql_connection.h"
#include "common/ob_timeout_ctx.h"
#include "lib/utility/ob_utility.h"
#include "lib/utility/utility.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"

#define USING_LOG_PREFIX SHARE

using namespace oceanbase::share;
using namespace oceanbase::common;
using namespace oceanbase::sqlclient;
using namespace oceanbase::transaction;

int ObAiServiceProxy::claim_ai_column_request(ObArenaAllocator &allocator,
                                                ObISQLClient &sql_proxy,
                                                int64_t lease_us,
                                                ObAiColumnLease &lease)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  ObAiColumnLease claimed;
  if (lease_us <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(trans.start(&sql_proxy))) {
  } else {
    ObSqlString sql;
    OZ (sql.append_fmt("SELECT table_id, request_id FROM oceanbase.%s "
        "WHERE eligible_ts <= NOW(6) AND (lease_expiry IS NULL OR lease_expiry <= NOW(6)) "
        "ORDER BY eligible_ts, table_id, request_id, column_id LIMIT 1 FOR UPDATE",
        OB_ALL_AI_COLUMN_TASK_TNAME));
    {
      ObISQLClient::ReadResult res;
      OZ (trans.read(res, sql.ptr()));
      auto *row = res.get_result();
      CK (OB_NOT_NULL(row));
      OZ (row->next());
      if (ret == OB_ITER_END) {
        ret = OB_ENTRY_NOT_EXIST;
      } else if (OB_SUCC(ret)) {
        ObString request;
        EXTRACT_INT_FIELD_MYSQL(*row, "table_id", claimed.table_id_, uint64_t);
        EXTRACT_VARCHAR_FIELD_MYSQL(*row, "request_id", request);
        OZ (ob_write_string(allocator, request, claimed.request_id_));
      }
    }

    ObSqlString group_condition;
    OZ (group_condition.append_fmt("table_id=%llu AND request_id=",
                                    static_cast<unsigned long long>(claimed.table_id_)));
    OZ (sql_append_hex_escape_str(claimed.request_id_, group_condition));
    sql.reset();
    OZ (sql.append_fmt("SELECT COUNT(*) AS task_count FROM oceanbase.%s WHERE %s",
                       OB_ALL_AI_COLUMN_TASK_TNAME, group_condition.ptr()));
    {
      ObISQLClient::ReadResult res;
      OZ (trans.read(res, sql.ptr()));
      auto *row = res.get_result();
      CK (OB_NOT_NULL(row));
      OZ (row->next());
      if (OB_SUCC(ret)) {
        EXTRACT_INT_FIELD_MYSQL(*row, "task_count", claimed.task_count_, int64_t);
        CK (claimed.task_count_ > 0);
      }
    }

    using oceanbase::sql::ObExprUuid;
    using oceanbase::sql::UuidCommon;
    char token[UuidCommon::LENGTH_UUID + 1] = {};
    OZ (ObExprUuid::gen_server_uuid(token, UuidCommon::LENGTH_UUID));
    OZ (ob_write_string(allocator, ObString::make_string(token), claimed.token_));
    sql.reset();
    OZ (sql.append_fmt("UPDATE oceanbase.%s SET lease_token='%s', "
        "lease_expiry=TIMESTAMPADD(MICROSECOND,%lld,NOW(6)) "
        "WHERE %s AND eligible_ts <= NOW(6) AND (lease_expiry IS NULL OR lease_expiry <= NOW(6))",
        OB_ALL_AI_COLUMN_TASK_TNAME, token, static_cast<long long>(lease_us), group_condition.ptr()));
    int64_t affected = 0;
    OZ (trans.write(sql.ptr(), affected));
    if (OB_SUCC(ret) && affected != claimed.task_count_) {
      ret = OB_EAGAIN;
    }
    ret = trans.handle_trans_in_the_end(ret);
    if (OB_SUCC(ret)) {
      lease = claimed;
    }
  }
  return ret;
}

int ObAiServiceProxy::read_ai_column_request(ObArenaAllocator &allocator,
                                               ObISQLClient &sql_proxy,
                                               const ObAiColumnLease &lease,
                                               ObIArray<ObAiColumnWork> &tasks)
{
  int ret = OB_SUCCESS;
  tasks.reset();
  ObSqlString sql;
  if (lease.table_id_ == OB_INVALID_ID || lease.request_id_.empty() ||
      lease.token_.empty() || lease.task_count_ <= 0) {
    ret = OB_INVALID_ARGUMENT;
  }
  OZ (sql.append_fmt("SELECT t.column_id, t.definition_version, t.retry_count, "
      "t.rowkey_predicate, a.max_retries, a.normalized_expr "
      "FROM oceanbase.%s t JOIN oceanbase.%s a ON a.table_id=t.table_id "
      "AND a.column_id=t.column_id AND a.definition_version=t.definition_version "
      "JOIN oceanbase.%s c ON c.table_id=t.table_id AND c.column_id=t.column_id "
      "WHERE t.table_id=%llu AND t.lease_expiry > NOW(6) AND t.request_id=",
      OB_ALL_AI_COLUMN_TASK_TNAME, OB_ALL_AI_COLUMN_TNAME, OB_ALL_COLUMN_TNAME,
      static_cast<unsigned long long>(lease.table_id_)));
  OZ (sql_append_hex_escape_str(lease.request_id_, sql));
  OZ (sql.append(" AND t.lease_token="));
  OZ (sql_append_hex_escape_str(lease.token_, sql));
  OZ (sql.append(" ORDER BY t.column_id"));

  ObISQLClient::ReadResult result;
  OZ (sql_proxy.read(result, sql.ptr()));
  auto *row = result.get_result();
  CK (OB_NOT_NULL(row));
  while (OB_SUCC(ret) && OB_SUCC(ret = row->next())) {
    ObAiColumnWork task;
    ObString predicate;
    ObString rule;
    EXTRACT_INT_FIELD_MYSQL(*row, "column_id", task.column_id_, uint64_t);
    EXTRACT_INT_FIELD_MYSQL(*row, "definition_version", task.definition_version_, int64_t);
    EXTRACT_INT_FIELD_MYSQL(*row, "retry_count", task.retry_count_, int64_t);
    EXTRACT_INT_FIELD_MYSQL(*row, "max_retries", task.max_retries_, int64_t);
    EXTRACT_VARCHAR_FIELD_MYSQL(*row, "rowkey_predicate", predicate);
    EXTRACT_VARCHAR_FIELD_MYSQL(*row, "normalized_expr", rule);
    OZ (ob_write_string(allocator, predicate, task.predicate_));
    OZ (ob_write_string(allocator, rule, task.rule_));
    OZ (tasks.push_back(task));
  }
  if (ret == OB_ITER_END) {
    ret = tasks.count() == lease.task_count_ ? OB_SUCCESS : OB_EAGAIN;
  }
  if (OB_FAIL(ret)) {
    tasks.reset();
  }
  return ret;
}

int ObAiServiceProxy::publish_ai_column_request(ObISQLClient &sql_proxy,
                                                  const ObAiColumnLease &lease,
                                                  const ObString &writeback_sql)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  ObSqlString writeback;
  ObSqlString remove;
  if (lease.table_id_ == OB_INVALID_ID || lease.request_id_.empty() ||
      lease.token_.empty() || lease.task_count_ <= 0 || writeback_sql.empty()) {
    ret = OB_INVALID_ARGUMENT;
  }
  OZ (writeback.append(writeback_sql));
  OZ (remove.append_fmt("DELETE t FROM oceanbase.%s t JOIN oceanbase.%s a "
      "ON a.table_id=t.table_id AND a.column_id=t.column_id "
      "AND a.definition_version=t.definition_version "
      "WHERE t.table_id=%llu AND t.lease_expiry > NOW(6) AND t.request_id=",
      OB_ALL_AI_COLUMN_TASK_TNAME, OB_ALL_AI_COLUMN_TNAME,
      static_cast<unsigned long long>(lease.table_id_)));
  OZ (sql_append_hex_escape_str(lease.request_id_, remove));
  OZ (remove.append(" AND t.lease_token="));
  OZ (sql_append_hex_escape_str(lease.token_, remove));
  OZ (trans.start(&sql_proxy));
  if (OB_SUCC(ret)) {
    auto *connection = trans.get_connection();
    CK (OB_NOT_NULL(connection));
    ObSessionDDLInfo ddl_info;
    ddl_info.set_ai_column_writeback(true);
    int64_t affected = 0;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(connection->set_ddl_info(&ddl_info))) {
    } else {
      // Acquire the business row before the tasks, as foreground DML does.
      // If the lease was replaced meanwhile, roll back the business update.
      ret = trans.write(writeback.ptr(), affected);
      ddl_info.reset();
      const int reset_ret = connection->set_ddl_info(&ddl_info);
      if (OB_SUCC(ret)) {
        ret = reset_ret;
      }
      if (OB_SUCC(ret) && affected != 1) {
        ret = OB_EAGAIN;
      }
    }
    OZ (trans.write(remove.ptr(), affected));
    if (OB_SUCC(ret) && affected != lease.task_count_) {
      ret = OB_EAGAIN;
    }
  }
  return trans.handle_trans_in_the_end(ret);
}

int ObAiServiceProxy::insert_ai_endpoint(ObMySQLTransaction &trans, const int64_t new_endpoint_version, const ObAiModelEndpointInfo &endpoint)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObDMLSqlSplicer sql;
  ObSqlString buffer;
  
  if (OB_FAIL(sql.add_pk_column("endpoint_id", endpoint.endpoint_id_))) {
  } else if (OB_FAIL(sql.add_pk_column("scope", ObHexEscapeSqlStr(endpoint.scope_)))) {
  } else if (OB_FAIL(sql.add_column("version", new_endpoint_version))) {
  } else if (OB_FAIL(sql.add_column("endpoint_name", endpoint.name_))) {
  } else if (OB_FAIL(sql.add_column("ai_model_name", ObHexEscapeSqlStr(endpoint.ai_model_name_)))) {
  } else if (OB_FAIL(sql.add_column("url", ObHexEscapeSqlStr(endpoint.url_)))) {
  } else if (OB_FAIL(sql.add_column("access_key", ObHexEscapeSqlStr(endpoint.access_key_)))) {
  } else if (OB_FAIL(sql.add_column("request_model_name", ObHexEscapeSqlStr(endpoint.request_model_name_)))) {
  } else if (OB_FAIL(sql.add_column("provider", ObHexEscapeSqlStr(endpoint.provider_)))) {
  } else if (OB_FAIL(sql.add_column("parameters", ObHexEscapeSqlStr(endpoint.parameters_)))) {
  } else if (OB_FAIL(sql.add_column("request_transform_fn", ObHexEscapeSqlStr(endpoint.request_transform_fn_)))) {
  } else if (OB_FAIL(sql.add_column("response_transform_fn", ObHexEscapeSqlStr(endpoint.response_transform_fn_)))) {
  } else if (OB_FAIL(sql.splice_insert_sql(OB_ALL_AI_MODEL_ENDPOINT_TNAME, buffer))) {
  } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
  } else if (1 != affected_rows) {
    ret = OB_ERR_UNEXPECTED;
  }

  if (OB_ERR_PRIMARY_KEY_DUPLICATE == ret) {
    ret = OB_ENTRY_EXIST;
    LOG_USER_ERROR(OB_ENTRY_EXIST, "endpoint already exists");
  }
  return ret;
}

int ObAiServiceProxy::update_ai_endpoint(ObMySQLTransaction &trans, const int64_t new_endpoint_version, const ObAiModelEndpointInfo &endpoint)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer sql;
  ObSqlString buffer;
  int64_t affected_rows = 0;
  
  if (OB_FAIL(sql.add_pk_column("endpoint_id", endpoint.endpoint_id_))) {
  } else if (OB_FAIL(sql.add_pk_column("scope", ObHexEscapeSqlStr(endpoint.scope_)))) {
  } else if (OB_FAIL(sql.add_column("version", new_endpoint_version))) {
  } else if (OB_FAIL(sql.add_column("endpoint_name", endpoint.name_))) {
  } else if (OB_FAIL(sql.add_column("ai_model_name", ObHexEscapeSqlStr(endpoint.ai_model_name_)))) {
  } else if (OB_FAIL(sql.add_column("url", ObHexEscapeSqlStr(endpoint.url_)))) {
  } else if (OB_FAIL(sql.add_column("access_key", ObHexEscapeSqlStr(endpoint.access_key_)))) {
  } else if (OB_FAIL(sql.add_column("request_model_name", ObHexEscapeSqlStr(endpoint.request_model_name_)))) {
  } else if (OB_FAIL(sql.add_column("provider", ObHexEscapeSqlStr(endpoint.provider_)))) {
  } else if (OB_FAIL(sql.add_column("parameters", ObHexEscapeSqlStr(endpoint.parameters_)))) {
  } else if (OB_FAIL(sql.add_column("request_transform_fn", ObHexEscapeSqlStr(endpoint.request_transform_fn_)))) {
  } else if (OB_FAIL(sql.add_column("response_transform_fn", ObHexEscapeSqlStr(endpoint.response_transform_fn_)))) {
  } else if (OB_FAIL(sql.splice_update_sql(OB_ALL_AI_MODEL_ENDPOINT_TNAME, buffer))) {
  } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
  } else if (1 != affected_rows) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObAiServiceProxy::select_ai_endpoint(ObArenaAllocator &allocator, ObISQLClient &sql_proxy,
                                         const ObString &name, ObAiModelEndpointInfo &endpoint, bool for_update)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObTimeoutCtx ctx;
  const int64_t default_timeout = config::internal_sql_execute_timeout();
  endpoint.reset();
  
  if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(ctx, default_timeout))) {
  } else if (OB_FAIL(sql.assign_fmt("SELECT * FROM %s WHERE endpoint_name = ",
      OB_ALL_AI_MODEL_ENDPOINT_TNAME))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(name, sql))) {
  } else if (for_update && (OB_FAIL(sql.append_fmt(" FOR UPDATE")))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(sql_proxy.read(res, sql.ptr()))) {
      } else if (NULL == (result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_AI_FUNC_ENDPOINT_NOT_FOUND;
          LOG_USER_ERROR(OB_AI_FUNC_ENDPOINT_NOT_FOUND, name.length(), name.ptr());
        } else {
        }
      } else if (OB_FAIL(build_ai_endpoint_(allocator, *result, endpoint))) {
      } else {
        int tmp_ret = OB_SUCCESS;
        if (OB_ITER_END != (tmp_ret = result->next())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get more row than one", KR(ret), KR(tmp_ret), K(sql));
        }
      }
    }
  }
  return ret;
}

int ObAiServiceProxy::select_ai_endpoint_by_ai_model_name(ObArenaAllocator &allocator, ObISQLClient &sql_proxy,
  const ObString &ai_model_name, ObNameCaseMode name_case_mode, ObAiModelEndpointInfo &endpoint)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObTimeoutCtx ctx;
  const int64_t default_timeout = config::internal_sql_execute_timeout();
  endpoint.reset();
  
  if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(ctx, default_timeout))) {
  } else if (OB_FAIL(sql.assign_fmt("SELECT * FROM %s ", OB_ALL_AI_MODEL_ENDPOINT_TNAME))) {
  } else {
    if (OB_ORIGIN_AND_INSENSITIVE == name_case_mode || OB_LOWERCASE_AND_INSENSITIVE == name_case_mode) {
      if (OB_FAIL(sql.append(" WHERE ai_model_name = "))) {
      } else if (OB_FAIL(sql_append_hex_escape_str(ai_model_name, sql))) {
      }
    } else if (OB_ORIGIN_AND_SENSITIVE == name_case_mode) {
      if (OB_FAIL(sql.append(" WHERE ai_model_name COLLATE utf8mb4_bin = "))) {
      } else if (OB_FAIL(sql_append_hex_escape_str(ai_model_name, sql))) {
      }
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "invalid name case mode");
    }
  }

  if (OB_SUCC(ret)) {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(sql_proxy.read(res, sql.ptr()))) {
      } else if (NULL == (result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_AI_FUNC_ENDPOINT_NOT_FOUND;
        } else {
        }
      } else if (OB_FAIL(build_ai_endpoint_(allocator, *result, endpoint))) {
      } else {
        int tmp_ret = OB_SUCCESS;
        if (OB_ITER_END != (tmp_ret = result->next())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get more row than one", KR(ret), KR(tmp_ret), K(sql));
        }
      }
    }
  }
  return ret;
}

int ObAiServiceProxy::build_ai_endpoint_(ObArenaAllocator &allocator, ObMySQLResult &result, ObAiModelEndpointInfo &endpoint)
{
  int ret = OB_SUCCESS;
  endpoint.reset();
  ObString scope;
  ObString name;
  ObString ai_model_name;
  ObString url;
  ObString access_key;
  ObString provider;
  ObString request_model_name;
  ObString arguments;
  ObString request_transform_fn;
  ObString response_transform_fn;
  int64_t type = 0;
  EXTRACT_INT_FIELD_MYSQL(result, "endpoint_id", endpoint.endpoint_id_, uint64_t);
  EXTRACT_VARCHAR_FIELD_MYSQL(result, "scope", scope);
  EXTRACT_VARCHAR_FIELD_MYSQL(result, "endpoint_name", name);
  EXTRACT_VARCHAR_FIELD_MYSQL(result, "ai_model_name", ai_model_name);
  EXTRACT_VARCHAR_FIELD_MYSQL(result, "url", url);
  EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(result, "access_key", access_key, true, false, "");
  EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(result, "provider", provider, true, false, ""/*wont be used*/);
  EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(result, "request_model_name", request_model_name, true, false, ""/*wont be used*/);
  EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(result, "parameters", arguments, true, false, ""/*wont be used*/);
  EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(result, "request_transform_fn", request_transform_fn, true, false, ""/*wont be used*/);
  EXTRACT_VARCHAR_FIELD_MYSQL_WITH_DEFAULT_VALUE(result, "response_transform_fn", response_transform_fn, true, false, ""/*wont be used*/);

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, scope, endpoint.scope_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, name, endpoint.name_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, ai_model_name, endpoint.ai_model_name_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, url, endpoint.url_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, access_key, endpoint.access_key_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, provider, endpoint.provider_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, request_model_name, endpoint.request_model_name_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, arguments, endpoint.parameters_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, request_transform_fn, endpoint.request_transform_fn_))) {
  } else if (OB_FAIL(deep_copy_ob_string(allocator, response_transform_fn, endpoint.response_transform_fn_))) {
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(endpoint.check_valid())) {
    }
  }

  return ret;
}

int ObAiServiceProxy::drop_ai_model_endpoint(ObMySQLTransaction &trans, const ObString &name)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  
  if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE endpoint_name = ",
      OB_ALL_AI_MODEL_ENDPOINT_TNAME))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(name, sql))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
  } else if (0 == affected_rows) {
    ret = OB_AI_FUNC_ENDPOINT_NOT_FOUND;
    LOG_USER_ERROR(OB_AI_FUNC_ENDPOINT_NOT_FOUND, name.length(), name.ptr());
  } else if (1 != affected_rows) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObAiServiceProxy::check_ai_endpoint_exists(ObArenaAllocator &allocator, ObISQLClient &sql_proxy, const ObString &name, bool &is_exists)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObTimeoutCtx ctx;
  const int64_t default_timeout = config::internal_sql_execute_timeout();
  
  int64_t count = 0;
  if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(ctx, default_timeout))) {
  } else if (OB_FAIL(sql.assign_fmt("SELECT count(*) FROM %s WHERE endpoint_name = ",
      OB_ALL_AI_MODEL_ENDPOINT_TNAME))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(name, sql))) {
  } else if (OB_FAIL(sql.append(" FOR UPDATE"))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      const int64_t idx = 0;
      ObMySQLResult *result = NULL;
      if (OB_FAIL(sql_proxy.read(res, sql.ptr()))) {
      } else if (NULL == (result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
      } else if (OB_FAIL(result->get_int(idx, count))) {
      } else if (count > 1) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        int tmp_ret = OB_SUCCESS;
        if (OB_ITER_END != (tmp_ret = result->next())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get more row than one", KR(ret), KR(tmp_ret), K(sql));
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    is_exists = count == 1;
  }
  return ret;
}
