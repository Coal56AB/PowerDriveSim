"""Rebuild the source waveform examples; generated projects have no Python dependency."""
from pathlib import Path
from generate_converter_examples import Diagram, quoted


def example(mode):
    p = Diagram("source/" + mode, "RC with " + mode + " voltage source")
    p.schema = 8
    p.profile = "0.05 0.00001 Trapezoidal"
    ground = p.node("ground", 0, 240, ground=True)
    supply = p.node("input", -240, 0)
    output = p.node("output", 240, 0)
    source = p.component("Source", "V", 2, -240, 100, turns=1)
    resistor = p.component("R", "R", 10, 0, 0)
    capacitor = p.component("C", "C", .001, 240, 100, turns=1)
    p.wire(supply, source["p"])
    p.wire(source["n"], ground)
    p.wire(supply, resistor["p"])
    p.wire(resistor["n"], output)
    p.wire(output, capacitor["p"])
    p.wire(capacitor["n"], ground)
    p.plot("Input and output", [supply, output], 520, 20,
           routes=[[(-240, -140), (440, -140), (440, 8)],
                   [(240, -80), (420, -80), (420, 32)]])
    for terminal in (supply, output):
        p.lines.append('x-label {} "name" 0 -65 0 0'.format(quoted(terminal[0])))
    kind = {"sine": 1, "pulse": 2, "table": 3}[mode]
    points = [(0, 0), (.01, 2), (.025, -1), (.04, 0)] if mode == "table" else []
    p.lines.append("source {} {} 0 50 0 0 0.4 {}{}".format(
        quoted(source["p"][0]), kind, len(points),
        "".join(" {} {}".format(t, value) for t, value in points)))
    return "\n".join(p.body()) + "\n"


if __name__ == "__main__":
    folder = Path(__file__).resolve().parents[1] / "examples"
    for mode in ("sine", "pulse", "table"):
        (folder / ("rc-" + mode + ".pds")).write_text(example(mode), encoding="utf-8")
