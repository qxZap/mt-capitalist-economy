"""The Capitalist Economy mod: what has to be right before the game reads its tables.

    python mod/build_mod.py

Vanilla Cargos_01's rows with stacking on and the volume discount off, as this mod's own
table Cargos_CapitalistEconomy plus its own copy of the Cargos composite, so it works
alone. With the DataTableFramework installed, its composite wins and lists this table
(registry.json "adopted"); a later table wins a same-named row, so these override vanilla
and leave every other mod's table alone. A cargo table of any other mod (Proxy, Arini) is
not copied here: its rows only exist where that mod is installed. The plugin changes those.
"""
import json
import os
import subprocess
import sys

MT = "D:/MT/MoreTuning"
sys.path.insert(0, MT + "/Rebase")
import channels  # noqa: E402
from pipeline import WORK, to_json  # noqa: E402

VANILLA = "D:/MTLiveMap/vanilla_extract/MotorTown/Content"  # the current game build
HERE = os.path.dirname(os.path.abspath(__file__))
NAME = "qxZap_CapitalistEconomy_P"
DEST = os.path.join(HERE, NAME, "MotorTown", "Content")
PACKAGE = "/Game/DataAsset/Cargos_CapitalistEconomy"
REPAK = "D:/MT/PakMerge/repak.exe"


def edit(rows):
    stacked = flat = 0
    for row in rows:
        for p in row["Value"]:
            if p["Name"] == "bAllowStacking" and not p["Value"]:
                p["Value"] = True
                stacked += 1
            elif p["Name"] == "PaymentSqrtRatio" and p["Value"] not in (0, 0.0, "+0"):
                p["Value"] = 0.0
                flat += 1
    return stacked, flat


def main():
    src = os.path.join(WORK, "capecon.json")
    with open(to_json(VANILLA + "/DataAsset/Cargos_01.uasset", src), encoding="utf-8") as f:
        table = json.load(f)
    rows = channels.rows_of(table)
    stacked, flat = edit(rows)
    channels.ship(channels.framework.make_table(table, PACKAGE, rows), PACKAGE, DEST, "Capitalist Economy")
    channels.framework.mod_composite("Cargos", [PACKAGE], DEST)
    subprocess.run([REPAK, "pack", os.path.join(HERE, NAME), os.path.join(HERE, NAME + ".pak")], check=True, stdout=subprocess.DEVNULL)
    print(f"{len(rows)} rows, stacking on for {stacked}, discount off on {flat}")


if __name__ == "__main__":
    main()
