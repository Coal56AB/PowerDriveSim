"""Bidirectional buck/boost from the shared half-bridge definition."""
from pathlib import Path
from generate_converter_examples import Diagram, quoted
from generate_bridge_library import half_bridge


def stage():
    leg = half_bridge()
    p = Diagram("library/bidirectional-buck-boost", "Bidirectional buck-boost")
    p.schema = 12
    block = p.instance("Half bridge", leg, -140, 0)
    high = p.node("HV", -400, -180)
    low = p.node("LV", 360, 0)
    common = p.node("COM", -400, 220)
    inductor = p.component("Inductor", "L", .002, 160, 0)
    p.wire(high, block["DC+"], [(-300, -180), (-300, -39)])
    p.wire(block["DC-"], common, [(-280, -13), (-280, 220)])
    p.wire(block["AC"], inductor["p"]); p.wire(inductor["n"], low)
    p.port("HV", high); p.port("LV", low); p.port("COM", common)
    p.port("gH", block["gH"], gate=True); p.port("gL", block["gL"], gate=True)
    p.parameter("L", inductor["p"], "value", "H", .002)
    p.parameter("iL(0)", inductor["p"], "initial", "A", 0)
    return p, leg


def fragment():
    bodies = stage()
    p = Diagram("library/bidirectional-buck-boost/fragment", bodies[0].name)
    p.schema = 12; p.instance(bodies[0].name, bodies[0], 0, 0)
    lines = p.body()
    for body in bodies: lines += body.definition()
    return "\n".join(lines) + "\n"


def example(discharge):
    bodies = stage()
    p = Diagram("bidirectional/" + ("discharge" if discharge else "charge"),
                "Bidirectional energy return" if discharge else "Bidirectional charging")
    p.schema = 12; p.profile = "0.04 0.000001 Trapezoidal"
    block = p.instance(bodies[0].name, bodies[0], 0, 0)
    source = p.component("HV supply", "V", 24, -360, 100, turns=1)
    battery = p.component("LV source", "V", 13.5 if discharge else 10.5, 640, 160, turns=1)
    resistance = p.component("Series R", "R", .5, 460, 0)
    current = p.component("Ilow", "IP", 0, 260, 0)
    common = p.node("COM", 0, 300, ground=True)
    low = p.node("Ulow", 640, 0)
    p.wire(source["p"], block["HV"], [(-360, -39)])
    p.wire(source["n"], common, [(-360, 300)])
    p.wire(common, block["COM"], [(-200, 300), (-200, -13)])
    p.wire(block["LV"], current["p"]); p.wire(current["n"], resistance["p"])
    p.wire(resistance["n"], low); p.wire(low, battery["p"])
    p.wire(battery["n"], common, [(640, 300)])
    for name, initial, y, gate_port, port_y, x in [("High gate", True, -260, "gH", 13, -160),
                                                 ("Low gate", False, -160, "gL", 39, -140)]:
        ident = p.uuid(name)
        p.lines.append('pattern {} {} -340 {} {}'.format(quoted(ident), quoted(name), y, int(initial)))
        for edge in range(1, 801):
            p.lines.append('event {} {} {}'.format(edge / 20000, quoted(ident), int(initial != bool(edge % 2))))
        p.wire((ident, "out"), block[gate_port], [(x, y), (x, port_y)])
    p.plot("Low-side current and voltage", [current["out"], low], 920, -20,
           routes=[[(260, -100), (800, -100), (800, -31)], [(760, 0), (760, -9)]])
    lines = p.body()
    for body in bodies: lines += body.definition()
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    (root / "library/converters/bidirectional-buck-boost.pds").write_text(fragment(), encoding="utf-8")
    for discharge in [False, True]:
        name = "bidirectional-discharge" if discharge else "bidirectional-charge"
        (root / "examples" / (name + ".pds")).write_text(example(discharge), encoding="utf-8")
