"""
Converte as fotos da pasta "imagens" em um arquivo "fotos.h" que o Arduino
compila junto com o sketch. Assim as fotos vao para a memoria do ESP32.

O que o script faz com cada foto:
  1. Abre a imagem (jpg, jpeg, png, webp... qualquer formato que o Pillow leia)
  2. Corta e redimensiona para 240x320, preenchendo a tela sem distorcer
  3. Salva como JPG "baseline" (o tipo que o ESP32 consegue decodificar)
  4. Escreve os bytes no fotos.h

Uso (na pasta do projeto, onde esta o .ino):
    pip install pillow
    python converter_fotos.py

Rode de novo sempre que trocar as fotos e grave o sketch outra vez.
"""

import io
from pathlib import Path

from PIL import Image, ImageOps

LARGURA, ALTURA = 240, 320   # tela em pe (retrato)
QUALIDADE_JPG = 85           # 0-100; mais alto = mais bonito e arquivo maior
EXTENSOES = {".jpg", ".jpeg", ".png", ".webp", ".bmp"}

pasta_projeto = Path(__file__).parent
pasta_imagens = pasta_projeto / "imagens"
arquivo_saida = pasta_projeto / "fotos.h"

fotos = sorted(p for p in pasta_imagens.iterdir() if p.suffix.lower() in EXTENSOES)
if not fotos:
    raise SystemExit(f"Nenhuma imagem encontrada em {pasta_imagens}")

linhas = [
    "// ARQUIVO GERADO AUTOMATICAMENTE por converter_fotos.py - nao edite a mao.",
    "// Cada foto e um JPG 240x320 guardado como lista de bytes na memoria flash.",
    "#pragma once",
    "#include <Arduino.h>",
    "",
]
nomes = []
total = 0

for caminho in fotos:
    nome = "img_" + "".join(c if c.isalnum() else "_" for c in caminho.stem.lower())

    img = Image.open(caminho)
    img = ImageOps.exif_transpose(img)          # respeita a rotacao do celular
    img = img.convert("RGB")                     # tira transparencia, se houver
    img = ImageOps.fit(img, (LARGURA, ALTURA))   # corta o excesso e redimensiona

    buffer = io.BytesIO()
    img.save(buffer, "JPEG", quality=QUALIDADE_JPG, progressive=False, optimize=True)
    dados = buffer.getvalue()
    total += len(dados)

    linhas.append(f"// {caminho.name} -> {len(dados)} bytes")
    linhas.append(f"const uint8_t {nome}[] PROGMEM = {{")
    for i in range(0, len(dados), 20):
        linhas.append("  " + ", ".join(f"0x{b:02X}" for b in dados[i:i + 20]) + ",")
    linhas.append("};")
    linhas.append("")
    nomes.append((nome, caminho.name))
    print(f"{caminho.name:25s} -> {len(dados) / 1024:6.1f} KB")

# Uma lista com todas as fotos, para o sketch percorrer
linhas.append("struct Foto { const char* nome; const uint8_t* dados; size_t tamanho; };")
linhas.append("")
linhas.append("const Foto fotos[] = {")
for nome, arquivo in nomes:
    linhas.append(f'  {{ "{arquivo}", {nome}, sizeof({nome}) }},')
linhas.append("};")
linhas.append("const int TOTAL_FOTOS = sizeof(fotos) / sizeof(fotos[0]);")

arquivo_saida.write_text("\n".join(linhas) + "\n", encoding="utf-8")
print(f"\n{len(nomes)} fotos, {total / 1024:.1f} KB no total -> {arquivo_saida.name}")
