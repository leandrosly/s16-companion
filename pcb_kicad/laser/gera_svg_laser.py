"""
gera_svg_laser.py - transforma a camada de cobre de uma placa do KiCad num SVG pronto para o
LightBurn (modo Fill), para queimar a tinta onde o cobre tem que sair.

Uso (PowerShell, na pasta do projeto):
    pip install shapely                      # so na primeira vez
    python laser\\gera_svg_laser.py S16_placa.kicad_pcb
    python laser\\gera_svg_laser.py S16_placa.kicad_pcb --camada F.Cu --sem-espelho
    python laser\\gera_svg_laser.py S16_placa.kicad_pcb --compensa 0.1     # laser "come" 0,1 mm por lado
    python laser\\gera_svg_laser.py S16_placa.kicad_pcb --positivo --compensa -0.1   # fotorresiste (dry film):
                                                     # o laser marca o cobre, entao a compensacao e para dentro

Gera <placa>_laser_<camada>.svg na mesma pasta do script:
  * preto = queima (tira a tinta): a area da placa menos o cobre, mais um furinho de guia
    no centro de cada pad e via (para a broca centralizar);
  * um unico caminho fechado, preenchimento par/impar: no LightBurn e so usar Fill, sem Weld;
  * tamanho real em mm, com a origem no canto da placa;
  * espelhado por padrao (B.Cu e vista pelo lado do cobre). Use --sem-espelho para F.Cu.

Le do arquivo .kicad_pcb: pads (redondo, oval, retangular, retangular arredondado), trilhas,
vias, zonas preenchidas (aperte B no KiCad e salve antes!) e o contorno (Edge.Cuts).
"""
import argparse, math, os, re, sys

try:
    from shapely.geometry import Point, LineString, Polygon, box
    from shapely.ops import unary_union, polygonize
    from shapely import affinity
except ImportError:
    sys.exit("Falta o shapely. Rode:  pip install shapely")


# ---------- leitura do arquivo do KiCad (s-expressions) ----------
def parse(s):
    toks = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', s)
    st = [[]]
    for t in toks:
        if t == '(':
            st.append([])
        elif t == ')':
            x = st.pop(); st[-1].append(x)
        else:
            st[-1].append(t[1:-1] if t.startswith('"') else t)
    return st[0][0]

def find(n, k): return [c for c in n if isinstance(c, list) and c and c[0] == k]
def one(n, k):
    r = find(n, k); return r[0] if r else None
def xy(n): return float(n[1]), float(n[2])


def main():
    ap = argparse.ArgumentParser(description="KiCad -> SVG para gravar com laser (LightBurn, modo Fill)")
    ap.add_argument("pcb", help="arquivo .kicad_pcb")
    ap.add_argument("--camada", default="B.Cu", help="camada de cobre (padrao B.Cu)")
    ap.add_argument("--sem-espelho", action="store_true", help="nao espelhar (para F.Cu)")
    ap.add_argument("--furo", type=float, default=0.35, help="diametro do furinho de guia em mm (0 = sem)")
    ap.add_argument("--compensa", type=float, default=0.0,
                    help="engorda o cobre em mm de cada lado, para compensar o laser que queima alem da linha (ex.: 0.1)")
    ap.add_argument("--positivo", action="store_true",
                    help="inverte: preto = cobre (para fotorresiste/dry film, onde o laser marca o que FICA)")
    ap.add_argument("--saida", help="nome do SVG de saida")
    a = ap.parse_args()

    b = parse(open(a.pcb, encoding="utf-8").read())
    cam = a.camada
    cobre, furos = [], []

    # pads
    for f in find(b, "footprint"):
        at = one(f, "at"); fx, fy = xy(at); fr = math.radians(float(at[3])) if len(at) > 3 else 0
        for p in find(f, "pad"):
            camadas = one(p, "layers")[1:] if one(p, "layers") else []
            if not any(c in (cam, "*.Cu") for c in camadas):
                continue
            pa = one(p, "at"); px, py = xy(pa); prot = float(pa[3]) if len(pa) > 3 else 0
            x = fx + px * math.cos(fr) + py * math.sin(fr)
            y = fy - px * math.sin(fr) + py * math.cos(fr)
            sz = one(p, "size"); w, h = float(sz[1]), float(sz[2]); forma = p[3]
            if forma == "circle":
                g = Point(0, 0).buffer(w / 2, 64)
            elif forma == "oval":
                r = min(w, h) / 2
                g = LineString([(-(w / 2 - r), -(h / 2 - r)), (w / 2 - r, h / 2 - r)]).buffer(r, 64) if w != h \
                    else Point(0, 0).buffer(r, 64)
            elif forma == "roundrect":
                r = float(one(p, "roundrect_rratio")[1]) * min(w, h)
                g = box(-w / 2 + r, -h / 2 + r, w / 2 - r, h / 2 - r).buffer(r, 32)
            else:                                   # rect (e o resto vira retangulo)
                g = box(-w / 2, -h / 2, w / 2, h / 2)
            g = affinity.rotate(g, -prot, origin=(0, 0))
            cobre.append(affinity.translate(g, x, y))
            if one(p, "drill") and a.furo > 0:
                furos.append(Point(x, y).buffer(a.furo / 2, 32))

    # trilhas e arcos
    for s in find(b, "segment"):
        if one(s, "layer")[1] != cam: continue
        w = float(one(s, "width")[1])
        cobre.append(LineString([xy(one(s, "start")), xy(one(s, "end"))]).buffer(w / 2, 32))
    for s in find(b, "arc"):
        if one(s, "layer")[1] != cam: continue
        w = float(one(s, "width")[1])
        cobre.append(LineString([xy(one(s, "start")), xy(one(s, "mid")), xy(one(s, "end"))]).buffer(w / 2, 32))

    # vias (atravessam: tem cobre em todas as camadas)
    for v in find(b, "via"):
        x, y = xy(one(v, "at"))
        cobre.append(Point(x, y).buffer(float(one(v, "size")[1]) / 2, 64))
        if a.furo > 0: furos.append(Point(x, y).buffer(a.furo / 2, 32))

    # zonas preenchidas (plano de GND etc.)
    zonas = 0
    for z in find(b, "zone"):
        for fp in find(z, "filled_polygon"):
            if one(fp, "layer")[1] != cam: continue
            pts = [xy(q) for q in find(one(fp, "pts"), "xy")]
            cobre.append(Polygon(pts).buffer(0)); zonas += 1

    # contorno da placa (linhas, retangulos, arcos e circulos na Edge.Cuts)
    linhas = []
    for k in ("gr_line", "gr_rect", "gr_arc", "gr_circle", "gr_poly"):
        for g in find(b, k):
            if one(g, "layer")[1] != "Edge.Cuts": continue
            if k == "gr_line":
                linhas.append(LineString([xy(one(g, "start")), xy(one(g, "end"))]))
            elif k == "gr_rect":
                (x0, y0), (x1, y1) = xy(one(g, "start")), xy(one(g, "end"))
                linhas.append(box(min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)).exterior)
            elif k == "gr_arc":
                (x0, y0), (xm, ym), (x1, y1) = xy(one(g, "start")), xy(one(g, "mid")), xy(one(g, "end"))
                # arco por 3 pontos
                d = 2 * (x0 * (ym - y1) + xm * (y1 - y0) + x1 * (y0 - ym))
                cx = ((x0**2 + y0**2) * (ym - y1) + (xm**2 + ym**2) * (y1 - y0) + (x1**2 + y1**2) * (y0 - ym)) / d
                cy = ((x0**2 + y0**2) * (x1 - xm) + (xm**2 + ym**2) * (x0 - x1) + (x1**2 + y1**2) * (xm - x0)) / d
                r = math.hypot(x0 - cx, y0 - cy)
                a0, am, a1 = (math.atan2(y - cy, x - cx) for x, y in ((x0, y0), (xm, ym), (x1, y1)))
                def entre(t, s, e):
                    return (t - s) % (2 * math.pi) <= (e - s) % (2 * math.pi)
                if not entre(am, a0, a1): a0, a1 = a1, a0
                span = (a1 - a0) % (2 * math.pi); n = max(8, int(span / (2 * math.pi) * 128))
                linhas.append(LineString([(cx + r * math.cos(a0 + span * i / n), cy + r * math.sin(a0 + span * i / n))
                                          for i in range(n + 1)]))
            elif k == "gr_circle":
                c = xy(one(g, "center")); e = xy(one(g, "end"))
                linhas.append(Point(c).buffer(math.dist(c, e), 128).exterior)
            elif k == "gr_poly":
                pts = [xy(q) for q in find(one(g, "pts"), "xy")]
                linhas.append(LineString(pts + [pts[0]]))
    pecas = list(polygonize(unary_union(linhas)))
    if not pecas:
        sys.exit("Nao achei um contorno fechado na Edge.Cuts.")
    placa = max(pecas, key=lambda p: p.area)
    for p in pecas:                                   # recortes internos (furos de fixacao etc.)
        if p is not placa and placa.contains(p): placa = placa.difference(p)

    cobreU = unary_union(cobre)
    if a.compensa:
        cobreU = cobreU.buffer(a.compensa, 32, join_style=1)
    if a.positivo:      # preto = cobre que fica (menos os furinhos de guia)
        queima = cobreU.intersection(placa).difference(unary_union(furos)) if furos else cobreU.intersection(placa)
    else:               # preto = onde o cobre sai
        queima = unary_union([placa.difference(cobreU)] + [f.intersection(placa) for f in furos])
    x0, y0, x1, y1 = placa.bounds
    if not a.sem_espelho:
        queima = affinity.scale(queima, xfact=-1, yfact=1, origin=((x0 + x1) / 2, 0))
    queima = affinity.translate(queima, -x0, -y0)
    W, H = x1 - x0, y1 - y0

    def anel(c): return "M " + " L ".join("%.4f %.4f" % p for p in c.coords[:-1]) + " Z"
    d = []
    for pg in (queima.geoms if hasattr(queima, "geoms") else [queima]):
        if pg.is_empty: continue
        d.append(anel(pg.exterior)); d += [anel(i) for i in pg.interiors]

    nome = os.path.splitext(os.path.basename(a.pcb))[0]
    extra = ("_positivo" if a.positivo else "") + (f"_comp{a.compensa:g}" if a.compensa else "")
    saida = a.saida or os.path.join(os.path.dirname(os.path.abspath(__file__)), f"{nome}_laser_{cam.replace('.', '')}{extra}.svg")
    with open(saida, "w", encoding="utf-8") as o:
        o.write(f'<?xml version="1.0" encoding="UTF-8"?>\n'
                f'<svg xmlns="http://www.w3.org/2000/svg" width="{W:.4f}mm" height="{H:.4f}mm" viewBox="0 0 {W:.4f} {H:.4f}">\n'
                f'<!-- {nome} {cam}{"" if a.sem_espelho else " espelhado"}{" positivo" if a.positivo else ""} compensa {a.compensa} mm: preto = laser. Um caminho, par/impar (LightBurn: Fill). -->\n'
                f'<path fill="#000000" fill-rule="evenodd" stroke="none" d="{" ".join(d)}"/>\n</svg>\n')
    print(f"{saida}\n  placa {W:.2f} x {H:.2f} mm | {cam}{'' if a.sem_espelho else ' (espelhado)'}"
          f"{' | POSITIVO' if a.positivo else ''}{f' | cobre +{a.compensa} mm por lado' if a.compensa else ''} | "
          f"{len(furos)} furinhos de guia | {zonas} pedacos de zona")
    if zonas == 0:
        print("  aviso: nenhuma zona preenchida nesta camada. Se tem plano de GND, aperte B no KiCad e salve.")


if __name__ == "__main__":
    main()
