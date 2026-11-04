/*
 * Copyright (c) 2026 OceanBase.
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

#pragma once

#include <cstdint>
#include <string_view>

namespace oceanbase
{
namespace share
{

enum class AiColumnFunction { COMPLETE, EMBED, UNSUPPORTED };
enum class AiColumnResultType { TEXT, JSON, INTEGER, NUMBER, VECTOR, UNSUPPORTED };

enum class AiColumnRuleError {
  OK,
  UNSUPPORTED_SYNC_MODE,
  INVALID_RETRY_COUNT,
  EMPTY_EXPRESSION,
  UNSUPPORTED_FUNCTION,
  UNSUPPORTED_RESULT_TYPE,
  AI_COLUMN_DEPENDENCY,
  INVALID_VECTOR_DIMENSION
};

// Semantic facts from a resolved expression, independent of parser nodes and
// their allocator. The resolver owns the expression string through validation.
struct AiColumnRule
{
  std::string_view normalized_expression;
  std::string_view sync_mode = "async";
  int64_t max_retries = 0;
  AiColumnFunction function = AiColumnFunction::UNSUPPORTED;
  AiColumnResultType result_type = AiColumnResultType::UNSUPPORTED;
  int64_t vector_dimension = 0;
  int64_t requested_embedding_dimension = 0;
  bool has_explicit_embedding_dimension = false;
  bool references_ai_column = false;

  AiColumnRuleError validate() const
  {
    if (!is_async(sync_mode)) {
      return AiColumnRuleError::UNSUPPORTED_SYNC_MODE;
    } else if (max_retries < 0) {
      return AiColumnRuleError::INVALID_RETRY_COUNT;
    } else if (normalized_expression.find_first_not_of(" \t\r\n") == std::string_view::npos) {
      return AiColumnRuleError::EMPTY_EXPRESSION;
    } else if (references_ai_column) {
      return AiColumnRuleError::AI_COLUMN_DEPENDENCY;
    } else if (function == AiColumnFunction::EMBED) {
      if (result_type != AiColumnResultType::VECTOR) {
        return AiColumnRuleError::UNSUPPORTED_RESULT_TYPE;
      } else if (vector_dimension <= 0 ||
                 (has_explicit_embedding_dimension &&
                  requested_embedding_dimension != vector_dimension)) {
        return AiColumnRuleError::INVALID_VECTOR_DIMENSION;
      }
    } else if (function == AiColumnFunction::COMPLETE) {
      if (result_type != AiColumnResultType::TEXT && result_type != AiColumnResultType::JSON &&
          result_type != AiColumnResultType::INTEGER && result_type != AiColumnResultType::NUMBER) {
        return AiColumnRuleError::UNSUPPORTED_RESULT_TYPE;
      }
    } else {
      return AiColumnRuleError::UNSUPPORTED_FUNCTION;
    }
    return AiColumnRuleError::OK;
  }

private:
  static bool is_async(std::string_view value)
  {
    constexpr std::string_view async = "async";
    if (value.size() != async.size()) {
      return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
      const char c = value[i];
      if ((c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c) != async[i]) {
        return false;
      }
    }
    return true;
  }
};

} // namespace share
} // namespace oceanbase
