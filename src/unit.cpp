/* Copyright (c) 2025 M.A.X. Port Team
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

#include "unit.hpp"

#include <SDL3/SDL.h>

#include <cstring>

#include "commandline.hpp"
#include "resource_manager.hpp"

/* Stand-in animation tables for hidden games, which do not load MAX.RES (see CommandLine_IsHidden()).
 *
 * The D_* resources of MAX.RES hold the sprite frame layout of each unit: base frame and frame count of the body,
 * turret, firing and connector sprites plus the turret mount offsets. Nothing is drawn in a hidden game, but a few
 * frame counts pace game logic: an explosion or smoke puff lives until its frame index reaches the last frame (the
 * destroyed unit is cleared at the half way frame), a bridge raises and lowers frame by frame and spinning turrets
 * and animated buildings cycle through their frames. A zero frame count would keep an explosion alive forever.
 *
 * The layouts follow the frame offsets the engine hardcodes elsewhere: 8 directions per sprite set, turret frames
 * after the body frames and firing frames after those. The frame counts are plausible values, not the original
 * data. The turret mount offsets only position sprites on screen and stay zero.
 */
struct Unit_HiddenFrameInfo {
    ResourceID data;
    int16_t image_base;
    int16_t image_count;
    int16_t turret_image_base;
    int16_t turret_image_count;
    int16_t firing_image_base;
    int16_t firing_image_count;
};

static constexpr Unit_HiddenFrameInfo Unit_HiddenFrameInfoTable[] = {
    // buildings: idle and working frame
    {D_LRGBLD, 0, 2, 0, 0, 0, 0},
    {D_SMLBLD, 0, 2, 0, 0, 0, 0},
    {D_MINING, 0, 2, 0, 0, 0, 0},
    {D_RADAR, 0, 8, 0, 0, 0, 0},
    // ground cover and rubble
    {D_DEFALT, 0, 1, 0, 0, 0, 0},
    {D_LRGSLA, 0, 1, 0, 0, 0, 0},
    {D_LRGRBL, 0, 1, 0, 0, 0, 0},
    {D_SMLRBL, 0, 1, 0, 0, 0, 0},
    {D_BRIDGE, 0, 3, 0, 0, 0, 0},
    // gun emplacements: base, turret directions, turret firing directions
    {D_FIXED, 0, 1, 8, 8, 16, 8},
    {D_ANTIAI, 0, 1, 8, 8, 16, 8},
    // mobile units: 8 directions, amphibious units keep their water frames at 8
    {D_MOBILE, 0, 8, 0, 0, 0, 0},
    {D_AMPHIB, 0, 8, 0, 0, 16, 8},
    {D_SUB, 0, 8, 0, 0, 0, 0},
    {D_FIRING, 0, 8, 0, 0, 8, 8},
    {D_COMMAN, 0, 8, 0, 0, 104, 8},
    {D_INFANT, 0, 8, 0, 0, 104, 8},
    {D_TANK, 0, 8, 8, 8, 16, 8},
    {D_SP_FLK, 0, 8, 8, 8, 16, 8},
    {D_ESCORT, 0, 8, 8, 8, 16, 8},
    {D_BATTLS, 0, 8, 8, 8, 16, 8},
    {D_ALTANK, 0, 8, 8, 8, 16, 8},
    {D_SCANNR, 0, 8, 8, 8, 0, 0},
    {D_AWAC, 0, 8, 8, 8, 0, 0},
    // projectiles
    {D_TORPDO, 0, 8, 0, 0, 0, 0},
    {D_ALNMSL, 0, 8, 0, 0, 0, 0},
    {D_ALNPBL, 0, 8, 0, 0, 0, 0},
    // effects, the frame count is their lifetime in animation steps
    {D_UEXPLD, 0, 16, 0, 0, 0, 0},
    {D_BEXPLD, 0, 16, 0, 0, 0, 0},
    {D_SEXPLD, 0, 8, 0, 0, 0, 0},
    {D_SMOKE, 0, 8, 0, 0, 0, 0},
    {D_BUBBLE, 0, 8, 0, 0, 0, 0},
};

static FrameInfo Unit_GetHiddenFrameInfo(ResourceID data) {
    FrameInfo file{};

    for (const auto& entry : Unit_HiddenFrameInfoTable) {
        if (entry.data == data) {
            file.image_base = entry.image_base;
            file.image_count = entry.image_count;
            file.turret_image_base = entry.turret_image_base;
            file.turret_image_count = entry.turret_image_count;
            file.firing_image_base = entry.firing_image_base;
            file.firing_image_count = entry.firing_image_count;

            return file;
        }
    }

    SDL_Log("Hidden mode: no stand-in animation table for %s.", ResourceManager_GetResourceID(data));

    // a single frame ends every frame sequence at once
    file.image_count = 1;

    return file;
}

Unit::Unit(uint32_t flags, ResourceID sprite, ResourceID shadow, ResourceID data, ResourceID flics_animation,
           ResourceID portrait, ResourceID icon, ResourceID armory_portrait, uint8_t land_type, CargoType cargo_type,
           LocalizedGender&& gender, uint32_t singular_name, uint32_t plural_name, uint32_t description,
           uint32_t tutorial_description, std::unordered_map<SfxType, SoundEffectInfo>&& sound_effects)
    : m_flags(flags),
      m_sprite(sprite),
      m_shadow(shadow),
      m_data(data),
      m_flics_animation(flics_animation),
      m_portrait(portrait),
      m_icon(icon),
      m_armory_portrait(armory_portrait),
      m_land_type(land_type),
      m_cargo_type(cargo_type),
      m_gender(std::move(gender)),
      m_singular_name(singular_name),
      m_plural_name(plural_name),
      m_description(description),
      m_tutorial_description(tutorial_description),
      m_sprite_data(nullptr),
      m_shadow_data(nullptr),
      m_frame_info([data, flags]() {
          FrameInfo file{};
          if (data != INVALID_ID) {
              uint8_t* data_buffer = ResourceManager_LoadResource(data);

              if (data_buffer) {
                  file.image_base = static_cast<int8_t>(data_buffer[0]);
                  file.image_count = static_cast<int8_t>(data_buffer[1]);
                  file.turret_image_base = static_cast<int8_t>(data_buffer[2]);
                  file.turret_image_count = static_cast<int8_t>(data_buffer[3]);
                  file.firing_image_base = static_cast<int8_t>(data_buffer[4]);
                  file.firing_image_count = static_cast<int8_t>(data_buffer[5]);
                  file.connector_image_base = static_cast<int8_t>(data_buffer[6]);
                  file.connector_image_count = static_cast<int8_t>(data_buffer[7]);

                  if (flags & (TURRET_SPRITE | SPINNING_TURRET)) {
                      for (int32_t i = 0; i < 8; ++i) {
                          file.angle_offsets[i].x = static_cast<int8_t>(data_buffer[8 + i * 2]);
                          file.angle_offsets[i].y = static_cast<int8_t>(data_buffer[8 + i * 2 + 1]);
                      }
                  }

              } else if (CommandLine_IsHidden()) {
                  file = Unit_GetHiddenFrameInfo(data);
              }
          }

          return file;
      }()),
      m_sound_effects(std::move(sound_effects)) {}

Unit::~Unit() {}

uint32_t Unit::GetFlags() const { return m_flags; }

void Unit::SetFlags(const uint32_t flags) {
    if (m_flags & EXPLODING) {
        m_flags &= ~(MISSILE_UNIT | MOBILE_AIR_UNIT | MOBILE_SEA_UNIT | MOBILE_LAND_UNIT | STATIONARY);
        m_flags |= flags & (MISSILE_UNIT | MOBILE_AIR_UNIT | MOBILE_SEA_UNIT | MOBILE_LAND_UNIT | STATIONARY);
    }
}

ResourceID Unit::GetSprite() const { return m_sprite; }

ResourceID Unit::GetShadow() const { return m_shadow; }

ResourceID Unit::GetData() const { return m_data; }

ResourceID Unit::GetFlicsAnimation() const { return m_flics_animation; }

ResourceID Unit::GetPortrait() const { return m_portrait; }

ResourceID Unit::GetIcon() const { return m_icon; }

ResourceID Unit::GetArmoryPortrait() const { return m_armory_portrait; }

uint8_t Unit::GetLandType() const { return m_land_type; }

Unit::CargoType Unit::GetCargoType() const { return m_cargo_type; }

Unit::Gender Unit::GetGender() const {
    auto it = m_gender.find(ResourceManager_GetSystemLocale());

    return (it != m_gender.end()) ? it->second : m_gender.at("en-US");
}

std::string_view Unit::GetSingularName() const { return ResourceManager_GetLanguageEntry(m_singular_name); }

std::string_view Unit::GetPluralName() const { return ResourceManager_GetLanguageEntry(m_plural_name); }

std::string_view Unit::GetDescription() const { return ResourceManager_GetLanguageEntry(m_description); }

std::string_view Unit::GetTutorialDescription() const {
    return ResourceManager_GetLanguageEntry(m_tutorial_description);
}

uint8_t* Unit::GetSpriteData() const { return m_sprite_data; }

uint8_t* Unit::GetShadowData() const { return m_shadow_data; }

const FrameInfo& Unit::GetFrameInfo() const { return m_frame_info; }

void Unit::SetSpriteData(uint8_t* sprite_data) { m_sprite_data = sprite_data; }

void Unit::SetShadowData(uint8_t* shadow_data) { m_shadow_data = shadow_data; }

const Unit::SoundEffectInfo& Unit::GetSoundEffect(SfxType sfx_type) const {
    auto it = m_sound_effects.find(sfx_type);
    return it->second;
}
