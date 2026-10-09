"""Quirk de ZHA para el termostato DIY Cheny (github.com/devcheny/Termostato_Zigbee).

ZHA no crea selectores para dispositivos genéricos y no consigue leer el nombre de
las salidas analógicas del termostato, así que este quirk define esas entidades:

  - Endpoint 15 (salida binaria): selector "Sensor de temperatura" Integrado / Externo
    en lugar del interruptor "Usar sensor externo".
  - Endpoint 14 (salida analógica): number "Temperatura externa" (°C).
  - Endpoint 16 (salida analógica): number "Ciclo mínimo caldera" (min).

Instalación: copiar a /config/custom_zha_quirks/ y en configuration.yaml:

    zha:
      custom_quirks_path: /config/custom_zha_quirks/

Después, reiniciar Home Assistant.
"""

from zigpy import types as t
from zigpy.zcl.clusters.general import AnalogOutput, BinaryOutput

from zhaquirks.builder import EntityType, QuirkBuilder, UnitOfTemperature, UnitOfTime


class FuenteTemperatura(t.enum8):
    """Sensor con el que regula el termostato (valor de la salida binaria 15)."""

    Integrado = 0x00
    Externo = 0x01


(
    QuirkBuilder("DIY Cheny", "Termostato")
    # Selector en lugar del interruptor "Usar sensor externo"
    .prevent_default_entity_creation(endpoint_id=15, cluster_id=BinaryOutput.cluster_id)
    .enum(
        BinaryOutput.AttributeDefs.present_value.name,
        FuenteTemperatura,
        BinaryOutput.cluster_id,
        endpoint_id=15,
        entity_type=EntityType.STANDARD,
        translation_key="fuente_temperatura",
        fallback_name="Sensor de temperatura",
    )
    # Números con nombre (ZHA no lee la descripción de estas salidas analógicas)
    .prevent_default_entity_creation(endpoint_id=14, cluster_id=AnalogOutput.cluster_id)
    .number(
        AnalogOutput.AttributeDefs.present_value.name,
        AnalogOutput.cluster_id,
        endpoint_id=14,
        min_value=-20,
        max_value=60,
        step=0.1,
        unit=UnitOfTemperature.CELSIUS,
        entity_type=EntityType.STANDARD,
        translation_key="temperatura_externa",
        fallback_name="Temperatura externa",
    )
    .prevent_default_entity_creation(endpoint_id=16, cluster_id=AnalogOutput.cluster_id)
    .number(
        AnalogOutput.AttributeDefs.present_value.name,
        AnalogOutput.cluster_id,
        endpoint_id=16,
        min_value=0,
        max_value=30,
        step=1,
        unit=UnitOfTime.MINUTES,
        entity_type=EntityType.CONFIG,
        translation_key="ciclo_minimo_caldera",
        fallback_name="Ciclo mínimo caldera",
    )
    .add_to_registry()
)
