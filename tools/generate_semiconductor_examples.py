"""Generate shipped semiconductor examples; no Python is needed by the application."""
from pathlib import Path
from generate_converter_examples import Diagram, quoted


def recovery():
    p = Diagram("diode-recovery", "Diode reverse recovery")
    p.schema = 10
    p.profile = "0.00018 0.000000025 Trapezoidal"
    ground = p.node("ground", 0, 240, ground=True)
    voltage = p.node("diode_voltage", 180, 0)
    source = p.component("Source", "V", 3, -240, 100, turns=1)
    probe = p.component("diode_current", "IP", 0, -40, 0)
    diode = p.component("Diode", "D", 0, 180, 100, turns=1)
    p.wire(source["p"], probe["p"], [(-240, 0)])
    p.wire(probe["n"], voltage)
    p.wire(voltage, diode["p"])
    p.wire(diode["n"], ground)
    p.wire(ground, source["n"])
    p.plot("Voltage and recovery current", [voltage, probe["out"]], 520, 60,
           routes=[[(180, -100), (420, -100), (420, 48)],
                   [(-40, -160), (450, -160), (450, 72)]])
    p.lines += [
        "source {} 2 -2 5000 0 0 0.5 0".format(quoted(source["p"][0])),
        "semiconductor {} 1 2 100000 0.7".format(quoted(diode["p"][0])),
        "diode_charge {} 1 0.000001 0.000005 0".format(quoted(diode["p"][0])),
        'x-label {} "name" 0 -65 0 0'.format(quoted(voltage[0])),
    ]
    return "\n".join(p.body()) + "\n"


if __name__ == "__main__":
    target = Path(__file__).resolve().parents[1] / "examples" / "diode-recovery.pds"
    target.write_text(recovery(), encoding="utf-8")
