// FreshOuttaFactory - FH2 mod platform mod.
// Keeps every car showroom-fresh: switches off visual crash damage for the player's car AND
// every Drivatar (the game's own damage option only covers the player), and can keep all cars
// clean by holding their dirt and mud shader levels at zero. Both options live on
// Pause > Settings > MODS and can be flipped while driving.
//
// How it works: every car's visuals go through one render-side class,
// Presentation11::CarPresentation. Four of its ICarPresentation methods are hooked:
//   ApplyDamage   game+0x137D0C0  dents/deformation/scratches  -> skipped when "no damage" is on
//   DetachPart    game+0x1384B80  bumpers/hoods falling off    -> skipped when "no damage" is on
//   SetDirtAmount game+0x13A9C40  body dirt level (0..1)       -> forced to 0 when "always clean" is on
//   SetMudAmount  game+0x13AB9D0  body mud level (0..1)        -> forced to 0 when "always clean" is on
// The game's simulation still runs untouched: this only stops the damage/dirt from ever being drawn.
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "fh2mod.h"

#define MOD_VERSION "1.0.0"

// Game offsets (Forza Horizon 2 PC, the 24 Nov 2015 build the platform targets),
// all methods of Presentation11::CarPresentation (vtable game+0x2EC1450).
#define RVA_APPLY_DAMAGE 0x137D0C0   // ICarPresentation vtable +0x190
#define RVA_DETACH_PART  0x1384B80   // ICarPresentation vtable +0x0B0
#define RVA_SET_DIRT     0x13A9C40   // ICarPresentation vtable +0x268
#define RVA_SET_MUD      0x13AB9D0   // ICarPresentation vtable +0x270

static const unsigned char kApplyDamageBytes[] = "\x48\x8B\xC4\x48\x89\x58\x10\x48\x89\x70\x18\x55";
static const unsigned char kDetachPartBytes[]  = "\x48\x8B\x89\x78\x12\x00\x00\xE9";
static const unsigned char kSetDirtBytes[]     = "\xC5\xFA\x11\x89\x1C\x13\x00\x00\xC3";
static const unsigned char kSetMudBytes[]      = "\xC5\xFA\x11\x89\x20\x13\x00\x00\xC3";

static const FH2ModApi *g_api;
static FH2Mod *g_mod;
static unsigned char *g_game;
static char g_ini[MAX_PATH];

static volatile LONG g_no_damage;    // "No crash damage" Mods-page switch
static volatile LONG g_no_dirt;      // "Always clean cars" Mods-page switch

static void logf_(const char *fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    g_api->log(g_mod, b);
}

// ---- detours ------------------------------------------------------------------------
// ApplyDamage(this, const DamageDesc*, const float4* local_pos, float impact, float a5, float a6, unsigned flags)
typedef void (*ApplyDamageFn)(void *, void *, void *, float, float, float, unsigned);
static ApplyDamageFn g_orig_apply_damage;
static void detour_apply_damage(void *self, void *desc, void *pos, float impact, float a5, float a6, unsigned flags) {
    if (g_no_damage) return;                    // the dent is never drawn, on any car
    g_orig_apply_damage(self, desc, pos, impact, a5, a6, flags);
}

// DetachPart(this, unsigned part)
typedef void (*DetachPartFn)(void *, unsigned);
static DetachPartFn g_orig_detach_part;
static void detour_detach_part(void *self, unsigned part) {
    if (g_no_damage) return;                    // bumpers and hoods stay on
    g_orig_detach_part(self, part);
}

// SetDirtAmount(this, float) / SetMudAmount(this, float)
typedef void (*SetAmountFn)(void *, float);
static SetAmountFn g_orig_set_dirt, g_orig_set_mud;
static void detour_set_dirt(void *self, float amount) {
    g_orig_set_dirt(self, g_no_dirt ? 0.f : amount);
}
static void detour_set_mud(void *self, float amount) {
    g_orig_set_mud(self, g_no_dirt ? 0.f : amount);
}

// ---- hooks --------------------------------------------------------------------------
static int check_bytes(const char *name, unsigned rva, const unsigned char *expect, size_t n) {
    if (memcmp(g_game + rva, expect, n) == 0) return 1;
    logf_("%s bytes mismatch at game+0x%X; that feature is disabled", name, rva);
    return 0;
}

static DWORD WINAPI hook_thread(void *) {
    g_game = (unsigned char *)g_api->wait_for_code(60000);
    if (!g_game) { logf_("wait_for_code timed out; FreshOuttaFactory is inactive"); return 0; }

    int ok = 0;
    if (check_bytes("CarPresentation::ApplyDamage", RVA_APPLY_DAMAGE, kApplyDamageBytes, sizeof kApplyDamageBytes - 1) &&
        check_bytes("CarPresentation::DetachPart", RVA_DETACH_PART, kDetachPartBytes, sizeof kDetachPartBytes - 1)) {
        FH2Hook *h1 = g_api->hook_function(g_mod, "CarPresentation::ApplyDamage",
            g_game + RVA_APPLY_DAMAGE, (void *)detour_apply_damage, (void **)&g_orig_apply_damage);
        FH2Hook *h2 = g_api->hook_function(g_mod, "CarPresentation::DetachPart",
            g_game + RVA_DETACH_PART, (void *)detour_detach_part, (void **)&g_orig_detach_part);
        if (h1 && h2) ok |= 1; else logf_("damage hooks failed (see the platform log)");
    }
    if (check_bytes("CarPresentation::SetDirtAmount", RVA_SET_DIRT, kSetDirtBytes, sizeof kSetDirtBytes - 1) &&
        check_bytes("CarPresentation::SetMudAmount", RVA_SET_MUD, kSetMudBytes, sizeof kSetMudBytes - 1)) {
        FH2Hook *h3 = g_api->hook_function(g_mod, "CarPresentation::SetDirtAmount",
            g_game + RVA_SET_DIRT, (void *)detour_set_dirt, (void **)&g_orig_set_dirt);
        FH2Hook *h4 = g_api->hook_function(g_mod, "CarPresentation::SetMudAmount",
            g_game + RVA_SET_MUD, (void *)detour_set_mud, (void **)&g_orig_set_mud);
        if (h3 && h4) ok |= 2; else logf_("dirt hooks failed (see the platform log)");
    }
    if (!ok) { logf_("nothing hooked; FreshOuttaFactory is inactive"); return 0; }
    if (!g_api->enable_hooks(g_mod, 0)) { logf_("enable_hooks failed (see the platform log)"); return 0; }
    logf_("hooks in place (damage %s, dirt %s); no damage %s, always clean %s",
          (ok & 1) ? "ok" : "unavailable", (ok & 2) ? "ok" : "unavailable",
          g_no_damage ? "on" : "off", g_no_dirt ? "on" : "off");
    return 0;
}

// ---- Mods page switches -------------------------------------------------------------
static void on_change(void *, const char *key, int value) {
    if (!key) return;
    if (!strcmp(key, "NoDamage")) {
        g_no_damage = value ? 1 : 0;
        WritePrivateProfileStringA("settings", "NoDamage", value ? "1" : "0", g_ini);
    } else if (!strcmp(key, "NoDirt")) {
        g_no_dirt = value ? 1 : 0;
        WritePrivateProfileStringA("settings", "NoDirt", value ? "1" : "0", g_ini);
    }
}

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi *api) {
    g_api = api;
    FH2ModInfo info = {};
    info.size = sizeof info;
    info.name = "FreshOuttaFactory";
    info.version = MOD_VERSION;
    info.platform_version = api->platform_version;
    info.github_repo = "suderek22/FH2_FreshOuttaFactory";
    info.check_updates = 1;
    info.description = "Keeps every car showroom-fresh: no visual crash damage on your car or any Drivatar, "
                       "and (optionally) no dirt or mud build-up on any car.";
    g_mod = api->register_mod(&info);
    if (!g_mod) return 0;

    HMODULE self;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&fh2mod_init, &self);
    GetModuleFileNameA(self, g_ini, MAX_PATH);
    char *dot = strrchr(g_ini, '.');
    if (dot) strcpy(dot, ".ini");
    g_no_damage = GetPrivateProfileIntA("settings", "NoDamage", 1, g_ini) ? 1 : 0;   // default: on
    g_no_dirt   = GetPrivateProfileIntA("settings", "NoDirt",   0, g_ini) ? 1 : 0;   // default: off

    FH2Setting s = {};
    s.size = sizeof s;
    s.key = "NoDamage";
    s.label = "No crash damage (all cars)";
    s.description = "Cars never show crash damage: no dents, scratches or parts falling off - on your car and "
                    "every Drivatar. Damage a car already has stays until it is repaired or reloaded. "
                    "The game's own damage simulation (mechanical damage) is not changed.";
    s.value = (int)g_no_damage;
    s.on_change = on_change;
    if (!api->add_setting(g_mod, &s)) logf_("could not add the NoDamage switch");

    FH2Setting d = {};
    d.size = sizeof d;
    d.key = "NoDirt";
    d.label = "Always clean (no dirt)";
    d.description = "Dirt and mud never build up on any car: yours and every Drivatar's stay freshly washed. "
                    "A car that is already dirty is wiped clean the moment it moves.";
    d.value = (int)g_no_dirt;
    d.on_change = on_change;
    if (!api->add_setting(g_mod, &d)) logf_("could not add the NoDirt switch");

    CreateThread(0, 0, hook_thread, 0, 0, 0);
    logf_("FreshOuttaFactory %s loaded (no damage %s, always clean %s)",
          MOD_VERSION, g_no_damage ? "on" : "off", g_no_dirt ? "on" : "off");
    return 0;
}
