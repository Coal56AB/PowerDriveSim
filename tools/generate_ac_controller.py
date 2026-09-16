"""Antiparallel thyristor AC regulator with externally supplied firing pulses."""
from pathlib import Path
from generate_converter_examples import Diagram, quoted


def controller():
    p = Diagram("library/ac-voltage-controller", "AC voltage controller")
    p.schema = 12
    source = p.node("IN", -240, 0)
    output = p.node("OUT", 240, 0)
    positive = p.component("T+", "T", 0, 0, -100)
    negative = p.component("T-", "T", 0, 0, 100, turns=2)
    p.wire(source, positive["p"], [(-240, -100)])
    p.wire(positive["n"], output, [(240, -100)])
    p.wire(output, negative["p"], [(240, 100)])
    p.wire(negative["n"], source, [(-240, 100)])
    p.port("IN", source); p.port("OUT", output)
    p.port("g+", positive["gate"], gate=True)
    p.port("g-", negative["gate"], gate=True)
    for sign, terminal in [("+", positive), ("-", negative)]:
        p.lines.append('semiconductor {} 1 0.01 1000000 0.7'.format(quoted(terminal["p"][0])))
        p.lines.append('x-label {} "value" -50 0 0 0'.format(quoted(terminal["p"][0])))
        p.parameter("Ron T" + sign, terminal["p"], "ron", "Ohm", .01)
        p.parameter("Vf T" + sign, terminal["p"], "forward_voltage", "V", .7)
    return p


def fragment(body):
    p = Diagram("library/ac-voltage-controller/fragment", body.name)
    p.schema = 12
    p.instance(body.name, body, 0, 0)
    return "\n".join(p.body() + body.definition()) + "\n"


def example(body):
    p = Diagram("ac-voltage-controller/example", "AC voltage controller")
    p.schema = 12; p.profile = "0.04 0.00001 Trapezoidal"
    block = p.instance(body.name, body, 0, 0)
    source = p.component("Supply", "V", 100, -340, 160, turns=1)
    p.lines.append('source {} 1 0 50 0 0 0.5 0'.format(quoted(source["p"][0])))
    ground = p.node("Neutral", 0, 360, ground=True)
    current = p.component("Iload", "IP", 0, 260, 0)
    load = p.component("Load", "R", 10, 480, 160, turns=1)
    output = p.node("Uload", 480, 0)
    p.wire(source["p"], block["IN"], [(-340, -26)])
    p.wire(source["n"], ground, [(-340, 360)])
    p.wire(block["OUT"], current["p"])
    p.wire(current["n"], output); p.wire(output, load["p"])
    p.wire(load["n"], ground, [(480, 360)])
    for label, delay, y, port in [("Positive firing", .0025, -260, "g+"),
                                 ("Negative firing", .0125, -160, "g-")]:
        gate = p.uuid(label)
        p.lines.append('pwm {} {} -340 {} 50 0.005 {}'.format(quoted(gate), quoted(label), y, delay))
        x = -180 if port == "g+" else -150
        p.wire((gate, "out"), block[port], [(x, y), (x, 0 if port == "g+" else 26)])
    p.plot("AC load voltage and current", [output, current["out"]], 820, 60,
           routes=[[(660, 0), (660, 49)], [(260, -100), (700, -100), (700, 71)]])
    return "\n".join(p.body() + body.definition()) + "\n"


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    body = controller()
    (root / "library/converters/ac-voltage-controller.pds").write_text(fragment(body), encoding="utf-8")
    (root / "examples/ac-voltage-controller.pds").write_text(example(body), encoding="utf-8")
