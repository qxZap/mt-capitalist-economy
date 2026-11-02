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

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include "../include/mt_plugin.hpp"

using namespace mt;

static bool flatPay;

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
            int rPer = off(st, "PaymentPer1Km"), rFrag = off(st, "Fragile"), rFlags = off(st, "CargoFlags");
            uintptr_t el = get<uintptr_t>(t + rs + 8); int num = get<int>(t + rs + 16);
            int got = 0, flat = 0;
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
            }
            MT_LOG("table %s: %d rows, volume discount off on %d (struct %s, perKm+%x fragile+%x flags+%x)", objName(t).c_str(), got, flat, objName(st).c_str(), rPer, rFrag, rFlags);
        }
    };

    // ---- settings: capitalist_economy.ini in Binaries\Win64, written with defaults if absent ----
    struct { const wchar_t* key; double v; } cfg[] = {
        { L"CurveAt1t", 435 }, { L"CurveExponent", 0.6 }, { L"DimKgPerM3", 250 },
        { L"OversizeBonus", 1.5 }, { L"NudgeMin", 0.85 }, { L"NudgeMax", 1.3 },
        { L"RoadShare", 0.5 }, { L"Enabled", 1 }, { L"FlatPay", 1 } };
      for (auto& c : cfg) {
          c.v = iniNum(L"capitalist_economy.ini", L"Pay", c.key, c.v);
          MT_LOG("setting %ls = %g", c.key, c.v);
      }
    double kA = cfg[0].v, kE = cfg[1].v, kDim = cfg[2].v, kOver = cfg[3].v, kNMin = cfg[4].v, kNMax = cfg[5].v, kRoadShare = cfg[6].v;
    bool enabled = cfg[7].v != 0 && jobsOk;
    flatPay = cfg[8].v != 0;
    auto clampd = [](double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; };

    std::unordered_map<uintptr_t, int64_t> wrote;  // cargo -> base we wrote
    for (;;) {
        Sleep(1000);
        if (!enabled) continue;
        uintptr_t sys = 0;
        for (int i = 0, n = api->num_objects(); i < n && !sys; i++) {
            uintptr_t o = api->object(i);
            if (o && !isCDO(o) && classOf(o) == sysCls) sys = o;
        }
        if (!sys) continue;
        if (rows.empty()) loadRows();
        std::unordered_map<int, float> road;  // job id -> PathDistance
        uintptr_t data = get<uintptr_t>(sys + oDeliveries); int num = get<int>(sys + oDeliveries + 8);
        for (int j = 0; j < num && j < 100000; j++)
            road[get<int>(data + (uintptr_t)j * dSize + dId)] = get<float>(data + (uintptr_t)j * dSize + dPath);

        for (int i = 0, n = api->num_objects(); i < n; i++) {
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
            MT_LOG("%s %-22s job %6d  line %6.2f km  road %7.0f  ratio %.2f  distance x%.2f | %7.1f kg %6.2f m3 row %s  size x%.2f | pay %lld -> %lld",
                   again ? "REPRICED" : "priced", name(key).c_str(), job->first, lineCm / 100000.0, job->second, r, fd,
                   kg, m3, row != rows.end() ? "yes" : "NO", fn, pay[0], np);
        }
    }
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    if (!attach(a)) return 1;
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
