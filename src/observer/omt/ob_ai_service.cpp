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

#define USING_LOG_PREFIX SERVER_OMT
#include "ob_ai_service.h"
#include "observer/ai_service/ob_ai_service_proxy.h"
#include "observer/ob_server.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "lib/utility/utility.h"
#include <rapidjson/document.h>
#include "observer/ai_service/ob_ai_service_executor.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_ai/ob_expr_ai_complete.h"
#include "sql/engine/expr/ob_expr_ai/ob_expr_ai_embed.h"
#include <limits>

using namespace oceanbase::share;
using namespace oceanbase::common;
namespace oceanbase
{
namespace omt
{

ObAiService::ObAiService()
  : is_inited_(false)
{
}

ObAiService::~ObAiService()
{
  destroy();
}

int ObAiService::server_module_init(ObAiService* &ai_service)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ai_service)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ai_service->init())) {
  }
  return ret;
}

int ObAiService::init()
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else {
    is_inited_ = true;
  }
  return ret;
}

namespace {

int append_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = OB_SUCCESS;
  OZ (sql.append("`"));
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name[i] == '`') {
      OZ (sql.append("`"));
    }
    OZ (sql.append(name.ptr() + i, 1));
  }
  OZ (sql.append("`"));
  return ret;
}

int read_ai_column_value(ObMySQLProxy &proxy, ObIAllocator &allocator,
                         const ObString &table, const ObAiColumnWork &task,
                         const ObObjMeta &result_type, ObString &value)
{
  int ret = OB_SUCCESS;
  rapidjson::Document rule;
  rule.Parse(task.rule_.ptr(), task.rule_.length());
  ObSqlString sql;
  if (rule.HasParseError() || !rule.IsObject() || !rule.HasMember("expression") ||
      !rule["expression"].IsString()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const auto &expr = rule["expression"];
    ObString system_prompt;
    if (rule.HasMember("system_prompt") && rule["system_prompt"].IsString()) {
      const auto &prompt = rule["system_prompt"];
      system_prompt.assign_ptr(prompt.GetString(), prompt.GetStringLength());
    }
    int64_t dimension = 0;
    if (rule.HasMember("embedding_dimension") && rule["embedding_dimension"].IsInt64()) {
      dimension = rule["embedding_dimension"].GetInt64();
    }
    sql::ObExprAIComplete::SystemPromptGuard prompt_guard(system_prompt);
    sql::ObExprAIEmbed::DimensionGuard dimension_guard(dimension);
    OZ (sql.append("SELECT /*+ parallel(1) */ CAST(("));
    OZ (sql.append(expr.GetString(), expr.GetStringLength()));
    OZ (sql.append_fmt(") AS CHAR) AS ai_value FROM %.*s WHERE %.*s",
                       table.length(), table.ptr(), task.predicate_.length(), task.predicate_.ptr()));
    ObISQLClient::ReadResult result;
    ObSessionParam session_param;
    int64_t sql_mode = 0;
    if (rule.HasMember("sql_mode") && rule["sql_mode"].IsUint64()) {
      sql_mode = static_cast<int64_t>(rule["sql_mode"].GetUint64());
      session_param.sql_mode_ = &sql_mode;
    }
    OZ (proxy.read(result, sql.ptr(), &session_param));
    auto *row = result.get_result();
    CK (OB_NOT_NULL(row));
    OZ (row->next());
    ObString text;
    OZ (row->get_varchar("ai_value", text));
    if (OB_SUCC(ret) && dimension > 0) {
      rapidjson::Document vector;
      vector.Parse(text.ptr(), text.length());
      if (vector.HasParseError() || !vector.IsArray() ||
          vector.Size() != static_cast<uint64_t>(dimension)) {
        ret = OB_INVALID_DATA;
      } else {
        constexpr double max_element = std::numeric_limits<float>::max();
        for (auto i = vector.Begin(); OB_SUCC(ret) && i != vector.End(); ++i) {
          if (!i->IsNumber() || i->GetDouble() < -max_element || i->GetDouble() > max_element) {
            ret = OB_INVALID_DATA;
          }
        }
      }
    } else if (OB_SUCC(ret) && (result_type.is_json() || result_type.is_numeric_type() ||
                               ob_is_decimal_int(result_type.get_type()))) {
      rapidjson::Document typed;
      typed.Parse(text.ptr(), text.length());
      if (typed.HasParseError() ||
          (result_type.is_integer_type() && !typed.IsInt64() && !typed.IsUint64()) ||
          (!result_type.is_json() && !typed.IsNumber())) {
        ret = OB_INVALID_DATA;
      }
    }
    OZ (ob_write_string(allocator, text, value));
  }
  return ret;
}

int execute_ai_column_group(ObMySQLProxy &proxy, ObArenaAllocator &allocator, const ObAiColumnLease &lease)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObAiColumnWork, 8> tasks;
  OZ (ObAiServiceProxy::read_ai_column_request(allocator, proxy, lease, tasks));
  schema::ObSchemaGetterGuard guard;
  const schema::ObTableSchema *table = nullptr;
  const schema::ObDatabaseSchema *database = nullptr;
  CK (OB_NOT_NULL(GCTX.schema_service_));
  OZ (GCTX.schema_service_->get_runtime_schema_guard(guard));
  OZ (guard.get_table_schema(lease.table_id_, table));
  CK (OB_NOT_NULL(table));
  OZ (guard.get_database_schema(table->get_database_id(), database));
  CK (OB_NOT_NULL(database));

  ObSqlString qualified;
  ObSqlString writeback;
  OZ (append_identifier(qualified, database->get_database_name_str()));
  OZ (qualified.append("."));
  OZ (append_identifier(qualified, table->get_table_name_str()));
  OZ (writeback.append_fmt("UPDATE /*+ use_plan_cache(none) */ %s SET ", qualified.ptr()));
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
    const auto &task = tasks.at(i);
    const auto *column = table->get_column_schema(task.column_id_);
    ObString value = task.result_;
    ObAiColumnState state = task.state_;
    CK (OB_NOT_NULL(column), column->is_ai_result_column());
    if (OB_SUCC(ret) && (state == ObAiColumnState::PENDING || state == ObAiColumnState::RETRY_WAIT)) {
      const int model_ret = read_ai_column_value(proxy, allocator, qualified.string(),
                                                  task, column->get_meta_type(), value);
      if (model_ret == OB_ITER_END) {
        return OB_EAGAIN;
      } else if (model_ret != OB_SUCCESS && task.retry_count_ < task.max_retries_) {
        return ObAiServiceProxy::retry_ai_column_request(proxy, lease, task.column_id_);
      }
      state = model_ret == OB_SUCCESS ? ObAiColumnState::READY : ObAiColumnState::FAILED;
      OZ (ObAiServiceProxy::save_ai_column_result(proxy, lease, task.column_id_, state, value));
    }
    if (i != 0) {
      OZ (writeback.append(", "));
    }
    OZ (append_identifier(writeback, column->get_column_name_str()));
    OZ (writeback.append("="));
    if (state == ObAiColumnState::FAILED) {
      OZ (writeback.append("NULL"));
    } else if (OB_SUCC(ret) && !column->get_meta_type().is_string_type()) {
      // Convert text on assignment, including JSON and vector types.
      OZ (writeback.append("CONVERT("));
      OZ (sql_append_hex_escape_str(value, writeback));
      OZ (writeback.append(" USING utf8mb4)"));
    } else {
      OZ (sql_append_hex_escape_str(value, writeback));
    }
    char state_name[64];
    OZ (schema::ObColumnSchemaV2::ai_column_companion_name(state_name, sizeof(state_name), task.column_id_, 0));
    OZ (writeback.append_fmt(", `%s`=%lld", state_name, static_cast<long long>(state)));
  }
  if (OB_SUCC(ret)) {
    OZ (writeback.append(" WHERE "));
    OZ (writeback.append(tasks.at(0).predicate_));
    OZ (ObAiServiceProxy::publish_ai_column_request(proxy, lease, writeback.string()));
  }
  return ret;
}

} // namespace

int ObAiService::start()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ai_column_timer_.init("AiColumn"))) {
  } else if (OB_FAIL(ai_column_timer_.schedule(*this, 200000, true))) {
    ai_column_timer_.destroy();
  }
  return ret;
}

void ObAiService::stop()
{
  ai_column_timer_.stop();
}

void ObAiService::wait()
{
  ai_column_timer_.wait();
}

void ObAiService::runTimerTask()
{
  if (GCTX.in_bootstrap_ || GCTX.sql_proxy_ == nullptr) {
    return;
  }
  ObArenaAllocator allocator("AiColumnWorker");
  ObAiColumnLease lease;
  int ret = ObAiServiceProxy::claim_ai_column_request(allocator, *GCTX.sql_proxy_, 60000000, lease);
  if (OB_SUCC(ret)) {
    ret = execute_ai_column_group(*GCTX.sql_proxy_, allocator, lease);
  }
  if (ret != OB_SUCCESS && ret != OB_ENTRY_NOT_EXIST && ret != OB_EAGAIN) {
    LOG_WARN("AI column worker could not process a request", K(ret), K(lease.table_id_));
  }
}

void ObAiService::destroy()
{
  ai_column_timer_.destroy();
  is_inited_ = false;
}

int ObAiService::get_ai_service_guard(ObAiServiceGuard &ai_service_guard)
{
  // TODO: implement after cache support
  int ret = OB_SUCCESS;
  return ret;
}

ObAiServiceGuard::ObAiServiceGuard()
  : local_allocator_(ObMemAttr("AiServiceGuard", ObCtxIds::DEFAULT_CTX_ID))
{
}

ObAiServiceGuard::~ObAiServiceGuard()
{
}

int ObAiServiceGuard::check_access_privilege()
{
  int ret = OB_SUCCESS;
  bool has_priv = false;

  sql::ObSQLSessionInfo *session = nullptr;
  if (OB_ISNULL(session = THIS_WORKER.get_session())) {
    // system background task thread do not have session, skip check
  } else {
    ObArenaAllocator tmp_allocator;
    share::schema::ObSchemaGetterGuard *schema_guard = session->get_cur_exec_ctx()->get_sql_ctx()->schema_guard_;
    if (OB_ISNULL(schema_guard)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      sql::ObAIServiceEndpointPrivUtil priv_util(*schema_guard);
      share::schema::ObSessionPrivInfo session_priv;
      uint64_t user_id = session->get_priv_user_id();
      if (user_id == OB_INVALID_ID) {
        user_id = OB_SYS_USER_ID;
      }
      if (OB_FAIL(schema_guard->get_session_priv_info(
                                                    user_id,
                                                    session->get_database_name(),
                                                    session_priv))) {
      } else if (OB_FAIL(priv_util.check_access_ai_model_priv(tmp_allocator, session_priv, has_priv))) {
      } else if (!has_priv) {
        ret = OB_ERR_NO_PRIVILEGE;
        LOG_WARN("no privilege for ai model access", K(ret));
        LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "access ai model");
      }
    }
  }

  return ret;
}

int ObAiServiceGuard::get_ai_endpoint(const common::ObString &name, const share::ObAiModelEndpointInfo *&endpoint_info)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (OB_FAIL(check_access_privilege())) {
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint(local_allocator_, name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

int ObAiServiceGuard::get_ai_endpoint_by_ai_model_name(const common::ObString &ai_model_name, const share::ObAiModelEndpointInfo *&endpoint_info, bool need_check)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (need_check && OB_FAIL(check_access_privilege())) {
    LOG_WARN("failed to check access privilege", K(ret));
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint_by_ai_model_name(local_allocator_, ai_model_name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

} // namespace omt
} // namespace oceanbase
