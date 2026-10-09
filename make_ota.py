"""Genera la imagen OTA Zigbee (.ota) para ZHA a partir del .bin compilado.

Lee FW_VERSION, OTA_MANUFACTURER, OTA_IMAGE_TYPE, OTA_HW_VERSION, MANUFACTURER y
MODEL del .ino, así que solo hay que subir FW_VERSION en el sketch antes de compilar.

Uso:
  python make_ota.py --build                      compila con arduino-cli y genera la OTA
  python make_ota.py --bin firmware.bin           genera la OTA de un .bin ya compilado
  ... --base-url https://.../                     además escribe index.json (proveedor
                                                  zigpy_remote de ZHA) apuntando a esa URL
  ... --out carpeta                               carpeta de salida (por defecto ota/)

Formato: cabecera OTA Zigbee (ZCL 11.4) + un elemento "Upgrade Image" (tag 0x0000)
con el binario de la app, que es lo que espera el cliente OTA de arduino-esp32.
"""
import argparse
import hashlib
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


def build():
    out = Path(tempfile.gettempdir()) / "arduino_ota_build"
    # cwd local: el core esp32 usa CMD.EXE en sus hooks y no admite rutas UNC
    r = subprocess.run(["arduino-cli", "compile", "--fqbn", FQBN, "--output-dir", str(out), str(HERE)], cwd=tempfile.gettempdir())
    if r.returncode != 0:
        sys.exit("Error al compilar")
    return out / (HERE.name + ".ino.bin")


def define(src, name, string=False):
    value = r'"([^"]*)"' if string else r"(0x[0-9A-Fa-f]+|\d+)"
    m = re.search(r"^\s*#define\s+" + name + r"\s+" + value, src, re.M)
    if not m:
        sys.exit(f"No encuentro #define {name} en {INO.name}")
    return m.group(1) if string else int(m.group(1), 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src_arg = ap.add_mutually_exclusive_group(required=True)
    src_arg.add_argument("--build", action="store_true", help="compilar con arduino-cli")
    src_arg.add_argument("--bin", type=Path, help="binario de app ya compilado")
    ap.add_argument("--out", type=Path, default=HERE / "ota", help="carpeta de salida")
    ap.add_argument("--base-url", help="URL donde se publicará el .ota; genera index.json")
    args = ap.parse_args()

    bin_path = build() if args.build else args.bin

    src = INO.read_text(encoding="utf-8")
    version = define(src, "FW_VERSION")
    manuf = define(src, "OTA_MANUFACTURER")
    image_type = define(src, "OTA_IMAGE_TYPE")
    hw = define(src, "OTA_HW_VERSION")

    payload = bin_path.read_bytes()
    if payload[:1] != b"\xE9":
        sys.exit(f"{bin_path} no parece un binario de app ESP32 (falta el byte mágico 0xE9)")

    element = struct.pack("<HI", 0x0000, len(payload)) + payload
    field_control = 0x0004                      # incluye versión mínima/máxima de hardware
    header_len = 56 + 4
    header_string = f"{HERE.name} v{version:#010x}".encode()[:32].ljust(32, b"\0")
    header = struct.pack(
        "<IHHHHHIH32sI",
        0x0BEEF11E,                             # identificador de fichero OTA
        0x0100,                                 # versión de cabecera
        header_len,
        field_control,
        manuf,
        image_type,
        version,
        0x0002,                                 # Zigbee PRO
        header_string,
        header_len + len(element),              # tamaño total del fichero
    ) + struct.pack("<HH", hw, hw)
    image = header + element

    args.out.mkdir(parents=True, exist_ok=True)
    name = f"{HERE.name}_{version:08X}.ota"
    (args.out / name).write_bytes(image)
    print(f"OTA generada: {args.out / name}")
    print(f"  version 0x{version:08X}  fabricante 0x{manuf:04X}  tipo 0x{image_type:04X}  hw 0x{hw:04X}")
    print(f"  {len(image)} bytes")

    if args.base_url:
        # Formato del proveedor zigpy_remote (el checksum tiene que ser sha3-256)
        index = {
            "firmwares": [{
                "binary_url": args.base_url.rstrip("/") + "/" + name,
                "file_version": version,
                "file_size": len(image),
                "image_type": image_type,
                "manufacturer_id": manuf,
                "manufacturer_names": [define(src, "MANUFACTURER", string=True)],
                "model_names": [define(src, "MODEL", string=True)],
                "checksum": "sha3-256:" + hashlib.sha3_256(image).hexdigest(),
                "min_hardware_version": hw,
                "max_hardware_version": hw,
            }]
        }
        (args.out / "index.json").write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
        print(f"Indice generado: {args.out / 'index.json'}")
    else:
        print("Copiala a /config/zigpy_ota/ en Home Assistant (ver README.md).")


if __name__ == "__main__":
    main()
