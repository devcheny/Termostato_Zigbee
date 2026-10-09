# detools 0.53.0 (solo la parte que aplica parches)

Copia sin modificar de `c/detools.c`, `c/detools.h` y `c/heatshrink/*` de
https://github.com/eerimoq/detools (etiqueta 0.53.0), licencia BSD de 2 cláusulas (ver LICENSE).

Los archivos de heatshrink están en esta misma carpeta en lugar de en `heatshrink/`
para que `#include "heatshrink_decoder.h"` funcione sin rutas de inclusión extra.

Se compilan desde `detools_build.c` y `heatshrink_build.c`, en la raíz del sketch,
con la configuración de `delta_ota_config.h`. Los parches los crea GitHub Actions con
el paquete de Python `detools==0.53.0` (ver `make_ota.py`); las dos versiones tienen
que coincidir.
