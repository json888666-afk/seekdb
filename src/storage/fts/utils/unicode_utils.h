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

#ifndef _OCEANBASE_STORAGE_FTS_UTILS_UNICODE_UTILS_H_
#define _OCEANBASE_STORAGE_FTS_UTILS_UNICODE_UTILS_H_

#include "lib/charset/ob_ctype.h"

namespace oceanbase
{
namespace storage
{
class ObUnicodeBlockUtils final
{
public:
  static bool is_unicode_cn_number(const ob_wc_t unicode);

  static bool is_alpha(const ob_wc_t unicode)
  {
    bool is_ascii_alpha
        = (unicode >= 0x41 && unicode <= 0x5a) || (unicode >= 0x61 && unicode <= 0x7a);
    bool is_fw_alpha
        = (unicode >= 0xFF21 && unicode <= 0xFF3A) || (unicode >= 0xFF41 && unicode <= 0xFF5A);
    return is_ascii_alpha || is_fw_alpha;
  }

  static bool is_arabic(const ob_wc_t unicode)
  {
    bool is_ascii_arabic = (unicode >= 0x30 && unicode <= 0x39);
    bool is_fw_arabic = (unicode >= 0xFF10 && unicode <= 0xFF19);
    return is_ascii_arabic || is_fw_arabic;
  }

  static bool check_high_surrogate(const ob_wc_t unicode);

  static bool check_high_private_use_surrogate(const ob_wc_t unicode);

  static bool check_low_surrogate(const ob_wc_t unicode);

  static bool check_letter_connector(const ob_wc_t unicode);

  static bool check_num_connector(const ob_wc_t unicode);

  static bool check_ignore_as_single(const ob_wc_t unicode);

  static bool is_chinese(const ob_wc_t unicode)
  {
    // variables name keep same with it's code block name, skip code style.
    bool CJK_UNIFIED_IDEOGRAPHS = (unicode >= 0x4E00 && unicode <= 0x9FFF);
    bool CJK_COMPATIBILITY_IDEOGRAPHS = (unicode >= 0xF900 && unicode <= 0xFAFF);
    bool CJK_UNIFIED_IDEOGRAPHS_EXTENSION_A = (unicode >= 0x3400 && unicode <= 0x4DBF);
    return CJK_UNIFIED_IDEOGRAPHS || CJK_COMPATIBILITY_IDEOGRAPHS
           || CJK_UNIFIED_IDEOGRAPHS_EXTENSION_A;
  }

  static bool is_other_cjk(const ob_wc_t unicode)
  {
    // variables name keep same with it's code block name, skip code style.
    bool HALFWIDTH_AND_FULLWIDTH_FORMS = (unicode >= 0xFF00 && unicode <= 0xFFEF);

    bool HANGUL_SYLLABLES = (unicode >= 0xAC00 && unicode <= 0xD7AF);
    bool HANGUL_JAMO = (unicode >= 0x1100 && unicode <= 0x11FF);
    bool HANGUL_COMPATIBILITY_JAMO = (unicode >= 0x3130 && unicode <= 0x318f);

    bool HIRAGANA = (unicode >= 0x3040 && unicode <= 0x309F);
    bool KATAKANA = (unicode >= 0x30A0 && unicode <= 0x30FF);
    bool KATAKANA_PHONETIC_EXTENSIONS = (unicode >= 0x31F0 && unicode <= 0x31FF);

    return HALFWIDTH_AND_FULLWIDTH_FORMS || HANGUL_SYLLABLES || HANGUL_JAMO
           || HANGUL_COMPATIBILITY_JAMO || HIRAGANA || KATAKANA || KATAKANA_PHONETIC_EXTENSIONS;
  }

  static bool is_surrogate(const ob_wc_t unicode);

  static bool is_convertable_fullwidth(const ob_wc_t unicode);

private:
  // Do compare with u8 char
  // only for static number
  static ob_wc_t get_unicode_from_u8(const char *input, const uint8_t char_len);
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_UTILS_UNICODE_UTILS_H_
