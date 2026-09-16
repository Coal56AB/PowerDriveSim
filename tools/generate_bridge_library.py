"""Editable half/full bridges with explicit antiparallel diodes."""
from pathlib import Path
from generate_converter_examples import Diagram, quoted


def half_bridge():
    p = Diagram("library/half-bridge", "Half bridge")
    p.schema = 12
    positive = p.node("DC+", -120, -260)
    phase = p.node("AC", -120, 0)
    negative = p.node("DC-", -120, 260)
    gates = []
    for name, top, bottom, y in [("H", positive, phase, -130), ("L", phase, negative, 130)]:
        switch = p.component("S" + name, "S", 0, 0, y, turns=1)
        diode = p.component("D" + name, "D", 0, 160, y, turns=3)
        p.wire(top, switch["p"], [(0, y - 130)])
        p.wire(switch["n"], bottom, [(0, y + 130)])
        p.wire(top, diode["n"], [(160, y - 130)])
        p.wire(diode["p"], bottom, [(160, y + 130)])
        gates.append(switch["gate"])
    p.port("DC+", positive); p.port("AC", phase); p.port("DC-", negative)
    p.port("gH", gates[0], gate=True); p.port("gL", gates[1], gate=True)
    return p


def full_bridge():
    leg = half_bridge()
    p = Diagram("library/full-bridge", "Full bridge")
    p.schema = 12
    a = p.instance("Leg A", leg, -140, 0)
    b = p.instance("Leg B", leg, 180, 0)
    positive = p.node("DC+", -360, -220)
    negative = p.node("DC-", -360, 220)
    for ports, x in [(a, -270), (b, 50)]:
        p.wire(positive, ports["DC+"], [(x, -220), (x, -39)])
        p.wire(ports["DC-"], negative, [(x - 20, -13), (x - 20, 220)])
    p.port("DC+", positive); p.port("A", a["AC"])
    p.port("DC-", negative); p.port("B", b["AC"])
    for name, ports in [("A", a), ("B", b)]:
        for level in ["H", "L"]:
            p.port("g" + name + level, ports["g" + level], gate=True)
    return p, leg


def definitions(full):
    if full:
        return full_bridge()
    return (half_bridge(),)


def fragment(full):
    bodies = definitions(full)
    p = Diagram("library/" + ("full" if full else "half") + "-bridge/fragment", bodies[0].name)
    p.schema = 12
    p.instance(bodies[0].name, bodies[0], 0, 0)
    lines = p.body()
    for body in bodies: lines += body.definition()
    return "\n".join(lines) + "\n"


def example(full):
    bodies = definitions(full)
    p = Diagram(("full" if full else "half") + "-bridge/example", bodies[0].name + " with RL load")
    p.schema = 12; p.profile = "0.006 0.000002 Trapezoidal"
    block = p.instance(bodies[0].name, bodies[0], 0, 0)
    neutral = p.node("Neutral", -380, 300, ground=True)
    if full:
        source = p.component("Supply", "V", 24, -380, 60, turns=1)
        p.wire(source["p"], block["DC+"], [(-380, -100), (-160, -100)])
        p.wire(source["n"], neutral)
        p.wire(neutral, block["DC-"], [(-180, 300), (-180, -39)])
        output, return_node = block["A"], block["B"]
    else:
        upper = p.component("Supply+", "V", 12, -380, -100, turns=1)
        lower = p.component("Supply-", "V", 12, -380, 100, turns=1)
        midpoint = p.node("Midpoint", -380, 0)
        p.wire(upper["n"], midpoint); p.wire(midpoint, lower["p"])
        p.wire(midpoint, neutral, [(-500, 0), (-500, 300)])
        p.wire(upper["p"], block["DC+"], [(-380, -220), (-180, -220)])
        p.wire(lower["n"], block["DC-"], [(-380, 220), (-180, 220), (-180, -13)])
        output, return_node = block["AC"], neutral
    positive = p.pattern("Positive", [True, False, True, False, True, False, True], -350, -380)
    negative = p.pattern("Negative", [False, True, False, True, False, True, False], -350, -280)
    for gate, names, x in [(positive, ["gAH", "gBL"] if full else ["gH"], -140),
                           (negative, ["gAL", "gBH"] if full else ["gL"], -120)]:
        for name in names:
            port_y = {"gAH": -13, "gAL": 13, "gBH": 39, "gBL": 65, "gH": 13, "gL": 39}[name]
            p.wire(gate, block[name], [(x, -380 if gate == positive else -280), (x, port_y)])
    resistor = p.component("Load R", "R", 10, 340, -100)
    inductor = p.component("Load L", "L", .01, 540, -100)
    current = p.component("Iload", "IP", 0, 740, -100)
    voltage = p.component("Uload", "VP", 0, 300, 100, turns=1)
    p.wire(output, resistor["p"], [(180, -13 if full else 0), (180, -100)])
    p.wire(resistor["n"], inductor["p"]); p.wire(inductor["n"], current["p"])
    p.wire(current["n"], return_node, [(840, -100), (840, 260), (140, 260), (140, 13 if full else 300)])
    p.wire(output, voltage["p"], [(200, -13 if full else 0), (200, 40)])
    p.wire(voltage["n"], return_node, [(300, 260), (140, 260), (140, 13 if full else 300)])
    p.plot("Load voltage and current", [voltage["out"], current["out"]], 1080, 40,
           routes=[[(900, 100), (900, 29)], [(740, -180), (940, -180), (940, 51)]])
    lines = p.body()
    for body in bodies: lines += body.definition()
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    for full in [False, True]:
        name = ("full" if full else "half") + "-bridge.pds"
        (root / "library" / "converters" / name).write_text(fragment(full), encoding="utf-8")
        (root / "examples" / name).write_text(example(full), encoding="utf-8")
