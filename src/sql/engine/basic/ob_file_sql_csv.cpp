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

#include "ob_file_sql_csv.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <utility>

namespace oceanbase {
namespace sql {
namespace file_sql {
namespace {

constexpr size_t MAX_FILE_SQL_ROW_BYTES = 16 * 1024 * 1024;
constexpr size_t MAX_FILE_SQL_COLUMNS = 4096;

enum class FieldState { START, UNQUOTED, QUOTED, AFTER_QUOTE };

bool digits(const std::string &s, size_t first, size_t last)
{
  for (size_t i = first; i < last; ++i) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
  }
  return true;
}

int number(const std::string &s, size_t first, size_t last)
{
  int result = 0;
  for (size_t i = first; i < last; ++i) {
    result = result * 10 + s[i] - '0';
  }
  return result;
}

bool date_prefix(const std::string &s)
{
  if (s.size() < 10 || s[4] != '-' || s[7] != '-' ||
      !digits(s, 0, 4) || !digits(s, 5, 7) || !digits(s, 8, 10)) {
    return false;
  }
  const int year = number(s, 0, 4);
  const int month = number(s, 5, 7);
  const int day = number(s, 8, 10);
  const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return year > 0 && month >= 1 && month <= 12 && day >= 1 &&
         day <= lengths[month - 1] + (month == 2 && leap ? 1 : 0);
}

std::string lower_ascii(std::string s)
{
  for (char &c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

CsvType infer(const std::string &s)
{
  const auto lower = lower_ascii(s);
  if (lower == "true" || lower == "false") {
    return CsvType::BOOLEAN;
  }
  if (date_prefix(s)) {
    if (s.size() == 10) {
      return CsvType::DATE;
    }
    if (s.size() == 19 && (s[10] == ' ' || s[10] == 'T') &&
        s[13] == ':' && s[16] == ':' &&
        digits(s, 11, 13) && digits(s, 14, 16) && digits(s, 17, 19) &&
        number(s, 11, 13) < 24 && number(s, 14, 16) < 60 && number(s, 17, 19) < 60) {
      return CsvType::DATETIME;
    }
  }
  const size_t start = !s.empty() && (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if (start < s.size() && digits(s, start, s.size())) {
    errno = 0;
    std::strtoll(s.c_str(), nullptr, 10);
    return errno == ERANGE ? CsvType::VARCHAR : CsvType::BIGINT;
  }
  if (!s.empty() && s.find_first_of(".eE") != std::string::npos) {
    std::istringstream stream(s);
    stream.imbue(std::locale::classic());
    double value = 0;
    stream >> std::noskipws >> value;
    if (stream && stream.eof() && std::isfinite(value)) {
      return CsvType::DOUBLE;
    }
  }
  return CsvType::VARCHAR;
}

CsvType common_type(CsvType a, CsvType b)
{
  if (a == CsvType::UNKNOWN || a == b) {
    return b;
  }
  if ((a == CsvType::BIGINT && b == CsvType::DOUBLE) ||
      (a == CsvType::DOUBLE && b == CsvType::BIGINT)) {
    return CsvType::DOUBLE;
  }
  if ((a == CsvType::DATE && b == CsvType::DATETIME) ||
      (a == CsvType::DATETIME && b == CsvType::DATE)) {
    return CsvType::DATETIME;
  }
  return CsvType::VARCHAR;
}

} // namespace

bool CsvReader::next(CsvRow &row)
{
  row.clear();
  std::string field;
  FieldState state = FieldState::START;
  bool has_input = false;
  size_t row_bytes = 0;
  auto finish_field = [&]() {
    if (row.size() == MAX_FILE_SQL_COLUMNS) {
      throw std::length_error("CSV row has too many columns");
    }
    const bool is_null = state == FieldState::START;
    row.push_back({std::move(field), is_null});
    field.clear();
    state = FieldState::START;
  };

  while (true) {
    const int c = input_.get();
    if (c == std::char_traits<char>::eof()) {
      if (input_.bad() || !input_.eof()) {
        throw std::runtime_error("failed reading CSV input");
      }
      if (state == FieldState::QUOTED) {
        throw std::invalid_argument("unterminated CSV quoted field");
      }
      if (has_input) {
        finish_field();
      }
      return has_input;
    }
    has_input = true;
    if (++row_bytes > MAX_FILE_SQL_ROW_BYTES) {
      throw std::length_error("CSV row is too large");
    }

    if (state == FieldState::QUOTED) {
      if (c == '"') {
        state = FieldState::AFTER_QUOTE;
      } else {
        field.push_back(static_cast<char>(c));
      }
    } else if (c == ',' || c == '\n' || c == '\r') {
      finish_field();
      if (c != ',') {
        if (c == '\r' && input_.peek() == '\n') {
          input_.get();
        }
        return true;
      }
    } else if (state == FieldState::AFTER_QUOTE) {
      if (c != '"') {
        throw std::invalid_argument("unexpected data after CSV closing quote");
      }
      field.push_back('"');
      state = FieldState::QUOTED;
    } else if (c == '"') {
      if (state != FieldState::START) {
        throw std::invalid_argument("unexpected quote in unquoted CSV field");
      }
      state = FieldState::QUOTED;
    } else {
      field.push_back(static_cast<char>(c));
      state = FieldState::UNQUOTED;
    }
  }
}

std::vector<CsvColumn> infer_csv_schema(std::istream &input, bool header)
{
  CsvReader reader(input);
  CsvRow row;
  std::vector<CsvColumn> columns;
  if (!reader.next(row)) {
    return columns;
  }

  std::unordered_set<std::string> seen;
  for (size_t i = 0; i < row.size(); ++i) {
    const std::string base = header && !row[i].text.empty()
                                 ? row[i].text : "column" + std::to_string(i);
    std::string name = base;
    for (size_t suffix = 2; !seen.insert(lower_ascii(name)).second; ++suffix) {
      name = base + "__" + std::to_string(suffix);
    }
    columns.push_back({std::move(name), CsvType::UNKNOWN, false});
  }

  auto observe = [&]() {
    if (row.size() != columns.size()) {
      throw std::invalid_argument("inconsistent CSV column count");
    }
    for (size_t i = 0; i < row.size(); ++i) {
      if (row[i].is_null) {
        columns[i].nullable = true;
      } else {
        columns[i].type = common_type(columns[i].type, infer(row[i].text));
      }
    }
  };
  if (!header) {
    observe();
  }
  while (reader.next(row)) {
    observe();
  }
  for (auto &col : columns) {
    if (col.type == CsvType::UNKNOWN) {
      col.type = CsvType::VARCHAR;
    }
  }
  return columns;
}

bool read_json_line(std::istream &input, JsonRow &row)
{
  std::string line;
  while (true) {
    line.clear();
    int ch = 0;
    while ((ch = input.get()) != '\n' && ch != std::char_traits<char>::eof()) {
      if (line.size() == MAX_FILE_SQL_ROW_BYTES) {
        throw std::length_error("JSON line is too large");
      }
      line.push_back(static_cast<char>(ch));
    }
    if (line.empty() && ch == std::char_traits<char>::eof()) {
      break;
    }
    if (line.find_first_not_of(" \t\r") == std::string::npos) {
      continue;
    }
    rapidjson::Document object;
    if (line.find('\0') != std::string::npos ||
        object.Parse<rapidjson::kParseValidateEncodingFlag>(line.c_str()).HasParseError() ||
        !object.IsObject()) {
      throw std::invalid_argument("expected one JSON object per line");
    }
    if (object.MemberCount() > MAX_FILE_SQL_COLUMNS) {
      throw std::length_error("JSON object has too many columns");
    }
    row.clear();
    std::unordered_set<std::string> names;
    for (auto member = object.MemberBegin(); member != object.MemberEnd(); ++member) {
      const auto &value = member->value;
      if (value.IsObject() || value.IsArray()) {
        throw std::invalid_argument("nested JSON values are not supported in FileSQL");
      }
      std::string name(member->name.GetString(), member->name.GetStringLength());
      if (name.empty() || name.find('\0') != std::string::npos || !names.insert(lower_ascii(name)).second) {
        throw std::invalid_argument("invalid or duplicate JSON column name");
      }
      CsvType type = CsvType::UNKNOWN;
      CsvCell cell{{}, value.IsNull()};
      if (value.IsString()) {
        cell.text.assign(value.GetString(), value.GetStringLength());
        const auto inferred = infer(cell.text);
        type = inferred == CsvType::DATE || inferred == CsvType::DATETIME ? inferred : CsvType::VARCHAR;
      } else if (!cell.is_null) {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        if (!value.Accept(writer)) {
          throw std::invalid_argument("could not serialize JSON field");
        }
        cell.text.assign(buffer.GetString(), buffer.GetSize());
        type = value.IsBool() ? CsvType::BOOLEAN : value.IsInt64() ? CsvType::BIGINT :
               value.IsDouble() ? CsvType::DOUBLE : CsvType::VARCHAR;
      }
      row.emplace_back(CsvColumn{std::move(name), type, cell.is_null}, std::move(cell));
    }
    return true;
  }
  if (input.bad()) {
    throw std::runtime_error("could not read JSON lines");
  }
  return false;
}

std::vector<CsvColumn> infer_json_lines_schema(std::istream &input)
{
  std::vector<CsvColumn> columns;
  std::vector<size_t> present;
  std::unordered_map<std::string, size_t> indexes;
  size_t row_count = 0;
  JsonRow row;
  while (read_json_line(input, row)) {
    ++row_count;
    for (const auto &field : row) {
      const auto &column = field.first;
      const auto entry = indexes.emplace(lower_ascii(column.name), columns.size());
      if (entry.second) {
        if (columns.size() == MAX_FILE_SQL_COLUMNS) {
          throw std::length_error("JSON schema has too many columns");
        }
        columns.push_back(column);
        present.push_back(1);
      } else {
        const size_t index = entry.first->second;
        auto &existing = columns[index];
        if (!column.nullable) {
          existing.type = common_type(existing.type, column.type);
        }
        existing.nullable |= column.nullable;
        ++present[index];
      }
    }
  }
  for (size_t i = 0; i < columns.size(); ++i) {
    columns[i].nullable |= present[i] != row_count;
    if (columns[i].type == CsvType::UNKNOWN) {
      columns[i].type = CsvType::VARCHAR;
    }
  }
  return columns;
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
