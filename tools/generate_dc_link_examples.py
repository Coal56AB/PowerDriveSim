"""Explicit precharge, DC-link and switched resistor circuits."""
from pathlib import Path
from generate_converter_examples import Diagram


def precharge():
    p = Diagram("library/precharge", "Precharge")
    p.schema = 12
    source = p.node("IN", -240, 0); output = p.node("OUT", 240, 0)
    resistor = p.component("Precharge R", "R", 10, -100, 100)
    pre = p.component("Precharge switch", "S", 0, 100, 100)
    main = p.component("Bypass", "S", 0, 0, -100)
    p.wire(source, resistor["p"], [(-240, 100)])
    p.wire(resistor["n"], pre["p"]); p.wire(pre["n"], output, [(240, 100)])
    p.wire(source, main["p"], [(-240, -100)]); p.wire(main["n"], output, [(240, -100)])
    p.port("IN", source); p.port("OUT", output)
    p.port("gPre", pre["gate"], gate=True); p.port("gBypass", main["gate"], gate=True)
    p.parameter("Rpre", resistor["p"], "value", "Ohm", 10)
    return p


def dc_link():
    p = Diagram("library/dc-link", "DC-link")
    p.schema = 12
    positive = p.node("+", 0, -240); negative = p.node("-", 0, 240)
    esr = p.component("ESR", "R", .1, 0, -90, turns=1)
    capacitor = p.component("Capacitor", "C", .001, 0, 90, turns=1)
    p.wire(positive, esr["p"]); p.wire(esr["n"], capacitor["p"]); p.wire(capacitor["n"], negative)
    p.port("+", positive); p.port("-", negative)
    p.parameter("C", capacitor["p"], "value", "F", .001)
    p.parameter("uC(0)", capacitor["p"], "initial", "V", 0)
    p.parameter("ESR", esr["p"], "value", "Ohm", .1)
    return p


def switched_resistor():
    p = Diagram("library/switched-resistor", "Switched resistor")
    p.schema = 12
    positive = p.node("+", 0, -240); negative = p.node("-", 0, 240)
    resistor = p.component("Resistor", "R", 100, 0, -90, turns=1)
    switch = p.component("Switch", "S", 0, 0, 90, turns=1)
    p.wire(positive, resistor["p"]); p.wire(resistor["n"], switch["p"]); p.wire(switch["n"], negative)
    p.port("+", positive); p.port("-", negative); p.port("g", switch["gate"], gate=True)
    p.parameter("R", resistor["p"], "value", "Ohm", 100)
    return p


def fragment(key, body):
    p = Diagram("library/" + key + "/fragment", body.name)
    p.schema = 12; p.instance(body.name, body, 0, 0)
    return "\n".join(p.body() + body.definition()) + "\n"


def charging():
    pre, dc, resistor = precharge(), dc_link(), switched_resistor()
    p = Diagram("precharge-discharge", "Precharge, bypass and discharge")
    p.schema = 12; p.profile = "0.18 0.000002 Trapezoidal"
    block = p.instance(pre.name, pre, 0, 0)
    capacitor = p.instance(dc.name, dc, 480, 130)
    discharge = p.instance("Discharge", resistor, 800, 130)
    source = p.component("Supply", "V", 48, -340, 140, turns=1)
    current = p.component("Iin", "IP", 0, 220, 0)
    bus = p.node("Udc", 380, 0); ground = p.node("COM", 480, 360, ground=True)
    p.wire(source["p"], block["IN"], [(-340, -26)])
    p.wire(source["n"], ground, [(-340, 360)])
    p.wire(block["OUT"], current["p"]); p.wire(current["n"], bus)
    p.wire(bus, capacitor["+"], [(380, 130)])
    p.wire(capacitor["-"], ground, [(620, 130), (620, 360)])
    p.wire(bus, discharge["+"], [(680, 0), (680, 117)])
    p.wire(discharge["-"], ground, [(980, 130), (980, 360)])
    gpre = p.recorded("Precharge gate", True, [(.051, False)], -360, -240)
    bypass = p.recorded("Bypass gate", False, [(.05, True), (.07, False)], -360, -140)
    dump = p.recorded("Discharge gate", False, [(.08, True)], 580, -160)
    p.wire(gpre, block["gPre"], [(-180, -240), (-180, 0)])
    p.wire(bypass, block["gBypass"], [(-160, -140), (-160, 26)])
    p.wire(dump, discharge["g"], [(660, -160), (660, 143)])
    p.plot("DC voltage and charging current", [bus, current["out"]], 1160, -40,
           routes=[[(1040, 0), (1040, -51)], [(220, -100), (1080, -100), (1080, -29)]])
    return "\n".join(p.body() + pre.definition() + dc.definition() + resistor.definition()) + "\n"


def braking():
    dc, resistor = dc_link(), switched_resistor()
    p = Diagram("braking-chopper", "Braking chopper with external gate")
    p.schema = 12; p.profile = "0.06 0.00001 Trapezoidal"
    capacitor = p.instance(dc.name, dc, 0, 120, {"uC(0)": 48})
    brake = p.instance("Braking resistor", resistor, 420, 120, {"R": 40})
    source = p.component("Regenerated current", "I", .5, -340, 100, turns=3)
    bus = p.node("Udc", -340, -80); ground = p.node("COM", 160, 340, ground=True)
    p.wire(source["n"], bus); p.wire(source["p"], ground, [(-340, 340)])
    p.wire(bus, capacitor["+"], [(-160, -80), (-160, 120)])
    p.wire(capacitor["-"], ground, [(160, 120)])
    p.wire(bus, brake["+"], [(280, -80), (280, 107)])
    p.wire(brake["-"], ground, [(620, 120), (620, 340)])
    gate = p.recorded("External brake gate", False, [(.01, True), (.03, False), (.04, True)], 0, -220)
    p.wire(gate, brake["g"], [(240, -220), (240, 133)])
    p.plot("DC voltage and brake command", [bus, gate], 840, -100,
           routes=[[(-340, -320), (720, -320), (720, -111)], [(80, -220), (700, -220), (700, -89)]])
    return "\n".join(p.body() + dc.definition() + resistor.definition()) + "\n"


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    for key, body in [("precharge", precharge()), ("dc-link", dc_link()), ("switched-resistor", switched_resistor())]:
        (root / "library/converters" / (key + ".pds")).write_text(fragment(key, body), encoding="utf-8")
    for name, create in [("precharge-discharge", charging), ("braking-chopper", braking)]:
        (root / "examples" / (name + ".pds")).write_text(create(), encoding="utf-8")
