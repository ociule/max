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

#include <string>
#include <string_view>
#include <vector>

// Initials and name-prefix matching, independent of drawing and build orders.
class BuildMenuHotkeys {
    struct Item {
        std::string initials;
        std::string name;
    };

    std::vector<Item> items;
    std::string pending;
    int sequence_selected{-1};

public:
    static std::string Initials(std::string_view name) {
        std::string result;
        bool word_start = true;

        for (char letter : name) {
            if (letter == ' ' || letter == '-' || letter == '\t') {
                word_start = true;
            } else {
                if (word_start && ((letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z'))) {
                    result += letter >= 'A' && letter <= 'Z' ? letter + ('a' - 'A') : letter;
                }
                word_start = false;
            }
        }

        return result;
    }

    void Add(std::string_view name) {
        std::string letters;
        for (char letter : name) {
            if (letter >= 'A' && letter <= 'Z') letter += 'a' - 'A';
            if (letter >= 'a' && letter <= 'z') letters += letter;
        }
        items.push_back({Initials(name), letters});
    }
    void Reset() { pending.clear(); }

    // Returns completed initials or a unique name prefix, otherwise -1.
    // Repeating an identical shortcut cycles through items sharing those initials.
    int Match(int key, int selected) {
        if (key >= 'A' && key <= 'Z') {
            key += 'a' - 'A';
        }
        if (key < 'a' || key > 'z') {
            Reset();
            return -1;
        }

        if (pending.empty()) sequence_selected = selected;
        pending += static_cast<char>(key);
        for (int attempt = 0; attempt < 2; ++attempt) {
            bool prefix = false;
            bool longer = false;
            int first = -1;
            int next = -1;
            int name_match = -1;
            int name_count = 0;

            for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                if (items[i].initials.starts_with(pending)) {
                    prefix = true;
                    if (items[i].initials.size() > pending.size()) {
                        longer = true;
                    } else {
                        if (first < 0) first = i;
                        if (next < 0 && i > sequence_selected) next = i;
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
                int match = next >= 0 ? next : first;
                if (match < 0 && pending.size() > 1 && name_count == 1) match = name_match;
                if (!longer) Reset();
                return match;
            }

            // A mistyped or abandoned sequence must not swallow a fresh shortcut.
            pending.assign(1, static_cast<char>(key));
            sequence_selected = selected;
        }

        Reset();
        return -1;
    }
};

#endif /* BUILDMENUHOTKEYS_HPP */
