"""Generate editable transistor library fragments; no runtime Python dependency."""
from pathlib import Path
from generate_converter_examples import Diagram, quoted


def transistor(mosfet):
    name = "MOSFET" if mosfet else "IGBT"
    body = Diagram("library/" + name, name)
    body.schema = 12
    channel = body.component("Channel", "S" if mosfet else "IGBT", 0, 0, 0)
    diode = body.component("Body diode" if mosfet else "Antiparallel diode", "D", 0, 0, 180, turns=2)
    body.wire(channel["p"], diode["n"], [(-140, 0), (-140, 180)])
    body.wire(channel["n"], diode["p"], [(140, 0), (140, 180)])
    body.port("D" if mosfet else "C", channel["p"])
    body.port("S" if mosfet else "E", channel["n"])
    body.port("G", channel["gate"], gate=True)
    for obj in [channel, diode]:
        body.lines.append('semiconductor {} 1 0.01 1000000 0.7'.format(quoted(obj["p"][0])))
    body.lines.append('x-label {} "value" 45 0 0 0'.format(quoted(channel["p"][0])))
    for title, field, unit, value, obj in [
        ("Ron", "ron", "Ohm", .01, channel),
        ("Roff", "roff", "Ohm", 1e6, channel),
        *(([("Vf", "forward_voltage", "V", .7, channel)]) if not mosfet else []),
        ("Diode Ron", "ron", "Ohm", .01, diode),
        ("Diode Roff", "roff", "Ohm", 1e6, diode),
        ("Diode Vf", "forward_voltage", "V", .7, diode),
    ]:
        body.ports.append('public_parameter {} {} {} {} {} {}'.format(
            quoted(body.uuid("parameter/" + title)), quoted(title), quoted(unit),
            quoted(obj["p"][0]), quoted(field), value))
    root = Diagram("library/" + name + "/fragment", name)
    root.schema = 12
    root.instance(name, body, 0, 0)
    return "\n".join(root.body() + body.definition()) + "\n"


if __name__ == "__main__":
    target = Path(__file__).resolve().parents[1] / "library" / "semiconductors"
    target.mkdir(parents=True, exist_ok=True)
    for name, mosfet in [("mosfet", True), ("igbt", False)]:
        (target / (name + ".pds")).write_text(transistor(mosfet), encoding="utf-8")
