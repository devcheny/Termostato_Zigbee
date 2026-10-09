# Termostato_Zigbee

Termostato para la caldera con un **ESP32-C6 Super Mini**, dos relés de 3,3 V, un sensor **SHT31** y una **pantalla táctil de 3,5"** para manejarlo también a mano. Se integra en Home Assistant por **Zigbee (ZHA)** y se actualiza **por Zigbee (OTA)**, sin cables.

En ZHA aparece un dispositivo **DIY Cheny Termostato** con:

| Entidad | Qué hace | Endpoint |
|---------|----------|----------|
| `climate` | Calefacción: modo Apagado / Calor, consigna y temperatura actual. Muestra "Calentando" cuando el relé está activo | 10 |
| `switch` | Caldera (y con ella el agua caliente). Apagarla apaga también la calefacción | 11 |
| `sensor` ×2 | Temperatura y humedad del SHT31 | 12 |
| `switch` | Calefacción forzada: enciende la calefacción sin mirar la temperatura (ver abajo) | 13 |
| `number` | Temperatura externa: HA escribe aquí la de otro sensor (ver "Sensor externo") | 14 |
| `switch` | Usar sensor externo para regular, en vez del SHT31 | 15 |
| `update` | Firmware: avisa cuando hay versión nueva y la instala | 10 |

## Lógica de los relés

- **Relé CALDERA** (GPIO18): enciende la caldera, y con ella el agua caliente.
- **Relé CALEFACCION** (GPIO19): se enciende cuando la calefacción pide calor.

La calefacción no puede funcionar sin caldera, así que los dos interruptores van enlazados:

| Acción (desde HA o la pantalla) | Resultado |
|---|---|
| Encender la calefacción (modo Calor) | Se enciende también la caldera |
| Apagar la caldera | Se apaga también la calefacción |
| Apagar la calefacción | La caldera sigue encendida (agua caliente) |
| Encender la caldera | Solo agua caliente; la calefacción sigue como estaba |

Los estados posibles son: todo apagado, solo caldera (agua caliente), o caldera + calefacción. Calefacción sin caldera no puede darse, salvo con la **calefacción forzada** de pruebas, que activa los dos relés sin tocar los interruptores.

Al encender, primero entra el relé de la caldera; al apagar, primero sale el de la calefacción.

La regulación la hace el ESP32, no HA:

- Enciende cuando la temperatura baja **0,3 °C** por debajo de la consigna y apaga cuando sube 0,3 °C por encima (`HYSTERESIS`).
- Entre encendido y apagado pasan al menos **3 minutos** (`MIN_CYCLE_MS`), para no hacer ciclos cortos en la caldera. Poner el modo en Apagado corta al momento.
- Si el SHT31 deja de responder durante 1 minuto, deja de calentar por seguridad. El LED parpadea en rojo. La caldera (agua caliente) no se ve afectada.
- **Calefacción forzada**: con este interruptor en ON, los relés de calefacción y caldera se encienden sin mirar el modo, la consigna, el sensor ni el interruptor de la caldera. Sirve para probar los relés sin el SHT31 conectado, o para tirar de calefacción si el sensor se estropea. **Se apaga sola a los 30 minutos** (`FORCE_TIMEOUT_MS`) y el interruptor de HA vuelve a OFF. No se guarda: tras un reinicio arranca apagada. En la pantalla aparece "Calefaccion forzada".
- La configuración (modo, consigna, caldera, límites, calibración) se guarda en flash. Tras un corte de luz sigue funcionando igual aunque HA o la red Zigbee no estén.

## Pantalla táctil

Pantalla de 3,5" 320×480 SPI con táctil resistivo (la que viene con lápiz).

```
+------------------------------------------------+
| ● Zigbee                              Hum 48%  |
| Sensor interno  |        Consigna              |
|                 |         20.5°                |
|     21.4°       |   [   -   ]   [   +   ]      |
|   Calentando    |                              |
| [ Radiadores ENCENDIDA ]  [ Caldera ENCENDIDA ] |
+------------------------------------------------+
```

- **− / +**: cambian la consigna de 0,5 en 0,5 °C. Si mantienes pulsado, repite.
- **Radiadores**: cambia la calefacción entre Calor y Apagado (lo mismo que el modo en HA). Al encenderla se enciende también la caldera.
- **Caldera**: enciende o apaga la caldera (agua caliente). Al apagarla se apaga también la calefacción.
- **Zona de la temperatura**: al tocarla cambia entre el sensor interno (SHT31) y el externo. Encima de la temperatura pone cuál se usa: "Sensor interno", "Sensor externo" o "Externo sin datos" (en naranja: está elegido el externo, pero no llega y se regula con el SHT31).
- Lo que cambies en la pantalla se ve en HA al momento, y al revés.
- Tras 30 s sin tocarla baja el brillo (`DIM_AFTER_MS`, `BL_DIM`). El primer toque solo la enciende, no pulsa ningún botón.
- Arriba: estado de la red Zigbee (verde = conectado, rojo = sin red, azul = actualizando firmware) y la humedad.

**Chip de la pantalla:** se vende como "ILI9341", pero un ILI9341 no llega a 320×480. Estas pantallas llevan **ILI9488** (lo más habitual) o **ST7796S**. El sketch viene para ILI9488. Si la pantalla se queda en blanco o la imagen sale mal, comenta `#define PANEL_ILI9488` en [Pantalla.h](Pantalla.h) para usar ST7796S. Si los colores salen invertidos o con rojo y azul cambiados, cambia `PANEL_INVERT` o `PANEL_BGR`.

**Calibrar el táctil:** la primera vez, al arrancar sale "Toca la pantalla para calibrar el tactil" durante 8 s. Toca, y después toca el centro de cada flecha que aparece en las esquinas. Se guarda en el ESP32. Para recalibrar, toca la pantalla durante los 2 s del arranque.

## Conexionado

```
ESP32-C6 Super Mini        Módulos de relé / SHT31
  3V3   ───────────────┬── VCC relé 1
                       ├── VCC relé 2
                       └── VIN SHT31
  GND   ───────────────┬── GND relé 1, GND relé 2
                       └── GND SHT31
  GPIO18 ──────────────── IN relé 1  (CALDERA / agua caliente)
  GPIO19 ──────────────── IN relé 2  (CALEFACCION)
  GPIO6  ──────────────── SDA SHT31
  GPIO7  ──────────────── SCL SHT31
```

- 3V3, GND, 19 y 18 están en la fila derecha de la placa; 6 y 7 en la izquierda.

Pantalla (pines del conector de 14 pines de la pantalla):

```
ESP32-C6 Super Mini        Pantalla 3,5" SPI
  5V    ───────────────── VCC
  GND   ───────────────── GND
  GPIO20 ──────────────── CS
  GPIO0  ──────────────── RESET
  GPIO14 ──────────────── DC/RS
  GPIO22 ──────────────┬─ SDI(MOSI)
                       └─ T_DIN
  GPIO21 ──────────────┬─ SCK
                       └─ T_CLK
  GPIO1  ──────────────── LED
  (sin conectar)          SDO(MISO)
  GPIO2  ──────────────── T_CS
  GPIO23 ──────────────── T_DO
  GPIO3  ──────────────── T_IRQ
```

- 21, 22 y 23 son los agujeros del centro de la placa; 0, 1, 2 y 3 están en la fila izquierda; 20 y 14 en la derecha.
- **SDO(MISO) de la pantalla se deja sin conectar**: el ILI9488 no suelta esa línea y estropearía las lecturas del táctil. El táctil va por T_DO.
- **VCC a 5V** si la pantalla lleva su regulador (un chip de 3 patas cerca del conector y el jumper J1 abierto, lo normal en estas placas). Si no lo lleva, a 3V3. Las señales son de 3,3 V en los dos casos.
- LED controla la retroiluminación a través de un transistor de la propia pantalla, así que el GPIO1 puede regular el brillo. Si no te interesa atenuarla, conecta LED a 3V3.
- Los pines de la tarjeta SD no se usan.
- El SHT31 se detecta solo en la dirección 0x44 o 0x45. Las placas de SHT31 ya llevan resistencias pull-up en SDA/SCL.
- Si tus relés se activan con nivel **bajo** (muchos módulos lo llevan marcado como `L` o tienen un jumper H/L), cambia `RELAY_ON` a `LOW` en el sketch.
- Alimenta el conjunto con un cargador USB de 5 V de al menos 1 A. Los dos relés consumen unos 70 mA cada uno desde 3V3.
- **Pon el SHT31 lejos del ESP32 y de los relés**, con un cable de 15-20 cm o fuera de la caja. Si no, el calor de la placa falsea la lectura. Si aun así marca alto, corrígelo con la calibración (ver abajo).
- **Los contactos del relé van en lugar del termostato actual de la caldera** (contacto libre de tensión, bornes COM y NO). Mira el manual de la caldera para saber qué bornes son la demanda de calefacción y cuáles el agua caliente. Si conmutas 230 V, hazlo con la caldera desenchufada y con relés y cables adecuados para esa tensión.

Pines que se evitan:
- GPIO4, 5, 8, 9 y 15: son de arranque (strapping).
- GPIO8: es además el LED RGB de la placa.
- GPIO9: es además el botón BOOT.
- GPIO12 y 13: van al USB.
- GPIO16 y 17: son el UART0.

## Compilación y primera carga (por USB)

- Core **esp32** de Espressif **3.x** (probado con 3.3.12). La librería Zigbee viene incluida. No hace falta ninguna librería para el SHT31.
- Librería **LovyanGFX** (probado con 1.2.32) desde el gestor de librerías, o `arduino-cli lib install LovyanGFX`. Su configuración está en [Pantalla.h](Pantalla.h); no hay que editar nada dentro de la librería.
- Placa: **ESP32C6 Dev Module**.
- **Tools → Zigbee mode: Zigbee ED (end device)**.
- **Tools → Partition Scheme: Zigbee 4MB with spiffs**. Tiene dos particiones de app, que es lo que permite la OTA.
- `USB CDC On Boot: Enabled` para ver el Serial a 115200.
- La primera vez, activa `Erase All Flash Before Sketch Upload`.

Desde VS Code, con el `.ino` abierto: `Ctrl+Shift+P` → **Tasks: Run Task** → **Arduino: compilar y subir**. Elige la placa `esp32:esp32:esp32c6:ZigbeeMode=ed,...` (la variante con `EraseFlash=all` la primera vez) y el puerto.

Equivalente en terminal:

```
arduino-cli compile --upload -b "esp32:esp32:esp32c6:ZigbeeMode=ed,PartitionScheme=zigbee,CDCOnBoot=cdc" -p COM13 Termostato_Zigbee
```

## Emparejar en ZHA

1. En HA: **Ajustes → Dispositivos y servicios → Zigbee Home Automation → Añadir dispositivo**.
2. Alimenta el ESP32. El LED RGB parpadea en **azul** mientras busca red.
3. Cuando se une, el LED se apaga y aparece **DIY Cheny Termostato**.
4. Renombra las entidades: en el dispositivo, entra en cada entidad → engranaje → **Nombre** e **ID de entidad**. Por ejemplo:

   | Entidad que crea ZHA | Nombre | ID de entidad |
   |---|---|---|
   | climate | Calefacción | `climate.calefaccion` |
   | interruptor del endpoint 11 (caldera) | Caldera | `switch.caldera` |
   | interruptor del endpoint 13 (forzada) | Calefacción forzada | `switch.calefaccion_forzada` |
   | number del endpoint 14 | Temperatura externa | `number.termostato_temperatura_externa` |
   | interruptor del endpoint 15 | Usar sensor externo | `switch.termostato_sensor_externo` |

   Con ZHA el firmware no puede poner estos nombres, así que hay que hacerlo a mano una vez. Se conservan en todas las actualizaciones OTA. Solo se pierden si eliminas el dispositivo de ZHA, así que hazlo después del último reemparejado.

**Volver a emparejar:** mantén pulsado **BOOT** 3 s. El LED se pone rojo, los relés se apagan, se borra la red Zigbee y se reinicia en modo emparejamiento. Hazlo también si lo quitas de ZHA.

LED de estado:
- Azul parpadeando: buscando red.
- Naranja fijo: calefacción encendida.
- Rojo parpadeando: el SHT31 no responde y el modo es Calor.
- Apagado: conectado y en reposo.

## Programar desde Home Assistant

El termostato no guarda horarios: HA le cambia la consigna o el modo con automatizaciones. Si HA se cae, sigue con la última consigna.

Ejemplo: 21 °C de 7:00 a 23:00 y 17 °C por la noche, de lunes a viernes.

```yaml
alias: Calefacción - horario
triggers:
  - trigger: time
    at: "07:00:00"
    id: dia
  - trigger: time
    at: "23:00:00"
    id: noche
conditions:
  - condition: time
    weekday: [mon, tue, wed, thu, fri]
actions:
  - action: climate.set_temperature
    target:
      entity_id: climate.diy_cheny_termostato_termostato   # cámbialo por el tuyo
    data:
      temperature: "{{ 21 if trigger.id == 'dia' else 17 }}"
mode: single
```

La caldera es un `switch` normal: `switch.turn_on` / `switch.turn_off` en el horario que quieras. Recuerda que apagarla apaga también la calefacción.

Para un horario semanal editable desde la interfaz, sirve el **helper Horario** (`schedule`) de HA y una automatización que cambie la consigna cuando el horario pase a `on` / `off`.

### Sensor externo (p. ej. SONOFF SNZB-02D)

El termostato puede regular con la temperatura de otro sensor de la casa en vez de con el SHT31. Es útil si el termostato está en un sitio poco representativo (pasillo, cerca de la caldera…).

**Por qué no se empareja directamente con el ESP32:** un dispositivo Zigbee solo puede estar en una red, la del coordinador de ZHA, y el ESP32 no es coordinador. Dentro de la misma red, el sensor podría enviar sus lecturas directamente al ESP32 (un *binding*), pero ZHA no permite crear ese binding desde su interfaz. Además, el SNZB-02D pasa casi todo el tiempo dormido y es difícil configurarlo. Por eso es HA quien le pasa la temperatura al termostato.

1. Empareja el SNZB-02D en ZHA como cualquier sensor.
2. Crea esta automatización. Cambia `sensor.snzb_02d_temperatura` por la entidad de tu sensor y `number.termostato_temperatura_externa` por la del termostato:

   ```yaml
   alias: Termostato - temperatura externa
   description: Envía al termostato la temperatura del sensor externo
   triggers:
     - trigger: state
       entity_id: sensor.snzb_02d_temperatura
     - trigger: time_pattern
       minutes: /10          # reenvío periódico aunque no cambie
     - trigger: homeassistant
       event: start
   conditions:
     - condition: template
       value_template: "{{ states('sensor.snzb_02d_temperatura') | is_number }}"
   actions:
     - action: number.set_value
       target:
         entity_id: number.termostato_temperatura_externa
       data:
         value: "{{ states('sensor.snzb_02d_temperatura') | float | round(1) }}"
   mode: queued
   ```

3. Activa el interruptor **Usar sensor externo**, o toca la temperatura en la pantalla.

Seguridad: si pasa **1 hora** sin recibir la temperatura externa (HA caído, sensor sin pilas…), el termostato vuelve solo al SHT31. En la pantalla pone "Externo sin datos" y, en cuanto vuelve a llegar, regresa al externo. Si tampoco hay SHT31, no calienta. El tiempo se cambia en `EXT_TIMEOUT_MS`.

La elección de sensor se guarda en el ESP32. La temperatura externa no: tras un reinicio usa el SHT31 hasta que HA la vuelve a enviar (como mucho 10 minutos). La humedad de la pantalla y la entidad de humedad siguen siendo las del SHT31. La calibración solo se aplica al SHT31; el sensor externo se calibra en su propio dispositivo de ZHA.

### Calibración y límites

Si ZHA los muestra en la página del dispositivo (sección Configuración), puedes ajustar:
- **Calibración de temperatura local**: suma o resta hasta ±5 °C a la lectura del SHT31.
- **Consigna mínima / máxima**: limitan lo que se puede pedir desde HA (entre 5 y 30 °C).

Los valores se guardan en el ESP32.

## Actualizaciones por Zigbee (OTA)

Las versiones nuevas se publican solas en GitHub y ZHA las ofrece en la entidad **update** del termostato.

```
cambias el sketch y subes FW_VERSION ─► git push ─► GitHub Actions compila y crea la release
                                                     (fw-00000003: .ota + index.json)
HA lee releases/latest/download/index.json ─► la entidad update ofrece la versión ─► Instalar
```

### 1. Repositorio en GitHub (una vez)

El repositorio tiene que ser **público**: ZHA descarga el `index.json` y el `.ota` sin usuario ni token, y de un repositorio privado no puede. El firmware no lleva contraseñas ni claves (la clave de la red Zigbee se negocia al emparejar).

1. Crea en GitHub un repositorio público vacío llamado `Termostato_Zigbee`.
2. Sube esta carpeta (ya es un repositorio git con la rama `main` y el remoto `origin` configurado):
   ```
   git push -u origin main
   ```
3. En la pestaña **Actions** verás el workflow **Firmware OTA**. La primera vez tarda unos minutos porque descarga el core esp32; después usa la caché.

### 2. Configurar ZHA (una vez)

En `configuration.yaml` de HA:

```yaml
zha:
  zigpy_config:
    ota:
      extra_providers:
        - type: zigpy_remote
          url: https://github.com/devcheny/Termostato_Zigbee/releases/latest/download/index.json
```

Reinicia HA.

### 3. Publicar una versión nueva

1. Cambia lo que quieras y **sube `FW_VERSION`** en el sketch (`0x00000002` → `0x00000003`…).
2. `git commit` y `git push`. El mensaje del commit sale como notas de la release.
3. GitHub Actions compila, genera el `.ota` y el `index.json` y crea la release `fw-00000003`.

Si haces push sin subir `FW_VERSION`, el workflow no publica nada. Así puedes subir cambios del README u otros sin generar versiones.

### 4. Instalarla

1. ZHA lee el `index.json` como mucho **una vez cada 24 h**. Para que lo lea ya, reinicia HA o recarga la integración ZHA.
2. El termostato pregunta por actualizaciones al conectarse y luego cada hora. Para no esperar, reinícialo (desenchufar y enchufar).
3. La entidad **update** del dispositivo muestra la versión nueva. Pulsa **Instalar**. La descarga tarda entre 1 y 1,5 horas (ZHA envía la imagen en trozos de 50 bytes, unos 16.000 trozos) y mientras tanto el termostato sigue regulando con normalidad.
4. Al terminar, el ESP32 se reinicia con el firmware nuevo y conserva la red y la configuración.

ZHA comprueba el checksum del `.ota` antes de enviarlo. Si aun así la imagen llega mal, el ESP32 la descarta y sigue con la versión anterior.

### Alternativa sin GitHub (archivo local)

Para probar una versión sin publicarla:

1. En `configuration.yaml`, añade otro proveedor en `extra_providers` (puede convivir con el de GitHub). El texto de `warning` tiene que ser exactamente este:
   ```yaml
        - type: advanced
          warning: I understand I can *destroy* my devices by enabling OTA updates from files. Some OTA updates can be mistakenly applied to the wrong device, breaking it. I am consciously using this at my own risk.
          path: /config/zigpy_ota
   ```
2. Sube `FW_VERSION` y genera la OTA: en VS Code, **Tasks: Run Task → Arduino: generar OTA Zigbee**, o `python make_ota.py --build`.
3. Copia `ota/Termostato_Zigbee_XXXXXXXX.ota` a `/config/zigpy_ota/` y sigue los pasos de "Instalarla".

`make_ota.py` lee del sketch `FW_VERSION`, `OTA_MANUFACTURER`, `OTA_IMAGE_TYPE` y `OTA_HW_VERSION`. No cambies los tres últimos: el ESP32 solo acepta imágenes que coincidan con ellos, y ZHA solo ofrece la versión a dispositivos con ese mismo fabricante y tipo de imagen. El nombre (`MANUFACTURER`, `MODEL`) sí se puede cambiar sin afectar a la OTA.

## Notas

- Funciona como **end device** con la radio siempre escuchando, igual que `Luz_Plantas_ESP32`. No repite la señal Zigbee. Si necesitas que haga de router, cambia a `Zigbee mode: Zigbee ZCZR` y `Zigbee.begin(ZIGBEE_ROUTER)`.
- Las constantes de ajuste están al principio del sketch: `HYSTERESIS`, `MIN_CYCLE_MS`, pines, intervalos de lectura y envío. Las de la pantalla (pines, brillo, paso de la consigna) están al principio de [Pantalla.h](Pantalla.h).
- Si apagas los radiadores y los vuelves a encender enseguida, la calefacción puede tardar hasta 3 minutos en arrancar: es el tiempo mínimo entre ciclos (`MIN_CYCLE_MS`) que protege la caldera.
