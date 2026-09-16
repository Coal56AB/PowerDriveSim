"""Dual three-phase inverter; winding ends remain separate external terminals."""
from pathlib import Path
from generate_converter_examples import Diagram, switching_states
from generate_bridge_library import half_bridge


def inverter():
    leg = half_bridge()
    p = Diagram("library/open-end-winding/inverter", "Three-phase bridge")
    p.schema = 12
    positive = p.node("DC+", -280, -240)
    negative = p.node("DC-", -280, 240)
    phases = {}
    for index, phase in enumerate("ABC"):
        x = index * 340
        ports = p.instance("Leg " + phase, leg, x, 0)
        p.wire(positive, ports["DC+"], [(x - 160, -240), (x - 160, -39)])
        p.wire(ports["DC-"], negative, [(x - 140, -13), (x - 140, 240)])
        phases[phase] = ports
    for name, terminal in [("DC+", positive), ("A", phases["A"]["AC"]),
                           ("DC-", negative), ("B", phases["B"]["AC"]),
                           ("C", phases["C"]["AC"])]:
        p.port(name, terminal)
    for phase, ports in phases.items():
        for level in "HL": p.port("g" + phase + level, ports["g" + level], gate=True)
    return p, leg


def topology():
    bridge, leg = inverter()
    p = Diagram("library/open-end-winding", "Open-end winding inverter")
    p.schema = 12
    first = p.instance("Inverter 1", bridge, 0, 0)
    second = p.instance("Inverter 2", bridge, 400, 0)
    for phase in "ABC":
        p.port(phase + "1", first[phase]); p.port(phase + "2", second[phase])
    for sign in ["+", "-"]:
        p.port("DC1" + sign, first["DC" + sign]); p.port("DC2" + sign, second["DC" + sign])
    for number, ports in [(1, first), (2, second)]:
        for phase in "ABC":
            for level in "HL": p.port("g" + str(number) + phase + level, ports["g" + phase + level], gate=True)
    return p, bridge, leg


def fragment(bodies):
    p = Diagram("library/open-end-winding/fragment", bodies[0].name)
    p.schema = 12; p.instance(bodies[0].name, bodies[0], 0, 0)
    lines = p.body()
    for body in bodies: lines += body.definition()
    return "\n".join(lines) + "\n"


def example(bodies):
    p = Diagram("open-end-winding/example", "Open-end winding with independent RL phases")
    p.schema = 12; p.profile = "0.006 0.000002 Trapezoidal"
    # Both bridges share an explicit 24 V DC bus in this example.
    block = p.instance(bodies[0].name, bodies[0], 0, 0)
    source = p.component("Supply", "V", 24, -650, -100, turns=1)
    positive = p.node("DC+", -650, -300)
    ground = p.node("DC-", -650, 280, ground=True)
    p.wire(source["p"], positive); p.wire(source["n"], ground)
    for number, x in [(1, -240), (2, 240)]:
        p.wire(positive, block["DC" + str(number) + "+"], [(x, -300), (x, -130 if number == 1 else 26)])
        p.wire(ground, block["DC" + str(number) + "-"], [(x - 20, 280), (x - 20, -104 if number == 1 else 52)])
    bank = Diagram("open-end-winding/gates", "Recorded dual inverter gates")
    bank.schema = 12
    states = switching_states(3)
    for number in [1, 2]:
        for index, phase in enumerate("ABC"):
            for level in "HL":
                name = "g" + str(number) + phase + level
                # The zero state alternates both-low and both-high conduction.
                upper = [(state[index] == (1 if number == 1 else -1)) or
                         (state[index] == 0 and interval % 2 == 1)
                         for interval, state in enumerate(states)]
                values = upper if level == "H" else [not value for value in upper]
                terminal = bank.pattern(name, values, (number - 1) * 360 + (level == "L") * 140, index * 140)
                bank.port(name, terminal, gate=True, output=True)
    gates = p.instance(bank.name, bank, -520, 65)
    for index, name in enumerate(bank.port_ids):
        p.wire(gates[name], block[name])
    voltage_outputs, current_outputs = [], []
    for index, phase in enumerate("ABC"):
        y = 360 + index * 210
        resistor = p.component("R" + phase, "R", 10, -200, y)
        inductor = p.component("L" + phase, "L", .01, 0, y)
        current = p.component("I" + phase, "IP", 0, 200, y)
        voltage = p.component("U" + phase, "VP", 0, -180, y + 90)
        left, right = -700 - index * 40, 440 + index * 40
        p.wire(block[phase + "1"], resistor["p"], [(left, -208 + index * 26), (left, y)])
        p.wire(resistor["n"], inductor["p"]); p.wire(inductor["n"], current["p"])
        p.wire(current["n"], block[phase + "2"], [(right, y), (right, -52 + index * 26)])
        p.wire(block[phase + "1"], voltage["p"], [(left, -208 + index * 26), (left, y + 90)])
        p.wire(voltage["n"], block[phase + "2"], [(right, y + 90), (right, -52 + index * 26)])
        voltage_outputs.append(voltage["out"]); current_outputs.append(current["out"])
    p.plot("Winding voltages", voltage_outputs, 760, 410)
    p.plot("Winding currents", current_outputs, 760, 750)
    lines = p.body()
    for body in list(bodies) + [bank]: lines += body.definition()
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    bodies = topology()
    (root / "library/converters/open-end-winding.pds").write_text(fragment(bodies), encoding="utf-8")
    (root / "examples/open-end-winding.pds").write_text(example(bodies), encoding="utf-8")
