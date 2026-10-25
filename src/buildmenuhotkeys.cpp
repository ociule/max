/* Copyright (c) 2026 M.A.X. Port Team
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "buildmenuhotkeys.hpp"

#include <utf8proc.h>

#include "utf8.hpp"

static bool BuildMenuHotkeys_IsLetter(utf8proc_int32_t codepoint);
static std::vector<std::string> BuildMenuHotkeys_Split(std::string_view text);
static std::vector<std::string> BuildMenuHotkeys_Words(std::string_view name);

bool BuildMenuHotkeys_IsLetter(utf8proc_int32_t codepoint) {
    const utf8proc_category_t category = utf8proc_category(codepoint);

    return category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_LO;
}

// Splits text into its letters. An empty string marks each break between runs of letters.
std::vector<std::string> BuildMenuHotkeys_Split(std::string_view text) {
    const auto data = reinterpret_cast<const utf8proc_uint8_t*>(text.data());
    const utf8proc_ssize_t length = text.size();
    std::vector<std::string> result;

    for (utf8proc_ssize_t i = 0, bytes_read; i < length; i += bytes_read) {
        utf8proc_int32_t codepoint;

        bytes_read = utf8proc_iterate(data + i, length - i, &codepoint);

        if (bytes_read < 0) {
            break;
        }

        if (BuildMenuHotkeys_IsLetter(codepoint)) {
            result.emplace_back(text.substr(i, bytes_read));

        } else if (!result.empty() && !result.back().empty()) {
            result.emplace_back();
        }
    }

    return result;
}

std::vector<std::string> BuildMenuHotkeys_Words(std::string_view name) {
    std::vector<std::string> words;
    bool word_start = true;

    for (const auto& letter : BuildMenuHotkeys_Split(utf8_fold_str(std::string(name)))) {
        if (letter.empty()) {
            word_start = true;

        } else {
            if (word_start) {
                words.emplace_back();
                word_start = false;
            }

            words.back() += letter;
        }
    }

    return words;
}

std::string BuildMenuHotkeys::Initials(std::string_view name) {
    std::string result;

    for (const auto& word : BuildMenuHotkeys_Words(name)) {
        result += BuildMenuHotkeys_Split(word).front();
    }

    return result;
}

std::vector<std::string> BuildMenuHotkeys::Letters(std::string_view text) {
    std::vector<std::string> result;

    for (auto& letter : BuildMenuHotkeys_Split(text)) {
        if (!letter.empty()) {
            result.push_back(std::move(letter));
        }
    }

    return result;
}

void BuildMenuHotkeys::Add(std::string_view name) {
    std::string letters;

    for (const auto& word : BuildMenuHotkeys_Words(name)) {
        letters += word;
    }

    items.push_back({Initials(name), letters});
}

void BuildMenuHotkeys::Reset() { pending.clear(); }

int32_t BuildMenuHotkeys::Match(std::string_view letter, int32_t selected, uint64_t time) {
    const std::vector<std::string> words = BuildMenuHotkeys_Words(letter);

    if (words.size() != 1) {
        Reset();

        return -1;
    }

    const std::string& key = words.front();

    if (!pending.empty() && time - last_letter_time > SEQUENCE_TIMEOUT) {
        Reset();
    }

    last_letter_time = time;

    if (pending.empty()) {
        sequence_selected = selected;
    }

    pending += key;

    for (int32_t attempt = 0; attempt < 2; ++attempt) {
        bool prefix = false;
        bool longer = false;
        int32_t first = -1;
        int32_t next = -1;
        int32_t name_match = -1;
        int32_t name_count = 0;

        for (int32_t i = 0; i < static_cast<int32_t>(items.size()); ++i) {
            if (items[i].initials.starts_with(pending)) {
                prefix = true;

                if (items[i].initials.size() > pending.size()) {
                    longer = true;

                } else {
                    if (first < 0) {
                        first = i;
                    }

                    if (next < 0 && i > sequence_selected) {
                        next = i;
                    }
                }
            }

            if (items[i].name.starts_with(pending)) {
                prefix = true;
                longer = longer || items[i].name.size() > pending.size();
                name_match = i;
                ++name_count;
            }
        }

        if (prefix) {
            // Explicit initials win over a prefix of another item's name.
            int32_t match = next >= 0 ? next : first;

            if (match < 0 && pending.size() > key.size() && name_count == 1) {
                match = name_match;
            }

            if (!longer) {
                Reset();
            }

            return match;
        }

        // A mistyped or abandoned sequence must not swallow a fresh shortcut.
        pending = key;
        sequence_selected = selected;
    }

    Reset();

    return -1;
}
