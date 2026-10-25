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

#ifndef BUILDMENUHOTKEYS_HPP
#define BUILDMENUHOTKEYS_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Initials and name-prefix matching, independent of drawing and build orders. Names and typed letters are compared
// in any script, ignoring case and diacritics. A word is a run of letters, so "Anti-Aircraft" and "D.C.A." have the
// initials "aa" and "dca".
class BuildMenuHotkeys {
    struct Item {
        std::string initials;
        std::string name;
    };

    std::vector<Item> items;
    std::string pending;
    int32_t sequence_selected{-1};
    uint64_t last_letter_time{0};

public:
    // Letters typed further apart than this many milliseconds start a new sequence.
    static constexpr uint64_t SEQUENCE_TIMEOUT{1000};

    static std::string Initials(std::string_view name);
    static std::vector<std::string> Letters(std::string_view text);

    void Add(std::string_view name);
    void Reset();

    // Appends a typed letter to the sequence. Returns the index of the item whose initials, or unique name prefix, the
    // sequence spells, otherwise -1. Repeating an identical shortcut cycles through items sharing those initials.
    int32_t Match(std::string_view letter, int32_t selected, uint64_t time);
};

#endif /* BUILDMENUHOTKEYS_HPP */
