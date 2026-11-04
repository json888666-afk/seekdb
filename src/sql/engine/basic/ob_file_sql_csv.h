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

#include <istream>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace oceanbase {
namespace common { class ObString; }
namespace sql {
class ObSQLSessionInfo;
namespace file_sql {

int open_local_csv(ObSQLSessionInfo &session, const common::ObString &path,
                   std::unique_ptr<std::istream> &input);
int open_local_glob(ObSQLSessionInfo &session, const common::ObString &pattern,
                    std::unique_ptr<std::istream> &input, bool validate_only = false);

struct CsvCell
{
  std::string text;
  bool is_null;
};

using CsvRow = std::vector<CsvCell>;

enum class CsvType { UNKNOWN, BIGINT, DOUBLE, BOOLEAN, DATE, DATETIME, VARCHAR };

struct CsvColumn
{
  std::string name;
  CsvType type;
  bool nullable;
};

// A reader owns no file or rows. Its caller supplies an already-authorized
// input stream and can scan twice (infer, then execute) without materializing
// the entire file.
class CsvReader
{
public:
  explicit CsvReader(std::istream &input) : input_(input) {}
  // Returns false at EOF; throws on malformed quoting or a read failure.
  bool next(CsvRow &row);

private:
  std::istream &input_;
};

// Returns the schema in file order. A headerless scan uses column0, column1,
// etc. Unquoted empty fields are NULL; quoted empty fields are empty strings.
std::vector<CsvColumn> infer_csv_schema(std::istream &input, bool header);

using JsonRow = std::vector<std::pair<CsvColumn, CsvCell>>;
bool read_json_line(std::istream &input, JsonRow &row);
std::vector<CsvColumn> infer_json_lines_schema(std::istream &input);

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
