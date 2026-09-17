#!/usr/bin/env python3
"""Turn Doxygen's XML into the reference pages of the MkDocs site.

The public headers carry `///` comments for IDE completion; this renders
the same text as one markdown page per header, so the reference and the
tooltips can never disagree. It reads only what Doxygen extracted, adds
nothing, and links entities to each other by Doxygen's own refids.

    tools/doxygen-to-md.py --xml build/doxygen/xml --out docs/reference
"""
import argparse
import html
import pathlib
import re
import xml.etree.ElementTree as ET

# Everything the library does not promise to keep. Doxygen extracts them
# because the headers must compile; the reference does not show them.
PRIVATE_NS = ("blake3pp::detail", "blake3pp::kern", "blake3pp::ex", "stdexec")

# The order headers are presented in: the path a reader takes, not
# alphabetical. Anything not listed still appears, after these.
HEADER_ORDER = [
    "blake3pp.hpp", "core.hpp", "io.hpp",
    "parallel.hpp", "parallel_io.hpp", "parallel_backend.hpp", "dispatch.hpp",
]

# The order sections appear in, most-used first; anything unlisted follows.
SECTION_ORDER = ["Types", "Constants", "Member functions", "Static member functions",
                 "Non-member functions", "Data members", "Functions",
                 "Enumerations", "Macros", "Related"]

SECTION_TITLES = {
    "public-type": "Types",
    "public-attrib": "Data members",
    "public-static-attrib": "Constants",
    "public-func": "Member functions",
    "public-static-func": "Static member functions",
    "friend": "Non-member functions",
    "func": "Functions",
    "var": "Constants",
    "typedef": "Types",
    "enum": "Enumerations",
    "define": "Macros",
    "related": "Related",
}


def text_of(node) -> str:
    """Every character under a node, tags dropped."""
    return "".join(node.itertext()) if node is not None else ""


def squeeze(s: str) -> str:
    return re.sub(r"\s+", " ", s).strip()


def cpp(s: str) -> str:
    """Doxygen spells templates `array< T, N >`; C++ readers do not."""
    return re.sub(r"\s+>", ">", re.sub(r"<\s+", "<", squeeze(s)))


def slug(qualified: str) -> str:
    """A stable anchor from a qualified name, independent of heading text."""
    s = re.sub(r"[^A-Za-z0-9]+", "-", qualified).strip("-").lower()
    return s or "x"


class Renderer:
    """Doxygen description nodes to markdown.

    Carries the refid map so a `@ref` or an auto-linked name becomes a
    real link; entities that were filtered out render as plain code.
    """

    def __init__(self, links: dict):
        self.links = links

    def flow(self, node) -> str:
        parts = []
        if node.text:
            parts.append(node.text)
        for child in node:
            parts.append(self.element(child))
            if child.tail:
                parts.append(child.tail)
        return "".join(parts)

    def element(self, e) -> str:
        t = e.tag
        if t == "para":
            return self.flow(e).strip() + "\n\n"
        if t == "computeroutput":
            return f"`{cpp(text_of(e))}`"
        if t == "emphasis":
            return f"*{self.flow(e).strip()}*"
        if t in ("bold", "strong"):
            return f"**{self.flow(e).strip()}**"
        if t == "ref":
            label = squeeze(text_of(e))
            target = self.links.get(e.get("refid", ""))
            return f"[`{label}`]({target})" if target else f"`{label}`"
        if t == "ulink":
            return f"[{self.flow(e).strip()}]({e.get('url', '')})"
        if t == "itemizedlist":
            return "\n" + "".join(f"- {self.item(li)}\n" for li in e) + "\n"
        if t == "orderedlist":
            return "\n" + "".join(f"{i}. {self.item(li)}\n"
                                  for i, li in enumerate(e, 1)) + "\n"
        if t in ("programlisting", "verbatim"):
            if t == "verbatim":
                body, lang = text_of(e), ""
            else:
                body = "\n".join(self.codeline(cl) for cl in e.findall("codeline"))
                # @code{.unparsed} and friends are not C++; do not pretend.
                ext = (e.get("filename") or ".cpp").lstrip(".")
                lang = "cpp" if ext in ("cpp", "cc", "h", "hpp", "cxx") else ""
            return f"\n```{lang}\n{body.strip()}\n```\n\n"
        if t == "linebreak":
            return "\n"
        if t == "sp":
            return " "
        if t == "ndash":
            return "--"
        if t == "mdash":
            return "---"
        if t in ("parameterlist", "simplesect"):
            return ""          # lifted out before rendering
        if t in ("parblock",):
            return self.flow(e)
        return self.flow(e)

    def codeline(self, cl) -> str:
        """One line of a listing. Doxygen spells runs of spaces as <sp/>."""
        parts = []

        def walk(node):
            if node.text:
                parts.append(node.text)
            for child in node:
                if child.tag == "sp":
                    parts.append(" " * int(child.get("value", 1)))
                else:
                    walk(child)
                if child.tail:
                    parts.append(child.tail)

        walk(cl)
        return "".join(parts)

    def item(self, li) -> str:
        """A list item, flattened: markdown wants one line per bullet."""
        return squeeze(self.flow(li))

    def describe(self, brief, detailed) -> tuple:
        """Render (prose, params, returns, notes) from the two description nodes."""
        params, returns, notes = [], [], []
        for node in (brief, detailed):
            if node is None:
                continue
            for pl in node.iter("parameterlist"):
                if pl.get("kind") not in ("param", "templateparam"):
                    continue
                for item in pl.findall("parameteritem"):
                    names = [squeeze(text_of(n))
                             for n in item.iter("parametername")]
                    desc = item.find("parameterdescription")
                    params.append((", ".join(names),
                                   squeeze(self.flow(desc)) if desc is not None else ""))
            for ss in node.iter("simplesect"):
                kind = ss.get("kind")
                body = squeeze(self.flow(ss))
                if not body:
                    continue
                if kind == "return":
                    returns.append(body)
                elif kind in ("note", "warning", "attention", "see", "remark", "par"):
                    notes.append((kind, body))
        prose = "\n".join(x for x in (self.flow(brief).strip() if brief is not None else "",
                                      self.flow(detailed).strip() if detailed is not None else "")
                          if x)
        return prose.strip(), params, returns, notes

    def block(self, brief, detailed, level: int) -> str:
        """The full documentation body for one entity."""
        prose, params, returns, notes = self.describe(brief, detailed)
        out = []
        if prose:
            out.append(prose + "\n")
        if params:
            out.append("| Parameter | |\n| --- | --- |")
            out += [f"| `{n}` | {d} |" for n, d in params]
            out.append("")
        for r in returns:
            out.append(f"**Returns** {r}\n")
        for kind, body in notes:
            label = {"see": "See also", "par": "Note"}.get(kind, kind.capitalize())
            out.append(f'!!! note "{label}"\n\n    {body}\n')
        return "\n".join(out)


def signature(m) -> str:
    """The declaration as a C++ reader expects to see it."""
    tpl = m.find("templateparamlist")
    prefix = ""
    if tpl is not None:
        args = ", ".join(cpp(text_of(p)) for p in tpl.findall("param"))
        prefix = f"template <{args}>\n"
    # Doxygen keeps these out of <type>, but they are part of the contract
    # a caller reads. `inline` is not: it is on almost every header entity.
    quals = "".join(f"{q} " for q in ("static", "explicit", "constexpr", "consteval")
                    if m.get(q) == "yes")
    if m.get("nodiscard") == "yes":
        quals = "[[nodiscard]] " + quals
    typ = cpp(text_of(m.find("type")))
    name = squeeze(text_of(m.find("name")))
    args = cpp(text_of(m.find("argsstring")))
    kind = m.get("kind")
    if kind == "typedef":
        return f"{prefix}using {name} = {typ};"
    prefix += quals
    if kind == "enum":
        values = [squeeze(text_of(v.find("name"))) for v in m.findall("enumvalue")]
        body = ",\n    ".join(values)
        return f"enum class {name} {{\n    {body}\n}};"
    if kind == "define":
        return f"#define {name}"
    head = f"{typ} {name}".strip()
    init = cpp(text_of(m.find("initializer")))
    if kind == "variable" and init:
        return f"{prefix}{head} {init};"
    return f"{prefix}{head}{args};"


def is_private(name: str) -> bool:
    return any(name.startswith(ns + "::") or name == ns for ns in PRIVATE_NS)


def load(xml: pathlib.Path):
    """Every compound, indexed by refid."""
    compounds = {}
    for f in sorted(xml.glob("*.xml")):
        if f.name in ("index.xml", "Doxyfile.xml"):
            continue
        try:
            cd = ET.parse(f).getroot().find("compounddef")
        except ET.ParseError:
            continue
        if cd is not None:
            compounds[cd.get("id")] = cd
    return compounds


def members(cd, include_private=False):
    """(section kind, memberdef) pairs of a compound, private sections dropped."""
    for sec in cd.findall("sectiondef"):
        kind = sec.get("kind", "")
        if kind.startswith(("private", "protected")) and not include_private:
            continue
        for m in sec.findall("memberdef"):
            if m.get("prot") in ("private", "protected") and not include_private:
                continue
            yield kind, m


def in_order(grouped: dict):
    """Sections in SECTION_ORDER, then anything unforeseen, alphabetically."""
    rank = {t: i for i, t in enumerate(SECTION_ORDER)}
    return sorted(grouped.items(), key=lambda kv: (rank.get(kv[0], len(rank)), kv[0]))


def header_of(node) -> str:
    """Which public header an entity was declared in."""
    loc = node.find("location")
    return pathlib.Path(loc.get("file", "")).name if loc is not None else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xml", type=pathlib.Path, required=True)
    ap.add_argument("--out", type=pathlib.Path, required=True)
    a = ap.parse_args()

    compounds = load(a.xml)
    files = {cd.find("compoundname").text: cd
             for cd in compounds.values() if cd.get("kind") == "file"}
    order = [h for h in HEADER_ORDER if h in files]
    order += sorted(h for h in files if h not in order)

    # Namespace-scope entities live in the namespace compound, not the file
    # compound, so both are collected and bucketed by where they were
    # declared. That is what makes a page mirror one header.
    classes_of = {h: [] for h in order}
    frees_of = {h: [] for h in order}
    seen = set()
    for cd in compounds.values():
        kind = cd.get("kind")
        name = cd.find("compoundname").text or ""
        if kind in ("class", "struct", "concept", "union") and not is_private(name):
            h = header_of(cd)
            if h in classes_of and cd.get("id") not in seen:
                classes_of[h].append(cd)
                seen.add(cd.get("id"))
        if kind in ("namespace", "file") and not is_private(name):
            for sec_kind, m in members(cd):
                q = squeeze(text_of(m.find("qualifiedname"))) or \
                    squeeze(text_of(m.find("name")))
                if is_private(q) or m.get("id") in seen:
                    continue
                h = header_of(m)
                if h in frees_of:
                    frees_of[h].append((sec_kind, m))
                    seen.add(m.get("id"))
    for h in order:
        classes_of[h].sort(key=lambda c: c.find("compoundname").text)

    # Where every entity lands, built before any rendering so a
    # description can link forward as well as back. Anchors are assigned
    # here and only read later, so the heading and the link agree even
    # where two members reduce to one slug (operator== and operator<=>).
    links, anchors = {}, {}
    for header in order:
        page = pathlib.Path(header).stem + ".md"
        used = set()

        def anchor(node, qualified):
            base = slug(qualified)
            name = base
            n = 2
            while name in used:
                name, n = f"{base}-{n}", n + 1
            used.add(name)
            anchors[node.get("id")] = name
            links[node.get("id")] = f"{page}#{name}"
            return name

        links[files[header].get("id")] = page
        for klass in classes_of[header]:
            qname = klass.find("compoundname").text
            anchor(klass, qname)
            for _, m in members(klass):
                anchor(m, squeeze(text_of(m.find("qualifiedname"))) or
                       f"{qname}::{squeeze(text_of(m.find('name')))}")
        for _, m in frees_of[header]:
            anchor(m, squeeze(text_of(m.find("qualifiedname"))) or
                   squeeze(text_of(m.find("name"))))

    r = Renderer(links)
    a.out.mkdir(parents=True, exist_ok=True)
    written = []

    for header in order:
        cd = files[header]
        page = pathlib.Path(header).stem + ".md"
        out = [f"# `<blake3pp/{header}>`\n"]
        body = r.block(cd.find("briefdescription"), cd.find("detaileddescription"), 1)
        if body.strip():
            out.append(body)
        brief = squeeze(text_of(cd.find("briefdescription"))) or \
            squeeze(text_of(cd.find("detaileddescription")))

        for klass in classes_of[header]:
            qname = klass.find("compoundname").text
            kind = klass.get("kind")
            short = qname.split("::")[-1]
            out.append(f"\n## {short} {{ #{anchors[klass.get('id')]} }}\n")
            out.append(f"```cpp\n{kind} {qname};\n```\n")
            out.append(r.block(klass.find("briefdescription"),
                               klass.find("detaileddescription"), 2))
            grouped = {}
            for sec_kind, m in members(klass):
                grouped.setdefault(SECTION_TITLES.get(sec_kind, sec_kind), []).append(m)
            for title, ms in in_order(grouped):
                out.append(f"\n### {title}\n")
                for m in ms:
                    name = squeeze(text_of(m.find("name")))
                    out.append(f"\n#### {name} {{ #{anchors[m.get('id')]} }}\n")
                    out.append(f"```cpp\n{signature(m)}\n```\n")
                    out.append(r.block(m.find("briefdescription"),
                                       m.find("detaileddescription"), 4))

        grouped = {}
        for sec_kind, m in frees_of[header]:
            grouped.setdefault(SECTION_TITLES.get(sec_kind, sec_kind), []).append(m)
        for title, ms in in_order(grouped):
            out.append(f"\n## {title}\n")
            for m in ms:
                name = squeeze(text_of(m.find("name")))
                out.append(f"\n### {name} {{ #{anchors[m.get('id')]} }}\n")
                out.append(f"```cpp\n{signature(m)}\n```\n")
                out.append(r.block(m.find("briefdescription"),
                                   m.find("detaileddescription"), 3))

        (a.out / page).write_text("\n".join(out).rstrip() + "\n")
        written.append((header, page, brief))

    index = ["# Reference\n",
             "Every declaration in the public headers, generated from the `///` "
             "comments in them, so this page says exactly what your editor says.\n",
             "For the guided version, with the examples that go with it, read "
             "[The API](../api.md) instead.\n",
             "| Header | |", "| --- | --- |"]
    index += [f"| [`<blake3pp/{h}>`]({p}) | {b} |" for h, p, b in written]
    (a.out / "index.md").write_text("\n".join(index) + "\n")
    print(f"doxygen-to-md: {len(written)} headers -> {a.out}")


if __name__ == "__main__":
    main()
