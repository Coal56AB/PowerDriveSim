"""Generate editable switching DC/DC power stages and their examples."""
from pathlib import Path
from generate_converter_examples import Diagram, quoted


def converter(kind):
    p = Diagram("library/" + kind, {"buck": "Buck", "boost": "Boost", "buck-boost": "Inverting buck-boost"}[kind])
    p.schema = 12
    source = p.node("IN", -360, -100)
    output = p.node("OUT", 360, -100)
    common = p.node("COM", 0, 280)
    junction = p.node("Switch node", 0, -100)
    if kind == "buck":
        switch = p.component("Switch", "S", 0, -180, -100)
        diode = p.component("Diode", "D", 0, 0, 40, turns=3)
        inductor = p.component("Inductor", "L", .002, 180, -100)
        p.wire(source, switch["p"]); p.wire(switch["n"], junction)
        p.wire(common, diode["p"]); p.wire(diode["n"], junction)
        p.wire(junction, inductor["p"]); p.wire(inductor["n"], output)
    elif kind == "boost":
        inductor = p.component("Inductor", "L", .002, -180, -100)
        switch = p.component("Switch", "S", 0, 0, 40, turns=1)
        diode = p.component("Diode", "D", 0, 180, -100)
        p.wire(source, inductor["p"]); p.wire(inductor["n"], junction)
        p.wire(junction, switch["p"]); p.wire(switch["n"], common)
        p.wire(junction, diode["p"]); p.wire(diode["n"], output)
    else:
        switch = p.component("Switch", "S", 0, -180, -100)
        inductor = p.component("Inductor", "L", .002, 0, 40, turns=1)
        diode = p.component("Diode", "D", 0, 180, -100, turns=2)
        p.wire(source, switch["p"]); p.wire(switch["n"], junction)
        p.wire(junction, inductor["p"]); p.wire(inductor["n"], common)
        p.wire(output, diode["p"]); p.wire(diode["n"], junction)
    esr = p.component("Capacitor ESR", "R", .05, 360, 20, turns=1)
    capacitor = p.component("Capacitor", "C", .00022, 360, 160, turns=1)
    p.wire(output, esr["p"]); p.wire(esr["n"], capacitor["p"])
    p.wire(capacitor["n"], common, [(360, 280)])
    p.port("IN", source); p.port("OUT", output); p.port("COM", common)
    p.port("g", switch["gate"], gate=True)
    p.parameter("L", inductor["p"], "value", "H", .002)
    p.parameter("iL(0)", inductor["p"], "initial", "A", 0)
    p.parameter("C", capacitor["p"], "value", "F", .00022)
    p.parameter("uC(0)", capacitor["p"], "initial", "V", 0)
    p.parameter("ESR", esr["p"], "value", "Ohm", .05)
    return p


def fragment(kind):
    body = converter(kind)
    p = Diagram("library/" + kind + "/fragment", body.name)
    p.schema = 12
    p.instance(body.name, body, 0, 0)
    return "\n".join(p.body() + body.definition()) + "\n"


def example(kind):
    body = converter(kind)
    p = Diagram(kind + "/example", body.name + " converter")
    p.schema = 12
    p.profile = "0.05 0.000001 Trapezoidal"
    block = p.instance(body.name, body, 0, 0)
    source = p.component("DC supply", "V", 24, -320, 120, turns=1)
    ground = p.node("COM", 0, 320, ground=True)
    current = p.component("Iout", "IP", 0, 240, 0)
    load = p.component("Load", "R", 20, 400, 160, turns=1)
    output = p.node("Uout", 400, 0)
    gate = p.uuid("PWM")
    p.lines.append('pwm {} "PWM" -300 -160 10000 0.4 0'.format(quoted(gate)))
    p.wire(source["p"], block["IN"], [(-320, -26)])
    p.wire(source["n"], ground, [(-320, 320)])
    p.wire(ground, block["COM"], [(-180, 320), (-180, 0)])
    p.wire((gate, "out"), block["g"], [(-200, -160), (-200, 26)])
    p.wire(block["OUT"], current["p"]); p.wire(current["n"], output)
    p.wire(output, load["p"]); p.wire(load["n"], ground, [(400, 320)])
    p.plot("Output voltage and current", [output, current["out"]], 740, 40,
           routes=[[(560, 0), (560, 29)], [(240, -100), (600, -100), (600, 51)]])
    return "\n".join(p.body() + body.definition()) + "\n"


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    for kind in ["buck", "boost", "buck-boost"]:
        (root / "library" / "converters" / (kind + ".pds")).write_text(fragment(kind), encoding="utf-8")
        (root / "examples" / (kind + ".pds")).write_text(example(kind), encoding="utf-8")
