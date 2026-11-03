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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (!ob_is_string_type(types_stack[CONTENT_IDX].get_type())) {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    }
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_content->is_null()) {
    res.set_null();
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or content is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);

    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content;
    const ObExpr &content_expr = *expr.args_[CONTENT_IDX];
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(
            ctx.exec_ctx_, temp_allocator, *arg_content, content_expr.datum_meta_,
            content_expr.obj_meta_.has_lob_header(), content))) {
    } else if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->is_null() ? 0 : arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

// Group adjacent compatible rows into one provider request, preserving the
// association between each response and its original expression row.
int ObExprAIEmbed::eval_ai_embed_batch(BATCH_EVAL_FUNC_ARG_DECL)
{
  int ret = OB_SUCCESS;
  ObEvalCtx::BatchInfoScopeGuard batch_guard(ctx);
  batch_guard.set_batch_size(size);
  ObEvalCtx::TempAllocGuard alloc_guard(ctx);
  MultimodeAlloctor allocator(alloc_guard.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
  ObBitVector &evaluated = expr.get_evaluated_flags(ctx);
  ObDatum *results = expr.locate_batch_datums(ctx);
  ObArray<ObString> contents;
  ObSEArray<int64_t, 16> row_indexes;
  ObString group_model;
  int64_t group_dim = 0;
  const int64_t max_request_rows = 8;
  auto flush = [&]() -> int {
    int ret = OB_SUCCESS;
    if (!contents.empty()) {
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo endpoint;
      query::ObIAiEndpointResolver *resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      ObJsonObject *config = nullptr;
      ObJsonInt *dimension = nullptr;
      ObArray<ObString> embeddings;
      if (OB_ISNULL(resolver)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, group_model, info))) {
      } else if (OB_FAIL(resolver->resolve_by_model_name(group_model, allocator, endpoint))) {
      } else if (group_dim > 0
                 && (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))
                     || OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, group_dim, dimension))
                     || OB_FAIL(config->add("dimensions", dimension)))) {
      } else {
        ObAIFuncModel model(allocator, *info, endpoint);
        if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, embeddings))) {
        } else if (OB_UNLIKELY(embeddings.count() != row_indexes.count())) {
          ret = OB_INVALID_DATA;
        } else {
          for (int64_t j = 0; OB_SUCC(ret) && j < row_indexes.count(); ++j) {
            const int64_t row = row_indexes.at(j);
            batch_guard.set_batch_idx(row);
            if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, results[row], embeddings.at(j)))) {
            } else {
              evaluated.set(row);
            }
          }
        }
      }
      contents.reuse();
      row_indexes.reuse();
    }
    return ret;
  };

  for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
    if (skip.at(i) || evaluated.at(i)) {
      continue;
    }
    batch_guard.set_batch_idx(i);
    ObDatum *model_id = nullptr;
    ObDatum *content = nullptr;
    ObDatum *dim = nullptr;
    if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, model_id, content, dim))
                           : OB_FAIL(expr.eval_param_value(ctx, model_id, content))) {
    } else if (content->is_null()) {
      results[i].set_null();
      evaluated.set(i);
    } else if (model_id->is_null() || model_id->get_string().empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
    } else if (OB_NOT_NULL(dim) && (dim->is_null() || dim->get_int() <= 0)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
    } else {
      const ObString model = model_id->get_string();
      const int64_t dimension = OB_ISNULL(dim) ? 0 : dim->get_int();
      if (!contents.empty()
          && (group_model != model || group_dim != dimension || contents.count() == max_request_rows)
          && OB_FAIL(flush())) {
      } else {
        batch_guard.set_batch_idx(i);
        const ObExpr &content_expr = *expr.args_[CONTENT_IDX];
        ObString input;
        group_model = model;
        group_dim = dimension;
        if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                ctx.exec_ctx_, allocator, *content, content_expr.datum_meta_,
                content_expr.obj_meta_.has_lob_header(), input))) {
        } else if (input.empty()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
        } else if (OB_FAIL(contents.push_back(input))) {
        } else if (OB_FAIL(row_indexes.push_back(i))) {
        }
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(flush())) {
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx,
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
