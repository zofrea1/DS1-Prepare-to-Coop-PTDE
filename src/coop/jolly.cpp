// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 zofrea1. Prepare to Co-op Edition; see LICENSE and NOTICE.md.
#include "jolly.h"

#include "game.h"
#include "hook.h"
#include "log.h"
#include "text.h"
#include "world.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace coop::jolly {

namespace {

// The gesture a character is making: its controller's (+0x28) +0x370 holds the gesture's animation while the gesture
// plays (-1 otherwise). The gestures are animations 6800-6830 of c0000 (found 2026-11-12 by making each one through
// the gesture menu and reading the controller): Beckon 6800, Point up 6802, Point down 6803, Point forward 6805, Bow
// 6810, Wave 6815, Joy 6825, Praise the Sun 6801 (made from the menu after putting it in the equipped list:
// PlayerGameData+0x334 -> GestureEquipData, 8 slots at +8, (id << 1) | display << 16; Praise the Sun is id 15).
constexpr uint32_t kCtrl = 0x28;
constexpr uint32_t kCtrlGesture = 0x370;
constexpr int kPraiseTheSun = 6801;
constexpr int kBeckon = 6800;  // JollyTest
// Replenishment's own effect (miracle 5040 -> SpEffect 570): 10 HP a second for 60 s, with its icon.
constexpr int kWarmth = 570;
constexpr double kWindow = 3.0;     // s between the two players' starts
constexpr double kReach = 10.0;     // m between the two characters
constexpr double kCooldown = 180.0; // s between two rewards for one player

bool g_enabled = true;
bool g_test = false;

struct Heard {
    uint64_t who;
    uint32_t chr;
    std::wstring name;
    double at;
};
std::vector<Heard> g_heard;  // teammates' starts of the last few seconds
int g_prev = -1;             // our gesture last frame
double g_mine = -1;          // when our player began Praise the Sun (-1 none waiting)
double g_rewarded = -1e9;    // our last reward

double now() { return GetTickCount64() / 1000.0; }

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), ' ');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

int gesture(uint32_t chr) {
    uint32_t ctrl = 0, g = 0xFFFFFFFF;
    if (!chr || !read32(chr + kCtrl, &ctrl) || !ctrl || !read32(ctrl + kCtrlGesture, &g)) return -1;
    return static_cast<int>(g);
}

bool still_here(uint32_t chr) {
    if (chr == game::player()) return true;  // JollyTest
    for (uint32_t live : game::remote_player_chrs()) {
        if (live == chr) return true;
    }
    return false;
}

// Our start and a teammate's within kWindow, their character within kReach: our player is rewarded (their game does
// the same for theirs).
void try_reward(uint32_t me, double t) {
    game::Vec3 mine{};
    float angle = 0;
    if (!game::position(&mine, &angle)) return;
    for (auto it = g_heard.begin(); it != g_heard.end(); ++it) {
        if (std::fabs(it->at - g_mine) > kWindow || !it->chr || !still_here(it->chr)) continue;
        game::Vec3 theirs{};
        if (!game::chr_position(it->chr, &theirs)) continue;
        const double dx = theirs.x - mine.x, dy = theirs.y - mine.y, dz = theirs.z - mine.z;
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > kReach) {
            LOG_INFO("[coop] Jolly: %s praised the Sun %.1f s apart from us, but %.1f m away", utf8(it->name).c_str(),
                     it->at - g_mine, dist);
            continue;
        }
        const std::wstring with = it->name.empty() ? L"Jolly cooperation!" : L"Jolly cooperation with " + it->name + L"!";
        if (t - g_rewarded < kCooldown) {
            const int left = static_cast<int>(kCooldown - (t - g_rewarded) + 0.5);
            text::show(with + L" (the Sun's warmth again in " + std::to_wstring(left) + L" s)");
            LOG_INFO("[coop] Jolly cooperation with %s (%.1f m, %.1f s apart): no warmth for %d s more",
                     utf8(it->name).c_str(), dist, it->at - g_mine, left);
        } else {
            const bool ok = world::apply_own_effect(me, kWarmth);
            if (ok) g_rewarded = t;
            text::show(with);
            LOG_INFO("[coop] Jolly cooperation with %s (%.1f m, %.1f s apart): Replenishment's warmth (SpEffect %d) %s",
                     utf8(it->name).c_str(), dist, it->at - g_mine, kWarmth, ok ? "given" : "refused by the game");
        }
        g_mine = -1;
        g_heard.erase(it);
        return;
    }
}

}  // namespace

void set_enabled(bool on) { g_enabled = on; }
void set_test(bool on) { g_test = on; }

bool tick() {
    if (!g_enabled) return false;
    const uint32_t me = game::player();
    if (!me || game::loading()) {
        g_prev = -1;
        g_mine = -1;
        return false;
    }
    const int g = gesture(me);
    const int target = g_test ? kBeckon : kPraiseTheSun;
    const bool began = g == target && g_prev != target;
    g_prev = g;
    const double t = now();
    if (began) {
        g_mine = t;
        LOG_INFO("[coop] Jolly: we began %s", g_test ? "Beckon (JollyTest: counts as Praise the Sun)" : "Praise the Sun");
        if (g_test) remote_praise(0, me, L"yourself");
    }
    g_heard.erase(std::remove_if(g_heard.begin(), g_heard.end(), [&](const Heard& h) { return t - h.at > 2 * kWindow; }),
                  g_heard.end());
    if (g_mine >= 0) {
        if (t - g_mine > kWindow) {
            g_mine = -1;
        } else {
            try_reward(me, t);
        }
    }
    return began && !g_test;
}

void remote_praise(uint64_t who, uint32_t chr, const std::wstring& name) {
    if (!g_enabled) return;
    for (auto& h : g_heard) {
        if (h.who == who) {
            h = Heard{who, chr, name, now()};
            return;
        }
    }
    g_heard.push_back(Heard{who, chr, name, now()});
    if (who) LOG_INFO("[coop] Jolly: %s began Praise the Sun (character %08X here)", utf8(name).c_str(), chr);
}

}  // namespace coop::jolly
