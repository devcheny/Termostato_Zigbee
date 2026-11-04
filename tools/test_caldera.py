#!/usr/bin/env python3
"""Pruebas de la lógica de relés en el PC, sin placa.

Extrae del .ino y de Pantalla.h el código real (no una copia) de relés, regulación,
pantalla y cambios desde HA, y lo compila con tools/test_caldera.cpp, que simula el
reloj, los relés y la pila Zigbee.

  python tools/test_caldera.py
  CXX="python -m ziglang c++" python tools/test_caldera.py   (sin g++ instalado)

GitHub Actions lo ejecuta antes de publicar: si falla, no hay release.
"""
import os
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FUNCTIONS = [
    "extValid", "clampSetpoint", "markDirty", "applyRelays", "boilerWaitSecs", "setHeatDemand",
    "regulate", "updateControlTemp", "publishConfig", "setHeatMode", "setCaldera",
    "publishAndReport", "setHyst", "hystStep", "processZigbeeChanges", "localConfigChanged",
    "handleUi", "onAcsChange",
]

# Líneas de loop() y setup() que el simulador repite: si cambian, hay que revisarlo
LOOP_AND_SETUP = [
    "if (boilerWaiting && boilerWaitSecs() == 0) applyRelays();",
    "boilerSwitchedOnce = true;\n  lastBoilerSwitch = millis();\n  applyRelays();",
]


def read(name):
    with open(os.path.join(ROOT, name), encoding="utf-8", newline="") as f:
        return f.read().replace("\r\n", "\n")


def block(src, start, end):
    """Líneas desde la que contiene start hasta la que contiene end, incluidas."""
    a = src.index(start)
    a = src.rindex("\n", 0, a) + 1
    b = src.index("\n", src.index(end, a))
    return src[a:b + 1]


def function(src, name):
    m = re.search(r"^[\w][\w \t\*&]*\b" + name + r"\([^)\n]*\) \{\n", src, re.M)
    if not m:
        sys.exit(f"No encuentro la función {name} en el firmware")
    end = src.index("\n}\n", m.end()) + 3
    return m.group(0).rstrip(" {\n") + ";\n", src[m.start():end]


def main():
    ino = read("Termostato_Zigbee.ino")
    pantalla = read("Pantalla.h")

    for text in LOOP_AND_SETUP:
        if text not in ino:
            sys.exit(f"El simulador repite este código de loop/setup y ya no está:\n{text}")

    defines = block(ino, "#define RELAY_CALDERA_PIN", "#define RELAY_OFF")
    defines += block(ino, "#define HYST_DEFAULT", "#define SETPOINT_ABS_MAX")
    defines += block(pantalla, "#define SP_STEP", "#define SP_STEP")

    state = block(ino, "struct Config {", "bool boilerWaiting")
    state += block(ino, "volatile bool zbChanged", "volatile float zbHystOnValue")
    state += block(ino, "volatile int32_t zbPending[5];", "enum { P_MODE")
    ui_types = block(pantalla, "struct UiState {", "};\n\nenum UiAction").rsplit("enum UiAction", 1)[0]
    ui_types += block(pantalla, "enum UiAction", "UI_HOFF_UP };")

    protos, bodies = zip(*(function(ino, name) for name in FUNCTIONS))

    def host32(code):
        # En el ESP32 unsigned long es de 32 bits; en Linux, de 64. Sin esto el
        # desbordamiento de millis() no se probaría de verdad.
        code = re.sub(r"\bunsigned long\b", "uint32_t", code)
        return re.sub(r"(\d)UL\b", r"\1U", code)

    with tempfile.TemporaryDirectory() as tmp:
        for name, code in (("firmware_tipos.inc", defines + ui_types + state),
                           ("firmware_extraido.inc", "".join(protos) + "".join(bodies))):
            with open(os.path.join(tmp, name), "w", encoding="utf-8") as f:
                f.write(host32(code))
        exe = os.path.join(tmp, "test_caldera.exe")
        cxx = shlex.split(os.environ.get("CXX", "g++"))
        cmd = cxx + ["-std=gnu++17", "-O1", "-I", tmp,
                     os.path.join(ROOT, "tools", "test_caldera.cpp"), "-o", exe]
        subprocess.run(cmd, check=True)
        return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
