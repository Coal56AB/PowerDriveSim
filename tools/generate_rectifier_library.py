"""Generate editable diode bridges. The application reads the generated PDS files."""
from pathlib import Path
from generate_converter_examples import Diagram, quoted
import math


def bridge(three_phase):
    key = "diode-bridge-3p" if three_phase else "diode-bridge-1p"
    body = Diagram("library/" + key, "Three-phase diode bridge" if three_phase else "Diode bridge")
    body.schema = 12
    phases = "ABC" if three_phase else "AB"
    positive = body.node("DC+", -360, -240)
    negative = body.node("DC-", -360, 240)
    terminals = []
    for index, phase in enumerate(phases):
        x = (index - (len(phases) - 1) / 2) * 280
        ac = body.node(phase, x, 0)
        upper = body.component("D" + phase + "+", "D", 0, x, -120, turns=3)
        lower = body.component("D" + phase + "-", "D", 0, x, 120, turns=3)
        body.wire(ac, upper["p"])
        body.wire(upper["n"], positive, [(x, -240)])
        body.wire(negative, lower["p"], [(x, 240)])
        body.wire(lower["n"], ac)
        terminals.append(ac)
    # Alternate conserving port positions: AC inputs left, DC outputs right.
    body.port("A", terminals[0])
    body.port("DC+", positive)
    body.port("B", terminals[1])
    body.port("DC-", negative)
    if three_phase:
        body.port("C", terminals[2])
    return key, body


def fragment(three_phase):
    key, body = bridge(three_phase)
    root = Diagram("library/" + key + "/fragment", body.name)
    root.schema = 12
    root.instance(body.name, body, 0, 0)
    return "\n".join(root.body() + body.definition()) + "\n"


def example(three_phase):
    key, body = bridge(three_phase)
    p = Diagram(key + "/example", body.name)
    p.schema = 12
    p.profile = "0.04 0.00001 Trapezoidal"
    block = p.instance(body.name, body, 200, 0)
    ground = p.node("Neutral", -380, 400, ground=True)
    for index, phase in enumerate("ABC" if three_phase else "A"):
        x = -560 + index * 180
        source = p.component("V" + phase, "V", 100, x, 240, turns=1)
        angle = [0, -2 * math.pi / 3, 2 * math.pi / 3][index]
        p.lines.append('source {} 1 0 50 {} 0 0.5 0'.format(quoted(source["p"][0]), angle))
        y = -140 + index * 60
        p.wire(source["p"], block[phase], [(x, y), (20 + index * 20, y)])
        p.wire(source["n"], ground, [(x, 400)])
    if not three_phase:
        p.wire(ground, block["B"], [(20, 400)])
    load = p.component("Load", "R", 10, 660, 120, turns=1)
    voltage = p.component("Udc", "VP", 0, 460, 120, turns=1)
    current = p.component("Idc", "IP", 0, 580, -100)
    positive = p.node("DC+", 460, -100)
    negative = p.node("DC-", 460, 320)
    p.wire(block["DC+"], positive, [(360, -13), (360, -100)])
    p.wire(block["DC-"], negative, [(340, 13), (340, 320)])
    p.wire(positive, voltage["p"])
    p.wire(voltage["n"], negative)
    p.wire(positive, current["p"])
    p.wire(current["n"], load["p"], [(660, -100)])
    p.wire(load["n"], negative, [(660, 320)])
    p.plot("DC voltage and current", [voltage["out"], current["out"]], 960, 80,
           routes=[[(520, 120), (520, 220), (780, 220), (780, 69)],
                   [(820, -100), (820, 91)]])
    return "\n".join(p.body() + body.definition()) + "\n"


if __name__ == "__main__":
    folder = Path(__file__).resolve().parents[1] / "library" / "converters"
    folder.mkdir(parents=True, exist_ok=True)
    for three_phase in [False, True]:
        key, _ = bridge(three_phase)
        (folder / (key + ".pds")).write_text(fragment(three_phase), encoding="utf-8")
        (folder.parents[1] / "examples" / (key + ".pds")).write_text(example(three_phase), encoding="utf-8")
