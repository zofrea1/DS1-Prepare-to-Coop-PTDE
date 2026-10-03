#include "game.h"

#include "hook.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

namespace coop::game {

namespace {

// Session (FrpgSessionManagerImp)
constexpr uint32_t kCreateSession = 0x00D2D4F0;   // thiscall(sess, int map, float timeout) -> bool, ret 8
constexpr uint32_t kJoinSession = 0x00D2D320;     // thiscall(sess, NRSessionSearchResult*), ret 4
constexpr uint32_t kLeaveSession = 0x00D2B460;    // esi = sess
// Our own session as an NRSessionSearchResult (vanilla host code at 0xDB44EE).
constexpr uint32_t kRefreshSelfResult = 0x006FE430;  // thiscall(sess+4, sess+0x28, 1), ret 8
constexpr uint32_t kResultCopy = 0x006F9D90;         // thiscall(result, src), ret 4
constexpr uint32_t kResultValid = 0x006F8D80;        // thiscall(result) -> bool
constexpr uint32_t kResultSerialize = 0x006F8EE0;    // thiscall(result, ostream), ret 4
constexpr uint32_t kResultInit = 0x006F95D0;         // thiscall(result)
constexpr uint32_t kResultDeserialize = 0x006F9770;  // thiscall(result, istream) -> bool, ret 4
constexpr uint32_t kResultDtor = 0x006F9290;         // thiscall(result)
constexpr uint32_t kOutStreamCtor = 0x00592650;      // DLMemoryOutputStream(this, buf, cap), ret 8
constexpr uint32_t kOutStreamWrite = 0x00592910;     // thiscall(this, src, n), ret 8
constexpr uint32_t kOutStreamSize = 0x00592780;      // thiscall(this) -> bytes written
constexpr uint32_t kOutStreamDtor = 0x00467560;
constexpr uint32_t kInStreamCtor = 0x00570C80;       // DLMemoryInputStream(this, data, size), ret 8
constexpr uint32_t kInStreamRead = 0x00570E40;       // thiscall(this, dst, n), ret 8
constexpr uint32_t kWarpKick = 0x00D62930;           // WarpNextStageKick (Lua native), no args
constexpr uint32_t kSetFlagCore = 0x00D38CB0;        // eax = EventFlagMan, cl = 0, push value, push id; ret 8
constexpr uint32_t kGetFlagCore = 0x00D38C40;        // eax = EventFlagMan, cl = 0, push id -> bool; ret 4

uint32_t deref(uint32_t var) {
    uint32_t v = 0;
    return read32(var, &v) ? v : 0;
}

template <typename T>
T field(uint32_t base, uint32_t off, T fallback) {
    if (!base) return fallback;
    T v;
    __try {
        std::memcpy(&v, reinterpret_cast<const void*>(static_cast<uintptr_t>(base + off)), sizeof(T));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
    return v;
}

template <typename T>
void put(uint32_t base, uint32_t off, T v) {
    if (!base) return;
    std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(base + off)), &v, sizeof(T));
}

}  // namespace

uint32_t game_man() { return deref(kGameManVar); }
uint32_t game_data_man() { return deref(kGameDataManVar); }
uint32_t sess_man() { return deref(kSessManVar); }
uint32_t event_flag_man() { return deref(kEventFlagManVar); }

uint32_t player() {
    const uint32_t wcm = deref(kWorldChrManVar);
    return wcm ? field<uint32_t>(wcm, 0x3C, 0) : 0;
}

uint32_t player_game_data() {
    const uint32_t gdm = game_data_man();
    return gdm ? field<uint32_t>(gdm, 0x8, 0) : 0;
}

int session_state() { return field<int>(sess_man(), kSessState, 0); }
int join_step() { return field<int>(sess_man(), kSessJoinStep, 0); }
void set_join_step(int step) { put<int>(sess_man(), kSessJoinStep, step); }

int member_count() {
    const uint32_t s = sess_man();
    if (!s) return 0;
    const uint32_t b = field<uint32_t>(s, kSessMembersBegin, 0), e = field<uint32_t>(s, kSessMembersEnd, 0);
    return e >= b ? static_cast<int>((e - b) / 0x24) : 0;
}

bool own_world() { return field<uint8_t>(game_man(), kGmOwnWorld, 1) != 0; }
int chr_type() { return field<int>(player(), 0x70, -1); }
int map_id() { return field<int>(player(), 0x444, -1); }

bool position(Vec3* pos, float* angle) {
    const uint32_t p = player();
    if (!p) return false;
    const uint32_t ctrl = field<uint32_t>(p, 0x28, 0);
    const uint32_t phys = field<uint32_t>(ctrl, 0x1C, 0);
    if (!phys) return false;
    *pos = field<Vec3>(phys, 0x10, Vec3{0, 0, 0});
    if (angle) *angle = field<float>(phys, 0x4, 0.0f);
    return true;
}

int last_bonfire() { return field<int>(game_man(), kGmLastBonfire, -1); }
bool loading() { return player() == 0; }
bool in_game() { return player_game_data() != 0 && player() != 0; }

bool create_session(int map, float timeout) {
    const uint32_t s = sess_man();
    if (!s) return false;
    uint32_t t = 0;
    std::memcpy(&t, &timeout, 4);
    uint8_t ok = 0;
    __asm {
        push t
        push map
        mov ecx, s
        mov eax, kCreateSession
        call eax
        mov ok, al
    }
    return ok != 0;
}

bool build_invitation(int join_type, int flag, std::vector<uint8_t>* out) {
    const uint32_t s = sess_man();
    if (!s || session_state() != 3) return false;
    uint8_t result[0x200] = {};
    uint8_t stream[0x80] = {};
    uint8_t buf[0x200] = {};
    uint8_t valid = 0;
    uint32_t written = 0;
    void* r = result;
    void* strm = stream;
    void* b = buf;
    int jt = join_type, fl = flag;
    int* pjt = &jt;
    int* pfl = &fl;
    __asm {
        mov eax, s
        lea edx, [eax + 0x28]
        push 1
        push edx
        lea ecx, [eax + 4]
        mov eax, kRefreshSelfResult
        call eax

        mov eax, s
        lea edx, [eax + 0x28]
        push edx
        mov ecx, r
        mov eax, kResultCopy
        call eax

        mov ecx, r
        mov eax, kResultValid
        call eax
        mov valid, al
    }
    if (valid) {
        __asm {
            push 0x200
            push b
            mov ecx, strm
            mov eax, kOutStreamCtor
            call eax

            push 4
            push pjt
            mov ecx, strm
            mov eax, kOutStreamWrite
            call eax

            push 4
            push pfl
            mov ecx, strm
            mov eax, kOutStreamWrite
            call eax

            push strm
            mov ecx, r
            mov eax, kResultSerialize
            call eax

            mov ecx, strm
            mov eax, kOutStreamSize
            call eax
            mov written, eax

            mov ecx, strm
            mov eax, kOutStreamDtor
            call eax
        }
    }
    __asm {
        mov ecx, r
        mov eax, kResultDtor
        call eax
    }
    if (!valid || written == 0 || written > sizeof(buf)) return false;
    out->assign(buf, buf + written);
    return true;
}

bool join_from_invitation(const uint8_t* data, uint32_t data_size, int map, const Vec3& pos, float angle) {
    const uint32_t s = sess_man();
    const uint32_t gm = game_man();
    if (!s || !gm || !data || data_size < 8) return false;
    const int st = session_state();
    if (st != 0 && st != 2 && st != 5) return false;  // the states the vanilla guest code accepts
    uint8_t result[0x200] = {};
    uint8_t stream[0x80] = {};
    int join_type = -1, flag = 0;
    uint8_t ok = 0;
    void* r = result;
    void* istrm = stream;
    int* pjt = &join_type;
    int* pfl = &flag;
    __asm {
        push data_size
        push data
        mov ecx, istrm
        mov eax, kInStreamCtor
        call eax

        push 4
        push pjt
        mov ecx, istrm
        mov eax, kInStreamRead
        call eax

        push 4
        push pfl
        mov ecx, istrm
        mov eax, kInStreamRead
        call eax

        mov ecx, r
        mov eax, kResultInit
        call eax

        push istrm
        mov ecx, r
        mov eax, kResultDeserialize
        call eax
        mov ok, al
    }
    if (ok) {
        put<int>(gm, kGmJoinType, join_type);
        put<uint8_t>(gm, kGmJoinFlag, static_cast<uint8_t>(flag));
        __asm {
            push r
            mov ecx, s
            mov eax, kJoinSession
            call eax
        }
        // Where the summoned player appears (vanilla: the sign's position), mirrored as the game does.
        const float one = 1.0f, zero = 0.0f;
        const float a[4] = {pos.x, pos.y, pos.z, one};
        const float b[4] = {zero, angle, zero, zero};
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(gm + 0xA80)), a, 16);
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(gm + 0xA90)), b, 16);
        put<int>(gm, 0xAA4, map);
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(gm + 0xAB0)), a, 16);
        std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(gm + 0xAC0)), b, 16);
    }
    __asm {
        mov ecx, r
        mov eax, kResultDtor
        call eax
    }
    return ok != 0;
}

void leave_session() {
    const uint32_t s = sess_man();
    if (!s) return;
    __asm {
        push esi
        mov esi, s
        mov eax, kLeaveSession
        call eax
        pop esi
    }
}

void keep_room_open() {
    const uint32_t s = sess_man();
    if (!s) return;
    put<float>(s, kSessRoomTimer, -1.0f);
    put<uint8_t>(s, kSessLocked, 0);
}

bool flag(int id) {
    const uint32_t efm = event_flag_man();
    if (!efm || id < 0) return false;
    uint8_t on = 0;
    __asm {
        push id
        xor ecx, ecx
        mov eax, efm
        mov edx, kGetFlagCore
        call edx
        mov on, al
    }
    return on != 0;
}

void set_flag(int id, bool on) {
    const uint32_t efm = event_flag_man();
    if (!efm || id < 0) return;
    uint32_t v = on ? 1 : 0;
    __asm {
        push v
        push id
        xor ecx, ecx
        mov eax, efm
        mov edx, kSetFlagCore
        call edx
    }
}

void set_own_world_flag(int id, bool on) {
    const uint32_t efm = event_flag_man();
    if (!efm) return;
    const uint32_t active = field<uint32_t>(efm, 0x10C, 0);
    const uint32_t own = efm + 0x14;
    put<uint32_t>(efm, 0x10C, own);
    set_flag(id, on);
    put<uint32_t>(efm, 0x10C, active);
}

int bonfire_map(int bonfire) {
    if (bonfire < 0) return -1;
    const int area = (bonfire / 100000) % 100;
    const int block = (bonfire / 10000) % 10;
    if (area == 99) return -1;
    return (area << 24) | (block << 16);
}

void warp(int map, int point) {
    const uint32_t gm = game_man();
    if (!gm || !player_game_data()) return;
    put<int>(gm, kGmWarpMap, map);
    put<int>(gm, kGmWarpPoint, point);
    if (point < 0) put<uint8_t>(gm, kGmUseBonfire, 1);
    __asm {
        mov eax, kWarpKick
        call eax
    }
}

bool call_lua(const char* name) {
    // As 0xD2D2F0 does: eax = [[0x137D790] + 4], ecx = function name.
    uint32_t events = 0, script = 0;
    if (!read32(kLuaEventsVar, &events) || !events || !read32(events + 4, &script) || !script) return false;
    events = script;
    uint8_t ok = 0;
    __try {
        __asm {
            mov eax, events
            mov ecx, name
            mov edx, 0x00D65410
            call edx
            mov ok, al
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok != 0;
}

int soul_level() { return field<int>(player_game_data(), 0x88, 0); }
int souls() { return field<int>(player_game_data(), 0x8C, 0); }
void set_souls(int value) { put<int>(player_game_data(), 0x8C, value < 0 ? 0 : value); }
int death_count() { return field<int>(game_data_man(), 0x5C, 0); }

}  // namespace coop::game
