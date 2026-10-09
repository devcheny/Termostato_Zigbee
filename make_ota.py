"""Genera las imágenes OTA Zigbee (.ota) para ZHA a partir del .bin compilado.

Lee del .ino FW_VERSION, OTA_MANUFACTURER, OTA_IMAGE_TYPE, OTA_HW_VERSION,
OTA_HW_VERSION_FULL_ONLY y OTA_DELTA_SINCE, así que solo hay que subir FW_VERSION
en el sketch antes de compilar.

Uso:
  python make_ota.py --build                      compila con arduino-cli y genera la OTA
  python make_ota.py --bin firmware.bin           genera la OTA de un .bin ya compilado
  ... --base-url https://.../                     además escribe index.json (proveedor
                                                  zigpy_remote de ZHA) apuntando a esa URL
  ... --delta-from v1.ota v2.ota ...              además genera parches delta desde esas
                                                  versiones (imágenes completas anteriores).
                                                  Necesita: pip install detools==0.53.0
  ... --out carpeta                               carpeta de salida (por defecto ota/)

Formato: cabecera OTA Zigbee (ZCL 11.4) + un elemento "Upgrade Image" (tag 0x0000).
El elemento lleva la app tal cual (imagen completa) o "ZDLT" + tamaño + parche de
detools (delta), que es lo que entiende esp_delta_ota_ops.cpp.
"""
import argparse
import hashlib
import io
import json
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
INO = HERE / (HERE.name + ".ino")
FQBN = "esp32:esp32:esp32c6:ZigbeeMode=ed,PartitionScheme=zigbee,CDCOnBoot=cdc"

OTA_FILE_ID = 0x0BEEF11E
OTA_HEADER_LEN = 56 + 4                  # cabecera básica + versión mínima/máxima de hardware
DELTA_MAGIC = b"ZDLT"
# Tiene que coincidir con lo que acepta delta_ota_config.h (heatshrink con memoria dinámica)
HEATSHRINK_WINDOW_SZ2 = 11
HEATSHRINK_LOOKAHEAD_SZ2 = 4
DELTA_MAX_RATIO = 0.8                    # si el parche no ahorra al menos un 20 %, no se publica


def build():
    out = Path(tempfile.gettempdir()) / "arduino_ota_build"
    # cwd local: el core esp32 usa CMD.EXE en sus hooks y no admite rutas UNC
    r = subprocess.run(["arduino-cli", "compile", "--fqbn", FQBN, "--output-dir", str(out), str(HERE)], cwd=tempfile.gettempdir())
    if r.returncode != 0:
        sys.exit("Error al compilar")
    return out / (HERE.name + ".ino.bin")


def define(src, name):
    m = re.search(r"^\s*#define\s+" + name + r"\s+(0x[0-9A-Fa-f]+|\d+)", src, re.M)
    if not m:
        sys.exit(f"No encuentro #define {name} en {INO.name}")
    return int(m.group(1), 0)


def make_ota(manuf, image_type, version, hw_min, hw_max, element_data):
    element = struct.pack("<HI", 0x0000, len(element_data)) + element_data
    header_string = f"{HERE.name} v{version:#010x}".encode()[:32].ljust(32, b"\0")
    header = struct.pack(
        "<IHHHHHIH32sI",
        OTA_FILE_ID,
        0x0100,                                 # versión de cabecera
        OTA_HEADER_LEN,
        0x0004,                                 # incluye versión mínima/máxima de hardware
        manuf,
        image_type,
        version,
        0x0002,                                 # Zigbee PRO
        header_string,
        OTA_HEADER_LEN + len(element),          # tamaño total del fichero
    ) + struct.pack("<HH", hw_min, hw_max)
    return header + element


def read_ota(path):
    """Devuelve (versión, app) de una imagen completa generada por este script."""
    data = Path(path).read_bytes()
    file_id, _, header_len = struct.unpack_from("<IHH", data, 0)
    if file_id != OTA_FILE_ID:
        raise ValueError(f"{path} no es un fichero OTA Zigbee")
    version = struct.unpack_from("<I", data, 14)[0]   # tras id, versión y longitud de cabecera, control, fabricante y tipo
    tag, length = struct.unpack_from("<HI", data, header_len)
    app = data[header_len + 6:header_len + 6 + length]
    if tag != 0 or app[:1] != b"\xE9":
        raise ValueError(f"{path} no contiene una imagen completa")
    return version, app


def make_patch(old, new):
    try:
        import detools
    except ImportError:
        sys.exit("Para --delta-from hace falta: pip install detools==0.53.0")
    fpatch = io.BytesIO()
    detools.create_patch(io.BytesIO(old), io.BytesIO(new), fpatch,
                         compression="heatshrink",
                         use_mmap=False,      # BytesIO no admite mmap
                         heatshrink_window_sz2=HEATSHRINK_WINDOW_SZ2,
                         heatshrink_lookahead_sz2=HEATSHRINK_LOOKAHEAD_SZ2)
    patch = fpatch.getvalue()
    # Comprobación: el parche aplicado sobre la versión antigua da exactamente la nueva
    fto = io.BytesIO()
    detools.apply_patch(io.BytesIO(old), io.BytesIO(patch), fto)
    if fto.getvalue() != new:
        sys.exit("El parche generado no reconstruye la imagen nueva")
    return patch


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src_arg = ap.add_mutually_exclusive_group(required=True)
    src_arg.add_argument("--build", action="store_true", help="compilar con arduino-cli")
    src_arg.add_argument("--bin", type=Path, help="binario de app ya compilado")
    ap.add_argument("--out", type=Path, default=HERE / "ota", help="carpeta de salida")
    ap.add_argument("--base-url", help="URL donde se publicarán los .ota; genera index.json")
    ap.add_argument("--delta-from", type=Path, nargs="*", default=[], help="imágenes completas anteriores")
    args = ap.parse_args()

    bin_path = build() if args.build else args.bin

    src = INO.read_text(encoding="utf-8")
    version = define(src, "FW_VERSION")
    manuf = define(src, "OTA_MANUFACTURER")
    image_type = define(src, "OTA_IMAGE_TYPE")
    hw = define(src, "OTA_HW_VERSION")
    hw_full_only = define(src, "OTA_HW_VERSION_FULL_ONLY")
    delta_since = define(src, "OTA_DELTA_SINCE")

    app = bin_path.read_bytes()
    if app[:1] != b"\xE9":
        sys.exit(f"{bin_path} no parece un binario de app ESP32 (falta el byte mágico 0xE9)")

    args.out.mkdir(parents=True, exist_ok=True)
    images = []   # (nombre, contenido, entrada extra del índice)

    # Imagen completa: la aceptan todos, también los que solo piden completas
    full = make_ota(manuf, image_type, version, hw, hw_full_only, app)
    images.append((f"{HERE.name}_{version:08X}.ota", full,
                   {"min_hardware_version": hw, "max_hardware_version": hw_full_only}))
    print(f"Imagen completa: version 0x{version:08X}  fabricante 0x{manuf:04X}  tipo 0x{image_type:04X}  {len(full)} bytes")

    # Parches delta desde versiones anteriores que saben aplicarlos
    for path in args.delta_from:
        old_version, old_app = read_ota(path)
        if old_version >= version or old_version < delta_since:
            print(f"  0x{old_version:08X}: sin parche (no sabe aplicarlos o no es anterior)")
            continue
        patch = make_patch(old_app, app)
        data = DELTA_MAGIC + struct.pack("<I", len(patch)) + patch
        if len(data) > DELTA_MAX_RATIO * len(app):
            print(f"  0x{old_version:08X}: parche de {len(data)} bytes, no compensa")
            continue
        delta = make_ota(manuf, image_type, version, hw, hw, data)
        images.append((f"{HERE.name}_{version:08X}_desde_{old_version:08X}.ota", delta, {
            "min_hardware_version": hw,
            "max_hardware_version": hw,
            "min_current_file_version": old_version,
            "max_current_file_version": old_version,
            "specificity": 100,      # ZHA la prefiere a la completa
        }))
        print(f"  0x{old_version:08X}: parche de {len(delta)} bytes ({100 * len(delta) // len(full)} % de la completa)")

    for name, content, _ in images:
        (args.out / name).write_bytes(content)
        print(f"Generada: {args.out / name}")

    if args.base_url:
        # Formato del proveedor zigpy_remote (el checksum tiene que ser sha3-256).
        # Sin manufacturer_names / model_names: ZHA guarda el nombre al emparejar y, si
        # se renombra el dispositivo, dejaría de ofrecerle la OTA. El fabricante, el tipo
        # de imagen y la versión de hardware ya la limitan a este termostato.
        index = {"firmwares": [{
            "binary_url": args.base_url.rstrip("/") + "/" + name,
            "file_version": version,
            "file_size": len(content),
            "image_type": image_type,
            "manufacturer_id": manuf,
            "checksum": "sha3-256:" + hashlib.sha3_256(content).hexdigest(),
            **extra,
        } for name, content, extra in images]}
        (args.out / "index.json").write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
        print(f"Indice generado: {args.out / 'index.json'}")
    else:
        print("Copia la imagen completa a /config/zigpy_ota/ en Home Assistant (ver README.md).")


if __name__ == "__main__":
    main()
