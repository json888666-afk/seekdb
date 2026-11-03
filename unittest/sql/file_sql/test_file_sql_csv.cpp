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
#include <cassert>
#include <sstream>
#include <stdexcept>
using namespace oceanbase::sql::file_sql;

int main()
{
  std::istringstream input("ID,name,score,active,day,created,note,id\r\n"
                          "1,Alice,98.5,true,2024-02-29,2026-08-01 10:11:12,\"hello,\nworld\",101\r\n"
                          "2,Bob,87,FALSE,,,plain,102\r\n"
                          "3,Chen,90.25,true,2026-08-03,2026-08-03T12:13:14,\"say \"\"hi\"\"\",103");
  auto columns = infer_csv_schema(input, true);
  assert(columns.size() == 8 && columns[7].name == "id__2");
  const CsvType expected[] = {CsvType::BIGINT, CsvType::VARCHAR, CsvType::DOUBLE,
                              CsvType::BOOLEAN, CsvType::DATE, CsvType::DATETIME,
                              CsvType::VARCHAR, CsvType::BIGINT};
  for (unsigned i = 0; i < columns.size(); ++i) {
    assert(columns[i].type == expected[i]);
    assert(columns[i].nullable == (i == 4 || i == 5));
  }

  std::istringstream records("\"hello,\nworld\",\"say \"\"hi\"\"\",,\"\"\r\n,2,,");
  CsvReader reader(records);
  CsvRow row;
  assert(reader.next(row) && row.size() == 4);
  assert(row[0].text == "hello,\nworld" && row[1].text == "say \"hi\"");
  assert(row[2].is_null && !row[3].is_null && row[3].text.empty());
  assert(reader.next(row) && row.size() == 4 && row[3].is_null);
  assert(!reader.next(row));

  std::istringstream numeric("1,-2,9223372036854775808,2023-02-29\n2e1,3.5,1,text");
  columns = infer_csv_schema(numeric, false);
  assert(columns[0].name == "column0" && columns[0].type == CsvType::DOUBLE);
  assert(columns[1].type == CsvType::DOUBLE && columns[2].type == CsvType::VARCHAR);
  assert(columns[3].type == CsvType::VARCHAR);
  for (const char *malformed : {"a,b\n1", "\"a\"b\n", "\"a\n", "a\"b\n"}) {
    std::istringstream broken(malformed);
    bool threw = false;
    try { (void)infer_csv_schema(broken, true); }
    catch (const std::invalid_argument &) { threw = true; }
    assert(threw);
  }
}
