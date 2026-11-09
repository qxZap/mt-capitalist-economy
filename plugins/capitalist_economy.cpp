// Capitalist Economy -- cargo pay, as plugins\mt_plugin_CapitalistEconomy.dll.
//
// Ported from capitalist_economy\src\dllmain.cpp: the pricing is unchanged; only
// the plumbing moved. Engine discovery, the .usmap export and the ProcessEvent
// hook now belong to the loader, and Ctrl+F10 is its own plugin
// (mt_plugin_VehicleParker.dll). Settings stay in Binaries\Win64\
// capitalist_economy.ini, where they always were.
//
// Recomputes every live cargo's Net_Payment from the cargo's real weight, its
// size and the straight-line distance between pickup and drop-off.
//
// On a dedicated server it also sets the economy's console variables (AI driver share, depot use,
// patrol pay..), the [Cvars] section of the ini. They go into Config\UserEngine.ini before the
// engine reads it, which only takes for the few that are not cheat variables, and are then
// written in memory once the world is up (see findCvars) and read back to check.
// shortcut: server only. A client's copy would count as a console variable mismatch on a vanilla
// server and MTNet rewrites that file whole. Merge there if single player needs them too.

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include "../include/mt_plugin.hpp"

using namespace mt;

static bool flatPay, stackAll;

struct Cvar { const wchar_t* key; double v; };
static Cvar cvars[] = {
    // The template's policePatrolPointPayment / AreaPayment are now ...Base, beside a per-hour rate
    // (vanilla 300 + 10000/h, 2000 + 20000/h). Its policePatrolPayment has no successor.
    {L"mh.requestRewardsPeriodSeconds", 600}, {L"mh.policePatrolPointPaymentBase", 2000},
    {L"mh.policePatrolAreaPaymentBase", 5000}, {L"mh.refuelEVMinPercentPerSeconds", 0.1}, {L"mh.companyTruckProfitShare", 0.7},
    {L"mh.busCompanyProfitShare", 0.7}, {L"mh.policeSpikePadDeploySeconds", 1}, {L"mh.policePatrolPointsPerArea", 20},
    {L"mh.policePatrolPointsPerAreaMin", 6}, {L"mh.policePatrolAreaPerZoneMultiplierByPopulation", 4},
    {L"mh.companyDepotStorageUsagePerService", 0.01}, {L"mh.aISpeederSpawnRatio", 400}, {L"mh.invalidPartsDisableEngine", 0},
    {L"mh.invalidPartsDisableLeaderboard", 0}, {L"mh.invalidPartsLifespan", 0}, {L"mh.cargoStackMaxVehicleHeight", 840}};
static bool onServer;
static uintptr_t systemCDO, fRead;

static bool isDedicated() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return wcsstr(exe, L"MotorTownServer") != nullptr;
}

static void writeCvars() {
    std::wstring dir = gameDir() + L"..\\..\\Config";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\UserEngine.ini";
    for (Cvar& c : cvars) {
        c.v = iniNum(L"capitalist_economy.ini", L"Cvars", c.key, c.v);
        wchar_t text[32];
        swprintf_s(text, L"%g", c.v);
        WritePrivateProfileStringW(L"ConsoleVariables", c.key, text, path.c_str());
    }
}

static float readCvar(const wchar_t* key) {
    struct { const wchar_t* data; int num, max; } in = {key, (int)wcslen(key) + 1, (int)wcslen(key) + 1};
    unsigned char p[0x40] = {0};
    memcpy(p + off(fRead, "VariableName"), &in, sizeof in);
    api->process_event(systemCDO, fRead, p);
    float out = 0;
    memcpy(&out, p + off(fRead, "ReturnValue"), 4);
    return out;
}

// Most of these are ECVF_Cheat, which a shipping build will not set from the console or an ini,
// so their values are written where the variable keeps them. Each TAutoConsoleVariable is built
// by `lea rdx, name ... call [rax+10h or 18h]` (RegisterConsoleVariable, float or int), then the
// result goes to the global: `mov rdx, rax; lea rcx, global; call` or `mov [global+8], rax`.
// The global holds the variable at +8 and its TConsoleVariableData (the game and render thread
// copies) at +10h. Anything that does not read back as the game's own value is left alone.
struct Site { uintptr_t data; bool isInt; };
static std::unordered_map<std::wstring, Site> findCvars() {
    std::unordered_map<std::wstring, Site> out;
    auto base = (const uint8_t*)api->module_base();
    auto nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const uint8_t *text = nullptr, *rdata = nullptr;
    size_t textN = 0, rdataN = 0;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (!memcmp(sec->Name, ".text", 6)) { text = base + sec->VirtualAddress; textN = sec->Misc.VirtualSize; }
        if (!memcmp(sec->Name, ".rdata", 7)) { rdata = base + sec->VirtualAddress; rdataN = sec->Misc.VirtualSize; }
    }
    if (!text || !rdata) return out;
    std::unordered_map<uintptr_t, const wchar_t*> names;
    for (Cvar& c : cvars) {
        size_t n = (wcslen(c.key) + 1) * 2;
        for (size_t i = 0; i + n <= rdataN; i += 2)
            if (!memcmp(rdata + i, c.key, n)) { names[(uintptr_t)(rdata + i)] = c.key; break; }
    }
    auto rip = [](const uint8_t* at, int len) { return (uintptr_t)(at + len) + *(const int32_t*)(at + len - 4); };
    for (size_t i = 0; i + 0x60 < textN; i++) {
        const uint8_t* p = text + i;
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15) continue;  // lea rdx, [rip+x]
        auto hit = names.find(rip(p, 7));
        if (hit == names.end()) continue;
        const uint8_t* call = nullptr;
        for (int k = 7; k < 0x30 && !call; k++)
            if (p[k] == 0xFF && p[k + 1] == 0x50 && (p[k + 2] == 0x10 || p[k + 2] == 0x18)) call = p + k;
        if (!call) continue;
        const uint8_t* q = call + 3;
        uintptr_t global = 0;
        if (!memcmp(q, "\x48\x8B\xD0\x48\x8D\x0D", 6)) global = rip(q + 3, 7);
        else for (int k = 0; k < 0x20 && !global; k++)
            if (q[k] == 0x48 && q[k + 1] == 0x89 && q[k + 2] == 0x05) global = rip(q + k, 7) - 8;
        uintptr_t data = global ? get<uintptr_t>(global + 0x10) : 0;
        if (data) out[hit->second] = {data, call[2] == 0x18};
    }
    return out;
}

static void checkCvars(void*) {
    auto sites = findCvars();
    int wrong = 0;
    for (Cvar& c : cvars) {
        float now = readCvar(c.key);
        auto s = sites.find(c.key);
        if (fabs(now - c.v) >= 1e-4 && s != sites.end()) {
            bool same = s->second.isInt ? get<int32_t>(s->second.data) == (int32_t)now : get<float>(s->second.data) == now;
            if (!same) MT_LOG("cvar %ls: its data does not read %g, left alone", c.key, now);
            else if (s->second.isInt) { int32_t v[2] = {(int32_t)llround(c.v), (int32_t)llround(c.v)}; wr(s->second.data, v, 8); }
            else { float v[2] = {(float)c.v, (float)c.v}; wr(s->second.data, v, 8); }
        }
        float out = readCvar(c.key);
        if (fabs(out - c.v) < 1e-4) continue;
        wrong++;
        MT_LOG("cvar %ls is %g, wanted %g%s", c.key, out, c.v, s == sites.end() ? " (not a registered variable here)" : "");
    }
    MT_LOG("console variables: %d of %d took, %d found in the exe", (int)(sizeof cvars / sizeof *cvars) - wrong, (int)(sizeof cvars / sizeof *cvars), (int)sites.size());
}

static DWORD WINAPI run(LPVOID) {
    MT_LOG("Capitalist Economy loaded: pay by straight line, weight, size and fragility");
    if (!api->wait_engine(INFINITE)) { MT_LOG("FAILED: loader found no engine -- pay stays vanilla"); return 0; }

    uintptr_t cargo = api->find_object("Class", "MTCargo", "/Script/MotorTown");
    int oKey = off(cargo, "Net_CargoKey"), oW = off(cargo, "Net_Weight"), oDmg = off(cargo, "Net_Damage"),
        oId = off(cargo, "Net_DeliveryId"), oDst = off(cargo, "Net_DestinationLocation"),
        oSrc = off(cargo, "Net_SenderAbsoluteLocation"), oOne = off(cargo, "Net_SingleCargoPayment"),
        oPay = off(cargo, "Net_Payment"), oMesh = off(cargo, "Mesh");
    MT_LOG("offsets: key %x weight %x dmg %x id %x dst %x src %x single %x pay %x", oKey, oW, oDmg, oId, oDst, oSrc, oOne, oPay);

    // ---- the job list: MTDeliverySystem.Deliveries is a TArray<MTDelivery> ----
    uintptr_t sysCls = api->find_object("Class", "MTDeliverySystem", "/Script/MotorTown");
    uintptr_t delStruct = api->find_object("ScriptStruct", "MTDelivery", "/Script/MotorTown");
    int oDeliveries = off(sysCls, "Deliveries"), dId = off(delStruct, "ID"), dPath = off(delStruct, "PathDistance");
    int dSize = get<int>(delStruct + L().struct_child_props + 8);  // UStruct::PropertiesSize follows ChildProperties
    MT_LOG("jobs: deliveries+%x | MTDelivery size %x id+%x path+%x", oDeliveries, dSize, dId, dPath);
    bool jobsOk = oDeliveries >= 0 && dSize > 0 && dId >= 0 && dPath >= 0;
    if (!jobsOk) MT_LOG("FAILED: job list layout -- pay stays vanilla");

    // ---- mass and size of the cargo mesh, by reflection ----
    int oBody = off(api->find_object("Class", "PrimitiveComponent", "/Script/Engine"), "BodyInstance");
    int oMass = off(api->find_object("ScriptStruct", "BodyInstance", "/Script/Engine"), "MassInKgOverride");
    int oStatic = off(api->find_object("Class", "StaticMeshComponent", "/Script/Engine"), "StaticMesh");
    int oExt = off(api->find_object("Class", "StaticMesh", "/Script/Engine"), "ExtendedBounds");
    int oBox = off(api->find_object("ScriptStruct", "BoxSphereBounds", "/Script/CoreUObject"), "BoxExtent");
    MT_LOG("mesh: mesh+%x body+%x mass+%x static+%x extbounds+%x boxextent+%x", oMesh, oBody, oMass, oStatic, oExt, oBox);

    // ---- cargo rows, read from the loaded tables (any mod's edits included) ----
    struct Row { float perKm = 0, fragile = 0; int flags = 0; };
    std::unordered_map<uint32_t, Row> rows;  // FName index -> row
    auto loadRows = [&]() {
        for (int i = 0, n = api->num_objects(); i < n; i++) {
            uintptr_t t = api->object(i);
            if (!t || isCDO(t)) continue;
            std::string cn = objName(classOf(t));
            if (cn != "DataTable" && cn != "CompositeDataTable") continue;
            // RowStruct: first pointer to a ScriptStruct; RowMap (TMap<FName, uint8*>) right after it
            int rs = -1; uintptr_t st = 0;
            for (int x = 0x28; x <= 0x48 && rs < 0; x += 8) {
                uintptr_t q = get<uintptr_t>(t + x);
                if (q && objName(classOf(q)) == "ScriptStruct") { rs = x; st = q; }
            }
            if (rs < 0 || objName(st) != "CargoRow") continue;  // any table of cargo rows: mods name theirs anything
            int rSqrt = flatPay ? off(st, "PaymentSqrtRatio") : -1;
            // Written whole, so only when it has its byte to itself and is no bitfield.
            int rStack = off(st, "bAllowStacking");
            if (rStack == off(st, "bUseDamage")) { MT_LOG("bAllowStacking is a bitfield here -- stacking left as it is"); rStack = -1; }
            if (!stackAll) rStack = -1;
            int rPer = off(st, "PaymentPer1Km"), rFrag = off(st, "Fragile"), rFlags = off(st, "CargoFlags");
            uintptr_t el = get<uintptr_t>(t + rs + 8); int num = get<int>(t + rs + 16);
            int got = 0, flat = 0, stacked = 0;
            for (int j = 0; j < num && j < 4096; j++) {  // TSetElement: FName, uint8*, hash links = 24 bytes
                uint32_t key = get<uint32_t>(el + 24ull * j); uintptr_t row = get<uintptr_t>(el + 24ull * j + 8);
                if (!row || name(key).empty()) continue;  // free slot
                Row r; r.perKm = rPer >= 0 ? get<float>(row + rPer) : 0; r.fragile = rFrag >= 0 ? get<float>(row + rFrag) : 0;
                r.flags = rFlags >= 0 ? get<int>(row + rFlags) : 0;
                if (r.perKm < 0 || r.perKm > 1e6) continue;
                rows[key] = r; got++;
                // the volume discount: the more a load holds, the less each unit pays. 0 turns
                // it off, as vanilla's one-off loads (transformers, the big tank) already do.
                float zero = 0;
                if (rSqrt >= 0 && get<float>(row + rSqrt) != 0) { wr(row + rSqrt, &zero, 4); flat++; }
                uint8_t yes = 1;
                if (rStack >= 0 && !get<uint8_t>(row + rStack)) { wr(row + rStack, &yes, 1); stacked++; }
            }
            MT_LOG("table %s: %d rows, volume discount off on %d, stacking on for %d (struct %s, perKm+%x fragile+%x flags+%x)", objName(t).c_str(), got, flat, stacked, objName(st).c_str(), rPer, rFrag, rFlags);
        }
    };

    // ---- settings: capitalist_economy.ini in Binaries\Win64, written with defaults if absent ----
    struct { const wchar_t* key; double v; } cfg[] = {
        { L"CurveAt1t", 435 }, { L"CurveExponent", 0.6 }, { L"DimKgPerM3", 250 },
        { L"OversizeBonus", 1.5 }, { L"NudgeMin", 0.85 }, { L"NudgeMax", 1.3 },
        { L"RoadShare", 0.5 }, { L"Enabled", 1 }, { L"FlatPay", 1 }, { L"StackAll", 1 },
        { L"RoadSpeed", 80 }, { L"OffroadSpeed", 40 }, { L"OffroadBonus", 1.5 } };
      for (auto& c : cfg) {
          c.v = iniNum(L"capitalist_economy.ini", L"Pay", c.key, c.v);
          MT_LOG("setting %ls = %g", c.key, c.v);
      }
    systemCDO = api->find_object("KismetSystemLibrary", "Default__KismetSystemLibrary", "/Script/Engine");
    fRead = api->find_object("Function", "GetConsoleVariableFloatValue", "KismetSystemLibrary");
    double kA = cfg[0].v, kE = cfg[1].v, kDim = cfg[2].v, kOver = cfg[3].v, kNMin = cfg[4].v, kNMax = cfg[5].v, kRoadShare = cfg[6].v;
    bool enabled = cfg[7].v != 0 && jobsOk;
    flatPay = cfg[8].v != 0;
    stackAll = cfg[9].v != 0;
    double kRoad = cfg[10].v, kOffroad = cfg[11].v, kBonus = cfg[12].v;
    auto clampd = [](double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; };

    std::unordered_map<uintptr_t, int64_t> wrote;  // cargo -> base we wrote
    std::unordered_map<int, int> jobLogged;        // job id -> cargos it had when logged
    int dKey = off(delStruct, "CargoKey"), dNum = off(delStruct, "NumCargos"), dWeight = off(delStruct, "Weight"),
        dDemand = off(delStruct, "PaymentMultiplierByDemand"), dSupply = off(delStruct, "PaymentMultiplierBySupply"),
        dConfig = off(delStruct, "PaymentMultiplierByBalanceConfig"), dClimb = off(delStruct, "PathClimbHeight"), dPace = off(delStruct, "PathSpeedKPH"),
        dCargos = off(delStruct, "Server_Cargos");
    for (;;) {
        Sleep(1000);
        if (!enabled) continue;
        static int sysAt = -1;  // the object walk is the costly part: only when the world changed
        uintptr_t sys = sysAt >= 0 ? api->object(sysAt) : 0;
        if (!sys || isCDO(sys) || classOf(sys) != sysCls) {
            sys = 0;
            for (int i = 0, n = api->num_objects(); i < n && !sys; i++) {
                uintptr_t o = api->object(i);
                if (o && !isCDO(o) && classOf(o) == sysCls) { sys = o; sysAt = i; }
            }
        }
        // Before the world too: what the game takes at load has to be changed by then.
        if (!sys || rows.empty()) loadRows();
        if (!sys) continue;
        static int checked;
        if (onServer && checked >= 0 && ++checked > 90 && systemCDO && fRead && api->game_thread_ready()) {
            checked = -1;
            api->run_on_game_thread(checkCvars, nullptr);
        }
        std::unordered_map<int, float> road, pace;  // job id -> PathDistance, PathSpeedKPH
        bool loaded = false;
        uintptr_t data = get<uintptr_t>(sys + oDeliveries); int num = get<int>(sys + oDeliveries + 8);
        for (int j = 0; j < num && j < 100000; j++)
        {
            uintptr_t d = data + (uintptr_t)j * dSize;
            int id = get<int>(d + dId), have = get<int>(d + dCargos + 8);
            loaded = loaded || have > 0;
            road[id] = get<float>(d + dPath);
            pace[id] = dPace >= 0 ? get<float>(d + dPace) : 0;
            // What each open job is worth, for tuning pay against real numbers.
            auto seen = jobLogged.find(id);
            if (seen != jobLogged.end() && seen->second >= (have ? 1 : 0)) continue;
            jobLogged[id] = have ? 1 : 0;
            int64_t single = 0;
            if (have) single = get<int64_t>(get<uintptr_t>(get<uintptr_t>(d + dCargos)) + oOne);
            MT_LOG("job %s road %.0f kph %.0f climb %.0f units %d weight %.0f demand %.2f supply %.2f config %.2f unit pay %lld",
                   fname(d + dKey).c_str(), road[id], pace[id], get<float>(d + dClimb), get<int>(d + dNum), get<float>(d + dWeight),
                   get<float>(d + dDemand), get<float>(d + dSupply), get<float>(d + dConfig), single);
        }

        for (int i = 0, n = loaded ? api->num_objects() : 0; i < n; i++) {
            uintptr_t o = api->object(i);
            if (!o || isCDO(o)) continue;
            if (!api->is_a(classOf(o), cargo)) continue;
            int64_t pay[2], one[2]; double src[3], dst[3];
            if (!rd(o + oPay, pay, 16) || !rd(o + oOne, one, 16) || !rd(o + oSrc, src, 24) || !rd(o + oDst, dst, 24)) continue;
            auto it = wrote.find(o);
            if (it != wrote.end() && it->second == pay[0]) continue;  // still ours
            bool again = it != wrote.end();
            auto job = road.find(get<int>(o + oId));
            if (pay[0] <= 0 || job == road.end() || job->second <= 0) continue;  // not on a job: leave it

            // paid distance = RoadShare of the road the game priced + the rest of
            // the full straight line between pickup and drop. The weights add up to
            // one, so with 0.5 this is plainly the average of the two distances. The
            // straight line is 3D -- dz is the climb, so a mountain haul is paid for it.
            double dx = dst[0]-src[0], dy = dst[1]-src[1], dz = dst[2]-src[2];
            double lineCm = sqrt(dx*dx + dy*dy + dz*dz);
            double r = lineCm / job->second;
            if (r > 20) r /= 100;  // PathDistance in metres rather than cm
            double fd = kRoadShare + (1.0 - kRoadShare) * r;
            // A slow route is off-road: it winds because the ground does, so it loses the
            // straight-line cut and earns a bonus, both by how slow it is. Speed is the game's own.
            double kph = pace[job->first], span = kRoad - kOffroad;
            double rough = kph > 0 && span > 0 ? clampd((kRoad - kph) / span, 0, 1) : 0;
            if (fd < 1) fd += (1 - fd) * rough;
            fd *= 1 + (kBonus - 1) * rough;

            // weight, size, fragility: nudge vanilla's rate toward a freight curve
            uint32_t key = get<uint32_t>(o + oKey);
            double kg = get<float>(o + oW);
            uintptr_t mesh = oMesh >= 0 ? get<uintptr_t>(o + oMesh) : 0;
            if (kg <= 0 && mesh && oBody >= 0 && oMass >= 0) kg = get<float>(mesh + oBody + oMass);
            double m3 = 0;
            uintptr_t sm2 = mesh && oStatic >= 0 ? get<uintptr_t>(mesh + oStatic) : 0;
            if (sm2 && oExt >= 0 && oBox >= 0) {
                double e[3];
                if (rd(sm2 + oExt + oBox, e, 24)) m3 = 8 * e[0] * e[1] * e[2] / 1e6;
            }
            double fn = 1;
            auto row = rows.find(key);
            if (row != rows.end() && row->second.perKm > 0 && (kg > 0 || m3 > 0)) {
                double t = kg / 1000; if (m3 * kDim / 1000 > t) t = m3 * kDim / 1000;
                double fair = kA * pow(t, kE) * (1 + row->second.fragile) * ((row->second.flags & 64) ? kOver : 1);
                fn = clampd(sqrt(fair / row->second.perKm), kNMin, kNMax);
            }

            double f = fd * fn;
            int64_t np = (int64_t)llround(pay[0] * f), no = (int64_t)llround(one[0] * f);
            int64_t outPay[2] = { np, pay[0] + pay[1] - np }, outOne[2] = { no, one[0] + one[1] - no };
            if (!wr(o + oPay, outPay, 16) || !wr(o + oOne, outOne, 16)) continue;
            wrote[o] = np;
            MT_LOG("%s %-22s job %6d  line %6.2f km  road %7.0f  ratio %.2f  kph %.0f  distance x%.2f | %7.1f kg %6.2f m3 row %s  size x%.2f | pay %lld -> %lld",
                   again ? "REPRICED" : "priced", name(key).c_str(), job->first, lineCm / 100000.0, job->second, r, kph, fd,
                   kg, m3, row != rows.end() ? "yes" : "NO", fn, pay[0], np);
        }
    }
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    if (!attach(a)) return 1;
    if (iniDisabled(L"capitalist_economy.ini")) return 0;
    onServer = isDedicated();
    if (onServer) writeCvars();
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
