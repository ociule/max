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

#include <gtest/gtest.h>

#include <initializer_list>

namespace {

constexpr uint64_t QUICK{100};
constexpr uint64_t PAUSE{BuildMenuHotkeys::SEQUENCE_TIMEOUT + 1};

struct Keyboard {
    BuildMenuHotkeys keys;
    uint64_t time{0};

    Keyboard(std::initializer_list<const char*> names) {
        for (const char* name : names) {
            keys.Add(name);
        }
    }

    int32_t Press(std::string_view letter, int32_t selected, uint64_t delay = QUICK) {
        time += delay;

        return keys.Match(letter, selected, time);
    }
};

const std::initializer_list<const char*> ENGINEER_ITEMS = {
    "Storage Unit", "Fuel Tank",  "Gold Vault",     "Power Generator", "Connector",
    "Radar",        "Gun Turret", "Anti Aircraft",  "Artillery",       "Missile Launcher",
    "Landing Pad",  "Bridge",     "Water Platform", "Concrete Block",  "Road"};

const std::initializer_list<const char*> CONSTRUCTOR_ITEMS = {"Mining Station",
                                                              "Power Station",
                                                              "Light Vehicle Plant",
                                                              "Heavy Vehicle Plant",
                                                              "Air Units Plant",
                                                              "Shipyard",
                                                              "Gold Refinery",
                                                              "Depot",
                                                              "Hangar",
                                                              "Dock",
                                                              "Habitat",
                                                              "Research Center",
                                                              "Eco-sphere",
                                                              "Training Hall",
                                                              "Barracks"};

}  // namespace

TEST(BuildMenuHotkeysTest, Initials) {
    EXPECT_EQ(BuildMenuHotkeys::Initials("Connector"), "c");
    EXPECT_EQ(BuildMenuHotkeys::Initials("Power Generator"), "pg");
    EXPECT_EQ(BuildMenuHotkeys::Initials(" Anti-Aircraft "), "aa");
    EXPECT_EQ(BuildMenuHotkeys::Initials("Eco-Sphere"), "es");
    EXPECT_EQ(BuildMenuHotkeys::Initials("Water  Platform"), "wp");
    EXPECT_EQ(BuildMenuHotkeys::Initials("D.C.A."), "dca");
}

TEST(BuildMenuHotkeysTest, LocalizedInitialsIgnoreCaseAndDiacritics) {
    EXPECT_EQ(BuildMenuHotkeys::Initials("Öko-Sphäre"), "os");
    EXPECT_EQ(BuildMenuHotkeys::Initials("Générateur Energie"), "ge");
    EXPECT_EQ(BuildMenuHotkeys::Initials("Fábrica de vehículos ligeros"), "fdvl");
    EXPECT_EQ(BuildMenuHotkeys::Initials("Generatore d'energia"), "gde");
    EXPECT_EQ(BuildMenuHotkeys::Initials("Генератор Энерг"), "гэ");
    EXPECT_EQ(BuildMenuHotkeys::Initials("Завод Легких Машин"), "злм");
}

TEST(BuildMenuHotkeysTest, LocalizedLettersMatch) {
    Keyboard german({"Öko-Sphäre", "Straße", "Koppelung"});

    EXPECT_EQ(german.Press("ö", 2), -1);
    EXPECT_EQ(german.Press("S", 2), 0);
    EXPECT_EQ(german.Press("o", 0, PAUSE), -1);
    EXPECT_EQ(german.Press("s", 0), 0);
    EXPECT_EQ(german.Press("k", 0, PAUSE), 2);
    EXPECT_EQ(german.Press("s", 2, PAUSE), 1);
    EXPECT_EQ(german.Press("t", 1), 1);
    EXPECT_EQ(german.Press("r", 1), 1);
    EXPECT_EQ(german.Press("a", 1), 1);
    EXPECT_EQ(german.Press("ß", 1), 1);
    EXPECT_EQ(german.Press("e", 1), 1);
    EXPECT_EQ(german.Press("k", 1), 2);

    Keyboard russian({"Генератор Энерг", "Дор.", "Трубопров"});

    EXPECT_EQ(russian.Press("Г", 2), -1);
    EXPECT_EQ(russian.Press("э", 2), 0);
    EXPECT_EQ(russian.Press("д", 0, PAUSE), 1);
    EXPECT_EQ(russian.Press("Т", 1, PAUSE), 2);
    EXPECT_EQ(russian.Press("t", 2, PAUSE), -1);
}

TEST(BuildMenuHotkeysTest, LettersSplitsTextInput) {
    EXPECT_EQ(BuildMenuHotkeys::Letters("г"), std::vector<std::string>({"г"}));
    EXPECT_EQ(BuildMenuHotkeys::Letters("аb1 é"), std::vector<std::string>({"а", "b", "é"}));
    EXPECT_TRUE(BuildMenuHotkeys::Letters("1?").empty());
}

TEST(BuildMenuHotkeysTest, SelectOnlyAfterCompleteSequence) {
    Keyboard keyboard({"Connector", "Power Generator", "Power Station"});

    EXPECT_EQ(keyboard.Press("C", 1), 0);
    EXPECT_EQ(keyboard.Press("p", 0), -1);
    EXPECT_EQ(keyboard.Press("G", 0), 1);
    EXPECT_EQ(keyboard.Press("P", 1), -1);
    EXPECT_EQ(keyboard.Press("s", 1), 2);
}

TEST(BuildMenuHotkeysTest, NonLettersAreIgnored) {
    Keyboard keyboard({"Connector", "Power Generator"});

    EXPECT_EQ(keyboard.Press("1", 0), -1);
    EXPECT_EQ(keyboard.Press("", 0), -1);
    EXPECT_EQ(keyboard.Press("c", 0), 0);
}

TEST(BuildMenuHotkeysTest, NewShortcutRecoversFromIncompleteOrInvalidSequence) {
    Keyboard keyboard({"Connector", "Power Generator"});

    EXPECT_EQ(keyboard.Press("p", 0), -1);
    EXPECT_EQ(keyboard.Press("c", 1), 0);
    EXPECT_EQ(keyboard.Press("z", 0), -1);
    EXPECT_EQ(keyboard.Press("g", 0), -1);
    EXPECT_EQ(keyboard.Press("p", 0), -1);
    EXPECT_EQ(keyboard.Press("g", 0), 1);
}

TEST(BuildMenuHotkeysTest, ResetAbandonsPendingSequence) {
    Keyboard keyboard({"Power Generator"});

    EXPECT_EQ(keyboard.Press("p", 0), -1);
    keyboard.keys.Reset();
    EXPECT_EQ(keyboard.Press("g", 0), -1);
    EXPECT_EQ(keyboard.Press("p", 0), -1);
    EXPECT_EQ(keyboard.Press("g", 0), 0);
}

TEST(BuildMenuHotkeysTest, PauseStartsNewSequence) {
    Keyboard keyboard({"Power Generator"});

    EXPECT_EQ(keyboard.Press("p", 0), -1);
    EXPECT_EQ(keyboard.Press("g", 0, PAUSE), -1);
    EXPECT_EQ(keyboard.Press("p", 0, PAUSE), -1);
    EXPECT_EQ(keyboard.Press("g", 0, BuildMenuHotkeys::SEQUENCE_TIMEOUT), 0);
}

TEST(BuildMenuHotkeysTest, ShortcutAfterPauseIsNotAppendedToPreviousOne) {
    Keyboard engineer(ENGINEER_ITEMS);

    EXPECT_EQ(engineer.Press("r", 0), 5);
    EXPECT_EQ(engineer.Press("a", 5, PAUSE), 8);
    EXPECT_EQ(engineer.Press("b", 8, PAUSE), 11);
    EXPECT_EQ(engineer.Press("r", 11, PAUSE), 14);
    EXPECT_EQ(engineer.Press("c", 14, PAUSE), 4);
    EXPECT_EQ(engineer.Press("b", 4, PAUSE), 11);

    Keyboard constructor(CONSTRUCTOR_ITEMS);

    EXPECT_EQ(constructor.Press("s", 0), 5);
    EXPECT_EQ(constructor.Press("h", 5, PAUSE), 8);
    EXPECT_EQ(constructor.Press("a", 8, PAUSE), -1);
    EXPECT_EQ(constructor.Press("u", 8), -1);
    EXPECT_EQ(constructor.Press("p", 8), 4);
}

TEST(BuildMenuHotkeysTest, QuickSequencesStillCombine) {
    Keyboard engineer(ENGINEER_ITEMS);

    EXPECT_EQ(engineer.Press("c", 0), 4);
    EXPECT_EQ(engineer.Press("b", 4), 13);
    EXPECT_EQ(engineer.Press("r", 0, PAUSE), 5);
    EXPECT_EQ(engineer.Press("a", 5), 5);
    EXPECT_EQ(engineer.Press("r", 5, PAUSE), 14);
    EXPECT_EQ(engineer.Press("r", 14), 5);
}

TEST(BuildMenuHotkeysTest, SharedInitialsCycleAndShortNamesRemainReachable) {
    Keyboard keyboard({"Radar", "Repair Depot", "Research Depot"});

    EXPECT_EQ(keyboard.Press("r", 0), 0);
    EXPECT_EQ(keyboard.Press("d", 0), 1);
    EXPECT_EQ(keyboard.Press("r", 1), 0);
    EXPECT_EQ(keyboard.Press("d", 0), 2);
    EXPECT_EQ(keyboard.Press("r", 2), 0);
    EXPECT_EQ(keyboard.Press("d", 0), 1);
}

TEST(BuildMenuHotkeysTest, RadarAndRoadShareR) {
    Keyboard keyboard({"Connector", "Radar", "Road"});

    EXPECT_EQ(keyboard.Press("r", 0), 1);
    EXPECT_EQ(keyboard.Press("r", 1), 2);
    EXPECT_EQ(keyboard.Press("r", 2), 1);

    EXPECT_EQ(keyboard.Press("a", 1), 1);
    EXPECT_EQ(keyboard.Press("d", 1), 1);
    keyboard.keys.Reset();
    EXPECT_EQ(keyboard.Press("R", 1), 2);
    EXPECT_EQ(keyboard.Press("O", 2), 2);
    EXPECT_EQ(keyboard.Press("a", 2), 2);
    EXPECT_EQ(keyboard.Press("d", 2), 2);
}

TEST(BuildMenuHotkeysTest, LongerNamePrefixesDisambiguateAndInitialsTakePriority) {
    Keyboard keyboard({"Hangar", "Habitat", "Heavy Vehicle Plant", "Gold Refinery", "Greenhouse"});

    EXPECT_EQ(keyboard.Press("h", 2), 0);
    EXPECT_EQ(keyboard.Press("a", 0), -1);
    EXPECT_EQ(keyboard.Press("b", 0), 1);
    EXPECT_EQ(keyboard.Press("h", 1), 0);
    EXPECT_EQ(keyboard.Press("a", 0), -1);
    EXPECT_EQ(keyboard.Press("n", 0), 0);
    EXPECT_EQ(keyboard.Press("h", 0), 1);
    EXPECT_EQ(keyboard.Press("v", 1), -1);
    EXPECT_EQ(keyboard.Press("p", 1), 2);
    EXPECT_EQ(keyboard.Press("g", 2), 4);
    EXPECT_EQ(keyboard.Press("r", 4), 3);
}

TEST(BuildMenuHotkeysTest, EmptyMenu) {
    Keyboard keyboard({});

    EXPECT_EQ(keyboard.Press("c", -1), -1);
}
