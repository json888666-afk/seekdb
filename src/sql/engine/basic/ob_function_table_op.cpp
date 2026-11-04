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

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/schema/ob_priv_type.h"
#include "common/timezone/ob_time_convert.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <locale>
#include <sstream>
#include <stdexcept>
#ifdef __linux__
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "sql/engine/expr/ob_expr_lob_utils.h"


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_,
                    file_sql_source_columns_, file_sql_column_count_, file_sql_csv_header_, file_sql_json_lines_, file_sql_json_names_, file_sql_glob_, file_sql_parquet_, file_sql_snapshot_);

#ifdef __linux__
namespace {

std::string file_sql_snapshot(const struct stat &st)
{
  return std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" +
         std::to_string(st.st_size) + ":" + std::to_string(st.st_mtim.tv_sec) + ":" +
         std::to_string(st.st_mtim.tv_nsec) + ":" + std::to_string(st.st_ctim.tv_sec) + ":" +
         std::to_string(st.st_ctim.tv_nsec);
}

class FileSqlInputBuffer : public std::streambuf
{
public:
  FileSqlInputBuffer(FILE *file, std::string snapshot)
    : file_(file, &std::fclose), snapshot_(std::move(snapshot)) {}
private:
  int_type underflow() override
  {
    if (gptr() != egptr()) {
      return traits_type::to_int_type(*gptr());
    }
    struct stat current {};
    if (::fstat(::fileno(file_.get()), &current) != 0 || file_sql_snapshot(current) != snapshot_) {
      throw std::runtime_error("file changed during FileSQL scan");
    }
    const size_t count = std::fread(buffer_, 1, sizeof(buffer_), file_.get());
    if (count == 0) {
      if (std::ferror(file_.get())) {
        throw std::runtime_error("read_csv file read failed");
      }
      return traits_type::eof();
    }
    setg(buffer_, buffer_, buffer_ + count);
    return traits_type::to_int_type(*gptr());
  }

  std::unique_ptr<FILE, decltype(&std::fclose)> file_;
  std::string snapshot_;
  char buffer_[8192];
};

class FileSqlInputStream : public std::istream
{
public:
  FileSqlInputStream(FILE *file, std::string snapshot)
    : std::istream(nullptr), buffer_(file, std::move(snapshot))
  {
    rdbuf(&buffer_);
  }
private:
  FileSqlInputBuffer buffer_;
};

extern "C" {
void *seekdb_file_parquet_open(const char *path, const int64_t *projection, int64_t count);
void seekdb_file_parquet_close(void *scan);
int seekdb_file_parquet_column(const void *scan, size_t index, const char **name, size_t *length, bool *nullable);
int seekdb_file_parquet_next(void *scan, const char **data, size_t *length);
}

using FileSqlParquetScan = std::unique_ptr<void, decltype(&seekdb_file_parquet_close)>;

class FileSqlParquetBuffer : public std::streambuf
{
public:
  explicit FileSqlParquetBuffer(FileSqlParquetScan scan) : scan_(std::move(scan)) {}
private:
  int_type underflow() override
  {
    if (gptr() != egptr()) {
      return traits_type::to_int_type(*gptr());
    }
    const char *data = nullptr;
    size_t length = 0;
    const int status = seekdb_file_parquet_next(scan_.get(), &data, &length);
    if (status < 0) {
      throw std::runtime_error("could not decode Parquet row");
    }
    if (status == 0) {
      return traits_type::eof();
    }
    row_.assign(data, length);
    setg(row_.data(), row_.data(), row_.data() + row_.size());
    return traits_type::to_int_type(*gptr());
  }

  FileSqlParquetScan scan_;
  std::string row_;
};

class FileSqlParquetStream : public std::istream
{
public:
  explicit FileSqlParquetStream(FileSqlParquetScan scan)
    : std::istream(nullptr), buffer_(std::move(scan))
  {
    rdbuf(&buffer_);
  }
private:
  FileSqlParquetBuffer buffer_;
};

int open_file_sql_parquet(const std::string &path, std::unique_ptr<std::istream> &input,
                          std::vector<file_sql::CsvColumn> *schema,
                          const int64_t *projection, int64_t projection_count)
{
  FileSqlParquetScan scan(seekdb_file_parquet_open(path.c_str(), projection, projection_count),
                          &seekdb_file_parquet_close);
  if (!scan) {
    return OB_INVALID_ARGUMENT;
  }
  if (schema != nullptr) {
    for (size_t index = 0; ; ++index) {
      const char *name = nullptr;
      size_t length = 0;
      bool nullable = false;
      const int kind = seekdb_file_parquet_column(scan.get(), index, &name, &length, &nullable);
      if (kind == 0) {
        break;
      }
      file_sql::CsvType type;
      switch (kind) {
        case 1: type = file_sql::CsvType::BIGINT; break;
        case 2: type = file_sql::CsvType::DOUBLE; break;
        case 3: type = file_sql::CsvType::BOOLEAN; break;
        case 6: type = file_sql::CsvType::VARCHAR; break;
        default: return OB_INVALID_ARGUMENT;
      }
      schema->push_back({std::string(name, length), type, nullable});
    }
  } else {
    input = std::make_unique<FileSqlParquetStream>(std::move(scan));
  }
  return OB_SUCCESS;
}

struct FileSqlDescriptor
{
  explicit FileSqlDescriptor(int fd) : fd_(fd) {}
  ~FileSqlDescriptor() { if (fd_ >= 0) { ::close(fd_); } }
  int fd_;
};

bool file_is_within(const std::filesystem::path &root, const std::filesystem::path &file)
{
  auto it = file.begin();
  for (const auto &component : root) {
    if (it == file.end() || *it != component) {
      return false;
    }
    ++it;
  }
  return it != file.end();
}

} // namespace
#endif

int file_sql::open_local_csv(ObSQLSessionInfo &session, const ObString &path,
                             std::unique_ptr<std::istream> &input, bool parquet,
                             std::vector<file_sql::CsvColumn> *schema,
                             const int64_t *projection, int64_t projection_count,
                             std::string *snapshot, const ObString *expected)
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  input.reset();
  if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "FileSQL while enable_file_sql is OFF");
  } else if (0 == (session.get_user_priv_set() & OB_PRIV_FILE)) {
    ret = OB_ERR_NO_PRIVILEGE;
  } else {
#ifdef __linux__
    ObString secure_dir;
    if (OB_FAIL(session.get_secure_file_priv(secure_dir))) {
    } else if (secure_dir.empty() || 0 == secure_dir.case_compare("NULL")) {
      ret = OB_ERR_NO_PRIVILEGE;
    } else if (path.empty()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      try {
        const std::string filename(path.ptr(), path.length());
        const std::string directory(secure_dir.ptr(), secure_dir.length());
        if (filename.find('\0') != std::string::npos || filename.find("://") != std::string::npos ||
            directory.find('\0') != std::string::npos) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          namespace fs = std::filesystem;
          std::error_code ec;
          const fs::path root = fs::canonical(directory, ec);
          if (ec || !fs::is_directory(root, ec) || ec) {
            ret = OB_ERR_NO_PRIVILEGE;
          } else {
            // O_PATH pins the object without opening a device/FIFO for I/O.
            FileSqlDescriptor fd(::open(filename.c_str(), O_PATH | O_CLOEXEC));
            struct stat st {};
            if (fd.fd_ < 0 || ::fstat(fd.fd_, &st) != 0 || !S_ISREG(st.st_mode)) {
              ret = OB_FILE_NOT_EXIST;
            } else {
              const std::string proc_path = "/proc/self/fd/" + std::to_string(fd.fd_);
              const fs::path resolved = fs::canonical(proc_path, ec);
              if (ec || !file_is_within(root, resolved)) {
                ret = OB_ERR_NO_PRIVILEGE;
              } else {
                auto fingerprint = file_sql_snapshot(st);
                if (snapshot != nullptr) {
                  *snapshot = fingerprint;
                }
                if (expected != nullptr && *expected != ObString(fingerprint.size(), fingerprint.data())) {
                  ret = OB_INVALID_ARGUMENT;
                  LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file changed since FileSQL schema inference");
                } else if (parquet) {
                  ret = open_file_sql_parquet(proc_path, input, schema, projection, projection_count);
                } else {
                  std::unique_ptr<FILE, decltype(&std::fclose)> file(
                      std::fopen(proc_path.c_str(), "rb"), &std::fclose);
                  if (!file) {
                    ret = OB_FILE_NOT_EXIST;
                  } else {
                    input = std::make_unique<FileSqlInputStream>(file.get(), std::move(fingerprint));
                    file.release();
                  }
                }
              }
            }
          }
        }
      } catch (const std::exception &e) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("could not open local read_csv file", K(ret), "reason", e.what());
      }
    }
#else
    UNUSED(path);
    UNUSED(parquet);
    UNUSED(schema);
    UNUSED(projection);
    UNUSED(projection_count);
    UNUSED(snapshot);
    UNUSED(expected);
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "FileSQL on this platform");
#endif
  }
  return ret;
}

int file_sql::open_local_glob(ObSQLSessionInfo &session, const ObString &pattern,
                              std::unique_ptr<std::istream> &input, bool validate_only)
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  input.reset();
  if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "FileSQL while enable_file_sql is OFF");
  } else if (0 == (session.get_user_priv_set() & OB_PRIV_FILE)) {
    ret = OB_ERR_NO_PRIVILEGE;
  } else {
#ifdef __linux__
    ObString secure_dir;
    if (OB_FAIL(session.get_secure_file_priv(secure_dir))) {
    } else if (secure_dir.empty() || 0 == secure_dir.case_compare("NULL")) {
      ret = OB_ERR_NO_PRIVILEGE;
    } else if (pattern.empty()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      try {
        namespace fs = std::filesystem;
        const std::string text(pattern.ptr(), pattern.length());
        const std::string directory(secure_dir.ptr(), secure_dir.length());
        const fs::path path(text);
        const fs::path parent = path.has_parent_path() ? path.parent_path() : fs::path(".");
        const std::string leaf = path.filename().string();
        std::error_code ec;
        if (text.find('\0') != std::string::npos || text.find("://") != std::string::npos ||
            directory.find('\0') != std::string::npos || leaf.empty()) {
          ret = OB_INVALID_ARGUMENT;
        } else if (parent.string().find_first_of("*?[") != std::string::npos) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "FileSQL glob wildcards in directory names");
        } else {
          const fs::path root = fs::canonical(directory, ec);
          FileSqlDescriptor dir(::open(parent.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
          if (ec || dir.fd_ < 0) {
            ret = OB_ERR_NO_PRIVILEGE;
          } else {
            const std::string proc = "/proc/self/fd/" + std::to_string(dir.fd_);
            const fs::path resolved = fs::canonical(proc, ec);
            if (ec || (resolved != root && !file_is_within(root, resolved))) {
              ret = OB_ERR_NO_PRIVILEGE;
            } else if (!validate_only) {
              std::ostringstream rows;
              for (const auto &entry : fs::directory_iterator(proc)) {
                const std::string name = entry.path().filename().string();
                if (0 != ::fnmatch(leaf.c_str(), name.c_str(), FNM_PERIOD)) {
                  continue;
                }
                FileSqlDescriptor file(::openat(dir.fd_, name.c_str(), O_PATH | O_CLOEXEC));
                struct stat st {};
                if (file.fd_ < 0 || ::fstat(file.fd_, &st) != 0 || !S_ISREG(st.st_mode)) {
                  continue;
                }
                const fs::path target = fs::canonical("/proc/self/fd/" + std::to_string(file.fd_), ec);
                if (ec || !file_is_within(root, target)) {
                  continue;
                }
                rows << '"';
                for (char ch : (resolved / name).string()) {
                  if (ch == '"') {
                    rows << '"';
                  }
                  rows << ch;
                }
                rows << "\"\n";
              }
              input = std::make_unique<std::istringstream>(rows.str());
            }
          }
        }
      } catch (const std::exception &e) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("could not list FileSQL glob matches", K(ret), "reason", e.what());
      }
    }
#else
    UNUSED(pattern);
    UNUSED(validate_only);
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "FileSQL on this platform");
#endif
  }
  return ret;
}

int ObFunctionTableOp::open_csv()
{
  int ret = OB_SUCCESS;
  ObDatum *value = nullptr;
  auto *session = ctx_.get_my_session();
  csv_input_.reset();
  csv_row_.clear();
  if (OB_ISNULL(session) || OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
  } else if (value->is_null()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (MY_SPEC.file_sql_glob_) {
    ret = file_sql::open_local_glob(*session, value->get_string(), csv_input_);
  } else if (OB_FAIL(file_sql::open_local_csv(*session, value->get_string(), csv_input_, MY_SPEC.file_sql_parquet_,
                                                nullptr, MY_SPEC.file_sql_source_columns_.empty() ? nullptr :
                                                    &MY_SPEC.file_sql_source_columns_.at(0),
                                                MY_SPEC.file_sql_source_columns_.count(), nullptr,
                                                &MY_SPEC.file_sql_snapshot_))) {
  } else if (MY_SPEC.file_sql_csv_header_) {
    try {
      if (!file_sql::CsvReader(*csv_input_).next(csv_row_) ||
          csv_row_.size() != static_cast<size_t>(MY_SPEC.file_sql_column_count_)) {
        ret = OB_INVALID_ARGUMENT;
      }
    } catch (const std::exception &e) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("could not read CSV header", K(ret), "reason", e.what());
    }
  }
  if (OB_FAIL(ret)) {
    csv_input_.reset();
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_csv_row()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (!csv_input_ ||
      MY_SPEC.file_sql_source_columns_.count() != MY_SPEC.column_exprs_.count()) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    try {
      const bool json_lines = MY_SPEC.file_sql_json_lines_;
      const bool has_row = json_lines ? file_sql::read_json_line(*csv_input_, json_row_) :
                                       file_sql::CsvReader(*csv_input_).next(csv_row_);
      if (!has_row) {
        ret = OB_ITER_END;
      } else if (!json_lines && csv_row_.size() != static_cast<size_t>(MY_SPEC.file_sql_column_count_)) {
        ret = OB_INVALID_ARGUMENT;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
        ObExpr *expr = MY_SPEC.column_exprs_.at(i);
        const auto index = MY_SPEC.file_sql_source_columns_.at(i);
        if (index < 0 || index >= MY_SPEC.file_sql_column_count_) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        const file_sql::CsvCell missing{{}, true};
        const file_sql::CsvCell *source = &missing;
        if (json_lines) {
          const auto &name = MY_SPEC.file_sql_json_names_.at(i);
          for (const auto &field : json_row_) {
            if (ObCharset::case_insensitive_equal(name, ObString(field.first.name.size(), field.first.name.data()))) {
              source = &field.second;
              break;
            }
          }
        } else {
          source = &csv_row_[index];
        }
        const auto &cell = *source;
        const ObString text(cell.text.size(), cell.text.data());
        auto &datum = expr->locate_datum_for_write(eval_ctx_);
        if (cell.is_null) {
          datum.set_null();
        } else {
          switch (expr->datum_meta_.type_) {
            case ObIntType: {
              char *end = nullptr;
              errno = 0;
              const auto value = std::strtoll(cell.text.c_str(), &end, 10);
              if (errno == ERANGE || end == cell.text.c_str() || *end != '\0') {
                ret = OB_INVALID_ARGUMENT;
              } else {
                datum.set_int(value);
              }
              break;
            }
            case ObDoubleType: {
              std::istringstream number(cell.text);
              number.imbue(std::locale::classic());
              double value = 0;
              number >> std::noskipws >> value;
              if (!number || !number.eof() || !std::isfinite(value)) {
                ret = OB_INVALID_ARGUMENT;
              } else {
                datum.set_double(value);
              }
              break;
            }
            case ObTinyIntType:
              if (0 == text.case_compare("true")) {
                datum.set_int(1);
              } else if (0 == text.case_compare("false")) {
                datum.set_int(0);
              } else {
                ret = OB_INVALID_ARGUMENT;
              }
              break;
            case ObDateType: {
              int32_t value = 0;
              if (OB_SUCC(ObTimeConverter::str_to_date(text, value))) {
                datum.set_date(value);
              } else {
                ret = OB_INVALID_ARGUMENT;
              }
              break;
            }
            case ObDateTimeType: {
              int64_t value = 0;
              if (OB_SUCC(ObTimeConverter::str_to_datetime(text, ObTimeConvertCtx(nullptr, false), value))) {
                datum.set_datetime(value);
              } else {
                ret = OB_INVALID_ARGUMENT;
              }
              break;
            }
            default:
              datum.set_string(text);
              break;
          }
        }
        if (OB_SUCC(ret)) {
          expr->set_evaluated_projected(eval_ctx_);
        }
      }
    } catch (const std::exception &e) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("could not read CSV row", K(ret), "reason", e.what());
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (MY_SPEC.file_sql_column_count_ > 0) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_csv_row;
    ret = open_csv();
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (MY_SPEC.file_sql_column_count_ > 0) {
    ret = open_csv();
  } else {
    node_idx_ = 0;
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  csv_input_.reset();
  file_sql::CsvRow().swap(csv_row_);
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  return ret;
}

void ObFunctionTableOp::destroy()
{
  csv_input_.reset();
  file_sql::CsvRow().swap(csv_row_);
  ObOperator::destroy();
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
