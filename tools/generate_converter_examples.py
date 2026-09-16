"""Rebuild the editable M1 converter examples using only Python's standard library.

The generated .pds files are the shipped examples; this script is not used at runtime.
"""
import json
from pathlib import Path
import uuid


def quoted(value):
    return json.dumps(str(value), ensure_ascii=False)


class Diagram:
    def __init__(self, key, name):
        self.key, self.name = key, name
        self.id = self.uuid("diagram")
        self.lines, self.ports, self.port_ids = [], [], {}
        self.schema = 7
        self.profile = "0.006 0.00001 BackwardEuler"

    def uuid(self, key):
        return str(uuid.uuid5(uuid.NAMESPACE_URL, "powerdrivesim/examples/" + self.key + "/" + key))

    def node(self, name, x, y, ground=False):
        ident = self.uuid("node/" + name)
        self.lines.append("node {} {} {} {} {}".format(quoted(ident), quoted(name), int(ground), x, y))
        return ident, "node"

    def component(self, name, kind, value, x, y, initial=0, turns=0):
        ident = self.uuid("component/" + name)
        self.lines.append('component {} {} {} "" "" {} {} {} {} 0'.format(
            quoted(ident), quoted(name), kind, value, initial, x, y))
        if turns:
            self.lines.append("orientation {} {} 0".format(quoted(ident), turns))
        return {port: (ident, port) for port in ("p", "n", "gate", "out")}

    def wire(self, a, b, bends=()):
        ident = self.uuid("wire/" + "/".join(a + b))
        self.lines.append("wire {} {} {} {} {} {}{}".format(
            quoted(ident), *(quoted(v) for v in a + b), len(bends),
            "".join(" {} {}".format(x, y) for x, y in bends)))

    def port(self, name, terminal, gate=False, output=False):
        ident = self.uuid("port/" + name)
        self.port_ids[name] = ident
        self.ports.append("public_port {} {} {} {} {} {}".format(
            quoted(ident), quoted(name), quoted(terminal[0]), quoted(terminal[1]),
            1 if gate else 0, 2 if output else 1 if gate else 0))

    def instance(self, name, definition, x, y, parameters=None):
        ident = self.uuid("instance/" + name)
        values = parameters or {}
        overrides = "".join(" {} {}".format(quoted(definition.uuid("parameter/" + key)), value)
                            for key, value in values.items())
        self.lines.append("instance {} {} {} {} {} {}{}".format(
            quoted(ident), quoted(name), quoted(definition.id), x, y, len(values), overrides))
        return {name: (ident, port) for name, port in definition.port_ids.items()}

    def parameter(self, name, terminal, field, unit, value):
        self.ports.append('public_parameter {} {} {} {} {} {}'.format(
            quoted(self.uuid("parameter/" + name)), quoted(name), quoted(unit),
            quoted(terminal[0]), quoted(field), value))

    def pattern(self, name, states, x, y):
        ident = self.uuid("pattern/" + name)
        self.lines.append("pattern {} {} {} {} {}".format(
            quoted(ident), quoted(name), x, y, int(states[0])))
        for index in range(1, len(states)):
            if states[index] != states[index - 1]:
                self.lines.append("event {} {} {}".format(index * .001, quoted(ident), int(states[index])))
        return ident, "out"

    def recorded(self, name, initial, events, x, y):
        ident = self.uuid("pattern/" + name)
        self.lines.append("pattern {} {} {} {} {}".format(
            quoted(ident), quoted(name), x, y, int(initial)))
        for time, value in events:
            self.lines.append("event {} {} {}".format(time, quoted(ident), int(value)))
        return ident, "out"

    def plot(self, name, terminals, x, y, routes=None):
        ident = self.uuid("plot/" + name)
        self.lines.append("plot {} {} {} {} {} 0 -1 -1 -1".format(
            quoted(ident), quoted(name), x, y, len(terminals)))
        for index, terminal in enumerate(terminals, 1):
            self.wire(terminal, (ident, "in" + str(index)), routes[index - 1] if routes else ())

    def body(self):
        return ["PowerDriveSim " + str(self.schema), "project {} {}".format(quoted(self.id), quoted(self.name)),
                "profile " + self.profile, "nonlinear 64 1e-9 1e-12 1e-9",
                "wiring wires", "scope_enabled 0", "scopeview 0 -1 -1 -1"] + self.lines

    def definition(self):
        return ["definition {} {}".format(quoted(self.id), quoted(self.name))] + self.ports + [
            "body"] + self.body() + ["end_definition"]


def gates(levels, state):
    if levels == 2:
        return [state == 1, state == -1]
    return {1: [True, True, False, False], 0: [False, True, True, False],
            -1: [False, False, True, True]}[state]


def phase_leg(levels, phase, initial):
    count = 2 if levels == 2 else 4
    leg = Diagram("{}l/phase-{}".format(levels, phase), "Phase " + phase)
    positions = [(index - count / 2) * 360 for index in range(count + 1)]
    nodes = [leg.node("DC+" if index == 0 else "DC-" if index == count else
                      "phase" if index == count // 2 else "junction" + str(index), 0, y)
             for index, y in enumerate(positions)]
    neutral = leg.node("N", -340, 0) if levels == 3 else None
    initial_voltages = ([0, 600] if initial == 1 else [600, 0]) if levels == 2 else {
        1: [0, 0, 300, 300], 0: [300, 0, 0, 300], -1: [300, 300, 0, 0]}[initial]
    gate_ports = []
    for index in range(count):
        number = index + 1
        y = (positions[index] + positions[index + 1]) / 2
        switch = leg.component("S" + str(number), "S", 0, 0, y, turns=1)
        diode = leg.component("D" + str(number), "D", 0, 100, y, turns=3)
        resistor = leg.component("Rsn" + str(number), "R", 100, 240, y - 60, turns=1)
        capacitor = leg.component("Csn" + str(number), "C", 1e-8, 240, y + 60,
                                  initial=initial_voltages[index], turns=1)
        for terminal in (switch["p"], diode["n"], resistor["p"]):
            leg.wire(nodes[index], terminal)
        for terminal in (switch["n"], diode["p"], capacitor["n"]):
            leg.wire(nodes[index + 1], terminal)
        leg.wire(resistor["n"], capacitor["p"])
        gate_ports.append(switch["gate"])
    if levels == 3:
        upper = leg.component("Dclamp+", "D", 0, -180, -200, turns=3)
        lower = leg.component("Dclamp-", "D", 0, -180, 200, turns=3)
        leg.wire(neutral, upper["p"])
        leg.wire(upper["n"], nodes[1])
        leg.wire(nodes[3], lower["p"])
        leg.wire(lower["n"], neutral)
    leg.port("DC+", nodes[0])
    leg.port("phase", nodes[count // 2])
    leg.port("DC-", nodes[-1])
    if neutral:
        leg.port("N", neutral)
    for index, terminal in enumerate(gate_ports, 1):
        leg.port("g" + str(index), terminal, gate=True)
    return leg


def switching_states(levels):
    return ([[1, -1, -1], [1, 1, -1], [-1, 1, -1], [-1, 1, 1], [-1, -1, 1], [1, -1, 1]]
            if levels == 2 else
            [[1, 0, -1], [0, 1, -1], [-1, 1, 0], [-1, 0, 1], [0, -1, 1], [1, -1, 0]])


def converter_definition(levels):
    states = switching_states(levels)
    name = "2L VSI" if levels == 2 else "3L NPC"
    converter = Diagram(str(levels) + "l/converter", name)
    positive = converter.node("DC+", -360, -200)
    negative = converter.node("DC-", -360, 200)
    neutral = converter.node("N", -360, 0)
    # Visible capacitor ESR avoids an ideal voltage-source/capacitor constraint loop.
    # It is an explicit physical component, not a numerical stabilizer.
    for label, top, bottom, y in (("+", positive, neutral, -120), ("-", neutral, negative, 100)):
        esr = converter.component("ESR" + label, "R", .05, -220, y - 40, turns=1)
        capacitor = converter.component("Cdc" + label, "C", .001, -220, y + 80, initial=300, turns=1)
        converter.wire(top, esr["p"])
        converter.wire(esr["n"], capacitor["p"])
        converter.wire(capacitor["n"], bottom)
        converter.parameter("Cdc" + label, capacitor["p"], "value", "F", .001)
        converter.parameter("uCdc" + label + "(0)", capacitor["p"], "initial", "V", 300)
        converter.parameter("ESR" + label, esr["p"], "value", "Ohm", .05)
    definitions, legs = [], []
    for phase, letter in enumerate("ABC"):
        leg = phase_leg(levels, letter, states[0][phase])
        definitions.append(leg)
        terminals = converter.instance("Phase " + letter, leg, phase * 280 + 120, 0)
        legs.append(terminals)
        converter.wire(positive, terminals["DC+"])
        converter.wire(negative, terminals["DC-"])
        if levels == 3:
            converter.wire(neutral, terminals["N"])
    # Electrical ports alternate left/right; put rails on the left and phases on the right.
    for rail, terminal, phase in (("DC+", positive, 0), ("DC-", negative, 1), ("N", neutral, 2)):
        converter.port(rail, terminal)
        converter.port("ABC"[phase], legs[phase]["phase"])
    count = 2 if levels == 2 else 4
    for phase, letter in enumerate("ABC"):
        for index in range(count):
            converter.port(letter + str(index + 1), legs[phase]["g" + str(index + 1)], gate=True)
    return converter, definitions


def library_fragment(levels):
    converter, definitions = converter_definition(levels)
    root = Diagram(str(levels) + "l/library", converter.name)
    root.instance(converter.name, converter, 0, 0)
    lines = root.body() + converter.definition()
    for leg in definitions:
        lines += leg.definition()
    return "\n".join(lines) + "\n"


def example(levels):
    states = switching_states(levels)
    converter, definitions = converter_definition(levels)
    name = converter.name
    count = 2 if levels == 2 else 4
    gate_bank = Diagram(str(levels) + "l/recorded-gates", "Recorded gate sequence")
    for phase, letter in enumerate("ABC"):
        for index in range(count):
            assignments = [gates(levels, state[phase])[index] for state in states]
            pattern = gate_bank.pattern(letter + str(index + 1), assignments, index * 180, phase * 130)
            gate_bank.port(letter + str(index + 1), pattern, gate=True, output=True)
    root = Diagram(str(levels) + "l/project", name + " with RL load and recorded gates")
    block = root.instance(name, converter, 0, 0)
    external_gates = root.instance("Recorded gates", gate_bank, -420, 60)
    ground = root.node("N", -240, 300, ground=True)
    supply_p = root.component("Supply+", "V", 300, -240, -300, turns=1)
    supply_n = root.component("Supply-", "V", 300, -240, 180, turns=1)
    root.wire(supply_p["p"], block["DC+"])
    root.wire(supply_p["n"], ground)
    root.wire(ground, supply_n["p"])
    root.wire(supply_n["n"], block["DC-"])
    root.wire(ground, block["N"])
    voltages, currents = [], []
    for phase, letter in enumerate("ABC"):
        y = phase * 160 - 120
        voltage = root.component("U" + letter, "VP", 0, 230, y + 50)
        resistor = root.component("R" + letter, "R", 20, 300, y)
        inductor = root.component("L" + letter, "L", .02, 480, y)
        current = root.component("I" + letter, "IP", 0, 640, y)
        root.wire(block[letter], resistor["p"])
        root.wire(resistor["n"], inductor["p"])
        root.wire(inductor["n"], current["p"])
        root.wire(current["n"], ground)
        root.wire(block[letter], voltage["p"])
        root.wire(voltage["n"], ground)
        voltages.append(voltage["out"])
        currents.append(current["out"])
        for index in range(count):
            root.wire(external_gates[letter + str(index + 1)], block[letter + str(index + 1)])
    root.plot("Phase voltages", voltages, 870, -130)
    root.plot("Phase currents", currents, 870, 120)
    lines = root.body()
    for definition in [converter] + definitions + [gate_bank]:
        lines += definition.definition()
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    folder = Path(__file__).resolve().parents[1] / "examples"
    for levels, filename in ((2, "vsi-2l.pds"), (3, "npc-3l.pds")):
        (folder / filename).write_text(example(levels), encoding="utf-8")
        (folder.parent / "library/converters" / filename).write_text(library_fragment(levels), encoding="utf-8")
