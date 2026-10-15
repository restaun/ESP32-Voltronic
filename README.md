# ESP32 Voltronic

Firmware independiente para la Waveshare ESP32-S3-Touch-LCD-7B, configurado
con PlatformIO y ESP-IDF. Inicializa la pantalla táctil,
selecciona USB en el expansor CH422G, conecta como USB Host HID y consulta el
identificador PI30 del Axpert VMIII. Solo muestra **AXPERT CONECTADO** cuando
recibe una respuesta PI30 con CRC válido; enumerar un dispositivo HID no basta
para darlo por conectado.

[![Licencia GPL-3.0](https://img.shields.io/badge/licencia-GPL--3.0-blue.svg)](LICENSE)
[![Invítame a un café](https://img.shields.io/badge/Ko--fi-Inv%C3%ADtame%20a%20un%20caf%C3%A9-ff5e5b)](https://ko-fi.com/restaun)

## Compilar y cargar

Necesitas PlatformIO con soporte ESP-IDF y la placa Waveshare
ESP32-S3-Touch-LCD-7B (flash de 16 MB y PSRAM de 8 MB).
Abre esta carpeta en VS Code y ejecuta desde PlatformIO:

```powershell
pio run
pio run --target upload --upload-port COM5
pio device monitor
```

Sustituye `COM5` por tu puerto USB-UART. Las dependencias gestionadas se
descargan durante la compilación; no se incluyen sus copias en el repositorio.
El archivo para OTA se genera en
`.pio/build/waveshare-esp32-s3-touch-lcd-7b/firmware.bin`.

La tabla de particiones reserva dos espacios OTA de 4 MB. Al pasar desde el
firmware anterior es necesario cargar una vez por USB, porque la tabla de
particiones también cambia; después se podrá cargar `firmware.bin` desde la
página web local.

Conecta el inversor al USB nativo/OTG de la Waveshare, no al puerto USB-UART.
Alimenta la placa por separado y verifica que el cable USB Host entrega VBUS
al inversor. La selección de USB (en vez de CAN) se establece por el CH422G.

## Wi-Fi, estado y OTA

Desde la version **0.4.2**, si no hay configuracion guardada, el ESP32 crea
automaticamente la red WPA2 **Esp32AxpertVMII** y abre **Configurar Wi-Fi**
en la pantalla, con el mismo estilo LVGL que el panel. La clave aleatoria
de 12 caracteres aparece en la LCD; no es fija ni se publica en los registros.
Se genera una nueva si se reinicia sin guardar configuracion.

Para configurarlo desde el movil, conecta a esa red, conserva la conexion
aunque indique «sin Internet» y abre **http://192.168.4.1/**. Autenticate
como `admin` con la misma clave que muestra la LCD. Puedes guardar la red
de casa o una red propia, y cambiar la clave web; si la dejas vacia,
se conserva la clave inicial. No hace falta tocar la pantalla antes de
conectar el movil. El servidor sigue disponible mientras el formulario
inicial esta abierto en la LCD.

Tambien puedes completar todo en la pantalla:

- **Conectar a una Wi-Fi**: pulsa **Buscar**, elige una red de 2,4 GHz en
  **Elegir red** o escribe el SSID, incluida una red oculta. Una red abierta
  admite clave vacia; una protegida admite 8 a 63 bytes o PSK hexadecimal
  de 64 caracteres.
- **Crear una Wi-Fi**: introduce el nombre de la red propia y una clave
  WPA2 de 8 a 63 bytes; no requiere router ni Internet.
- **Clave web / OTA**: de 8 a 48 bytes; se precarga la clave inicial o la
  guardada. **Mostrar contrasenas** permite ver u ocultar ambas claves.
- El teclado LVGL permite minusculas, mayusculas, numeros y simbolos.
  Toca el campo que quieras editar. **Guardar** aplica los cambios y reinicia.

En modo red propia, conecta el ordenador o telefono a la red que has creado
y abre **http://192.168.4.1/**. La primera pagina web es la configuracion de
red; incluye un enlace al panel y OTA. Usa el usuario `admin` y la clave
web/OTA elegida en la pantalla (puede ser distinta de la clave WPA2).
El dispositivo admite hasta cuatro clientes. Es normal que el telefono
indique «sin Internet»; no hay portal cautivo ni apertura automatica.

En cualquiera de los modos, **Datos y OTA / Configurar Wi-Fi** abre `/wifi`.
Permite buscar redes, cambiar entre conexion al router y red propia,
cambiar opcionalmente la clave web y guardar con reinicio. Una clave web
vacia conserva la actual. El panel siempre esta disponible en `/panel`.

Si la red guardada no responde en 30 segundos al arrancar, o permanece
desconectada durante 30 segundos en funcionamiento, se activa una red de
recuperacion **Esp32AxpertVMII**, cuya clave WPA2 es la clave web/OTA actual.
Abre `http://192.168.4.1/` para reconfigurar. No se borran las claves
anteriores; un reinicio vuelve a intentar la red guardada. Mientras la red
de recuperacion esta activa, el cambio a la red de casa requiere guardar
su configuracion desde `/wifi` o reiniciar.

Desde la version **0.4.1**, el panel de la pantalla incluye un boton
**Configurar Wi-Fi** en la parte superior derecha, visible tambien en la
vista de detalles. Permite cambiar la red, sus claves o el modo sin usar
el navegador. Los datos guardados se precargan; las claves permanecen
ocultas salvo que actives **Mostrar contrasenas**. **Guardar** guarda
y reinicia. **Cancelar**, disponible
si ya habia configuracion guardada, reinicia sin guardar los cambios.
Al abrir los ajustes desde el panel se detienen el servidor web y la
conexion de red; se restauran al reiniciar. Esto no afecta al primer
arranque sin datos, donde se mantiene la web de configuracion del movil.
La web tambien permite mostrar u ocultar cada clave por separado.
El reloj RGB sigue a 20 MHz.

**Restablecer Wi-Fi y acceso web**, en `/wifi`, requiere autenticacion y
confirmacion. Borra exclusivamente la configuracion de red y acceso web,
reinicia y vuelve a la pantalla tactil inicial. No borra todo NVS ni cambia
ajustes del inversor. No es necesario restablecer para publicar el proyecto:
las claves se guardan en NVS de cada placa, nunca en el repositorio ni
en `firmware.bin`. Las actualizaciones OTA conservan esta configuracion.

Abre `http://<IP-local>/` desde un equipo de la misma red. La página muestra
el estado del HID y si el Axpert respondió correctamente a `QPI`/`PI30`.
En **Datos y OTA**, selecciona el `firmware.bin` compilado por PlatformIO para actualizar. No
expongas este servidor a Internet ni configures redirección de puertos; la
autenticación HTTP no cifra el tráfico.

También puedes abrir **http://esp32axpertvmii.local/** (mDNS). El nombre del
equipo es **Esp32AxpertVMII**; si el ordenador o la red no resuelven `.local`,
la IP que muestra la pantalla sigue funcionando.

## Panel energético en LCD y web

El firmware **0.4.2** incorpora un panel oscuro inspirado en el panel de
VoltronicSerial, adaptado a la LCD de 1024 x 600 y a navegadores de escritorio
y móvil. Incluye placas, entrada AC, Axpert, salida a casa, batería y ESP32.
Todos los valores energéticos proceden del Axpert; no se usan Shelly,
SmartSolar ni un enlace directo con los BMS.

- La LCD usa LVGL 8.4, fuentes Montserrat suavizadas, iconos y tarjetas
  redondeadas. Las medidas se actualizan cada segundo. Pulsa una tarjeta
  para ver los detalles y **Volver** para regresar al esquema.
  La configuracion Wi-Fi tambien usa LVGL y tiene acceso exclusivo al
  dibujo y al controlador tactil.
- La web consulta cada tres segundos **GET /api/panel** y el estado del ESP32.
  Toca una tarjeta para ver sus medidas. **Datos y OTA** mantiene el
  diagnóstico completo y la actualización de firmware. Se usa el usuario
  `admin` y la contraseña OTA, sin una segunda petición de contraseña en JS.
- LCD y web usan la misma instantánea: las medidas ausentes, inválidas,
  desconectadas o caducadas se representan con **Sin datos**, nunca con
  ceros inventados. El JSON compacto usa `null`; un cero real sigue siendo
  cero. `QPIGS` y modo caducan a los 18 s; BMS y avisos, a los 26 s.
- Solar FV y red AC llegan por trazados y puntos distintos al inversor;
  no hay una union exterior entre ambas entradas. La potencia AC de salida
  aparece solo en la tarjeta de casa, no se duplica como otra medida del
  inversor. La tarjeta Axpert muestra modo, temperatura e identidad USB.
- La entrada AC muestra tensión y frecuencia. El Axpert no facilita aquí
  potencia ni corriente de entrada: no se simulan a partir de la salida.
  La presencia de tensión de red no activa una animación de consumo de red.
- El SOC principal es el informado en `QPIGS`. El SOC BMS adicional, cuando
  está disponible y conectado según `QBMS`, se etiqueta **BMS vía Axpert**.
  La corriente neta es carga menos descarga; la potencia calculada es esa
  corriente por la tensión de batería. Son estimaciones y el signo positivo
  indica carga, no mediciones directas de la batería.
- Las energías acumuladas se convierten de Wh a kWh. El reloj mostrado es
  la última muestra del reloj del inversor, no un reloj corregido por el PC.
  Los bits `QPIWS` se exponen como estado bruto; no se presentan todos como
  fallos ni se afirma ausencia de alarmas.

La interfaz está incorporada en el firmware; no necesita SD ni Internet.
Desde **0.3.2**, LVGL dibuja directamente en el buffer RGB inactivo y espera dos
eventos de fin de trama del modo bounce antes de reutilizar el anterior.
Se evita copiar la imagen sobre el buffer que la LCD esta leyendo.
Se comparten los dos buffers RGB con LVGL en modo full-refresh, sin tercer
buffer ni copia adicional de la imagen. La configuracion sigue el modo
antidesgarro de doble buffer del
[ejemplo Waveshare 16_LVGL_UI](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7B/tree/master/examples/ESP-IDF/16_LVGL_UI).
Tras observar fallos visuales menos frecuentes pero persistentes con 0.3.2,
la **0.3.3** reduce solamente el reloj RGB de 30 a 20 MHz para comprobar
el margen de alimentacion de datos al panel. Con los mismos timings,
el barrido nominal pasa de aproximadamente 32,7 a 21,8 fotogramas/s;
las medidas siguen actualizandose cada segundo. No hubo mensajes serie
durante una observacion de 30 s de la 0.3.2; no es prueba de ausencia de
fallos DMA. Este ajuste es una prueba de estabilidad y su resultado visual
debe confirmarse en la LCD fisica.
Se usan CPU a 240 MHz, cache de datos de 64 bytes e instrucciones y rodata en
PSRAM, como en el ejemplo, para mantener el barrido durante operaciones de
flash. Se conserva PSRAM octal a 80 MHz: no se adopta el ajuste experimental
de 120 MHz del fabricante. El dibujo se pausa antes de comenzar OTA y se
reanuda si la carga falla. Una espera de trama prolongada se registra y
mantiene el buffer protegido, sin reiniciar automaticamente el equipo.
**Datos y OTA / Ver captura de la LCD** abre **GET /api/screen.bmp**, una
captura autenticada del buffer enviado al panel (1024 x 600, RGB565).
Permite revisar el diseño a distancia, pero no demuestra ausencia de
parpadeo ni verifica el barrido fisico. Durante la configuracion Wi-Fi la
captura puede responder 503 para no interferir con la entrada de claves.
No se monta ni se modifica la tarjeta SD insertada. No se guardan todavía
históricos. Los recursos HTML/CSS/JS se declaran tanto en CMake como en
`board_build.embed_txtfiles` para la compilación con PlatformIO.

Pruebas del panel, sin dependencias Node adicionales:

```powershell
node --test tests\panel.test.cjs tests\wifi.test.cjs
.\tests\test-live-panel.ps1 -Credential (Get-Credential -UserName admin)
```

## Adquisicion USB del Axpert

### Ajustes en los popups web (0.5.0)

La web clasifica 42 campos de configuracion consultados por USB. Que un
campo figure en el protocolo no significa que el firmware del Axpert lo
soporte. El panel LCD conserva sus vistas actuales; la edicion se realiza
en los popups de la web.

| Tarjeta | Configuraciones |
| --- | --- |
| Solar | Prioridad de carga, balance FV, condicion FV en paralelo y horario de carga |
| Entrada AC | Rango UPS/electrodomesticos, corriente de carga de red, retorno a red y bypass |
| Salida / Casa | Prioridad y horario de alimentacion, tension/frecuencia nominales, modo de salida y segunda salida |
| Bateria | Tipo, corrientes, absorcion, flotacion, corte, retorno a bateria, SOC y ecualizacion |
| Axpert | Zumbador, pantalla, alarmas, reinicios, registro de fallos, identidad, firmware, reloj y valores de fabrica |

Los valores ausentes, rechazados, no interpretables o caducados aparecen
como **Sin datos** y no se pueden editar. Los horarios se muestran de
00 a 23 h y son de solo consulta, al igual que los ajustes de segunda
salida, paralelo y variantes KING/MKSIII sin escritura o limites
verificables para el modelo. Las respuestas completas se conservan.

**GET /api/settings** devuelve `settings`, `additional`, `busy` y `job`.
Cada ajuste incluye grupo, consulta de origen, valor actual o `null`,
opciones o limites, `editable` y motivo de bloqueo. La escritura usa
**POST /api/settings**, autenticacion y JSON con `id`, `value`, `previous`
y `confirm: true`. No se admiten comandos arbitrarios.
La respuesta 202 significa aceptado para procesar, nunca aplicado:
solo `job.state: verified` confirma ACK con CRC valido y relectura
coincidente del valor.

**Aplicar** requiere confirmacion explicita. El servidor relee antes de
escribir, comprueba el valor anterior y sus dependencias, serializa un
solo cambio con las consultas USB y verifica el resultado. Un cambio de
conexion o configuracion durante la espera cancela el envio. NAK, error
USB, CRC invalido, timeout o relectura diferente no se presentan como
exito. No hay reintentos automaticos de escritura: si el resultado es
incierto, comprueba el valor real antes de repetir. OTA y guardar o
restablecer Wi-Fi se bloquean mientras hay un ajuste pendiente.

La escritura exige identidad PI30 y modelo `VMIII-` recientes. Las
corrientes solo admiten pasos comunicados por `QMCHGCR`/`QMUCHGCR`.
Las tensiones requieren bateria definida por usuario y limites del
manual incluido: 24 V con 1500/3000 W o 48 V con 5000 W. La ecualizacion
se habilita solo para AGM/inundada y esos modelos documentados; se bloquea
para perfiles de litio y bateria definida por usuario.
**Comprueba las especificaciones de tu bateria: el ESP32 no detecta su
quimica real.** Las opciones enumeradas proceden del protocolo y pueden
ser rechazadas con NAK; no se prueban mediante escrituras.
No se exponen calibracion, ATE, borrado de historicos, restablecimiento de
fabrica ni activacion inmediata de ecualizacion.

Pruebas C sin hardware con un compilador nativo o Zig y el cJSON incluido
en ESP-IDF, sin nuevas dependencias del firmware:

```powershell
.\tests\test-inverter-settings.ps1 -Compiler gcc
# Alternativa con Zig:
.\tests\test-inverter-settings.ps1 -Compiler zig -Zig
```

Cubren deteccion, limites y coherencia, codificacion de comandos,
relectura previa, ACK y verificacion posterior, NAK, CRC, caducidad,
timeout y cambios de conexion. No envian comandos al Axpert.

El firmware consulta una lista cerrada de comandos de lectura:

- Identificacion: `QPI`, `QID`, `QSID`, `QMN`, `QGMN`.
- Versiones: `QVFW`, `QVFW2`, `QVFW3`, `VERFW:`.
- Medidas, modo, avisos y BMS comunicado al inversor: `QPIGS`, `QMOD`,
  `QPIWS`, `QBMS`.
- Configuracion y capacidades: `QPIRI`, `QFLAG`, `QDI`, `QMCHGCR`,
  `QMUCHGCR`, `QOPPT`, `QCHPT`, `QBEQI`, `QDOP`.
- Reloj, energia FV y energia de salida acumuladas, estado del modulo Wi-Fi
  del inversor: `QT`, `QET`, `QLT`, `QWFS`.
- Si `QT` devuelve una fecha valida, energia FV y de salida del ano, mes y
  dia indicados por el reloj del inversor: `QEY`, `QEM`, `QED`, `QLY`,
  `QLM`, `QLD` con su fecha. No se cambia el reloj del Axpert.

El sondeo automatico no cambia ajustes ni fuerza carga, descarga o
ecualizacion; las escrituras anteriores requieren **Aplicar** y
confirmacion. Algunas consultas
son opcionales o propias de otras variantes: un `NAK` se registra como
`unsupported_nak` y se reintenta a los diez minutos, no como una lectura cero.
No se recorren anos, meses o dias anteriores ni otros equipos en paralelo.

La web y **GET /api/axpert**, con la misma autenticacion, muestran por comando:
respuesta ASCII completa sin CRC, tokens, campos interpretados cuando se
conoce el formato, antiguedad, indicador `stale`, estado del ultimo intento,
contadores de intentos, respuestas con CRC valido y errores. `decoded: false`
significa que el cuerpo no corresponde al formato esperado; se conserva la
respuesta para diagnostico. `QBMS` es informacion recibida por el Axpert,
no acceso directo al BMS de la bateria. Sus tres tensiones se convierten de
decimas de voltio a voltios; los tokens originales se conservan.
La escala de `QBMS` esta contrastada con la
[referencia PI30 de mpp-solar](https://github.com/jblance/mpp-solar/blob/master/mppsolar/protocols/pi30.py)
y con las tensiones de configuracion devueltas por esta unidad.

Las respuestas pueden ocupar varios informes HID; el buffer admite 1024
bytes. Los comandos largos tambien se fragmentan en informes de ocho bytes.
Solo hay una consulta en curso y una pausa entre ellas. Las medidas solicitan
refresco cada cuatro segundos y el resto tiene periodos mas largos; el ciclo
inicial de descubrimiento y los timeouts pueden retrasar esos refrescos.
Solo `QPI` con `PI30` y CRC valido confirma la identidad; la confirmacion
caduca si deja de recibirse. Una respuesta negativa a una consulta opcional
no significa que el USB este desconectado.

## Qué se reutiliza

- `VoltronicSerial/src/Inversor/protocols/voltronic_vmiii.py`: algoritmo CRC
  XMODEM, bytes reservados y formato comando + CRC + CR. El firmware implementa
  la misma trama para `QPI`.
- `VoltronicSerial/src/Inversor/transport.py`: referencia del transporte serie
  a 2400 8N1; no se usa porque aquí la comunicación del inversor va sobre USB
  HID, no por UART.
- Un flujo Node-RED con `HIDdevice` sirvió como referencia para el transporte
  USB HID y las consultas `QPIGS`/`QPIRI`.
- El protocolo Axpert Remote Panel Protocol sirvió como referencia de PI30,
  `QPI` y CRC. Los manuales, documentos y flujos locales no se distribuyen en
  este repositorio; no son necesarios para compilar.
- La configuración de pantalla RGB/CH422G sigue los ejemplos de Waveshare para
  ESP-IDF (1024 x 600), en
  [ESP32-S3-Touch-LCD-7B](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7B/tree/master/examples/ESP-IDF/06_LCD).

El VID/PID 0665:5161 identifica la variante VMIII común. El firmware verifica
además la respuesta `QPI` y su CRC antes de declarar conexión. Variantes con
otro PID necesitarán ajustar esa identificación.

## Prueba en la placa

La version 0.4.0 incorpora configuracion web, seleccion tactil de ambos
modos y recuperacion por punto de acceso. Se cargo por COM5 con hash
verificado y se borro la particion NVS para probar el primer arranque;
el registro confirma el inicio de la configuracion tactil. La version
0.4.1 anade el acceso a ajustes Wi-Fi desde el propio panel de la LCD.
La 0.4.1 se cargo y arranco conservando la red; el usuario confirmo que el
boton de la LCD abre los ajustes. La 0.4.2 sustituye el formulario de
mapa de bits por LVGL e incorpora la red inicial protegida para el movil.
Se cargo por COM5 con hash verificado y se borro exclusivamente NVS.
El registro confirma arranque 0.4.2, RGB a 20 MHz y punto de acceso en
192.168.4.1 sin errores durante la observacion inicial. El usuario confirmo
que puede abrir la configuracion desde el movil. Las 19 pruebas web pasan;
el control de mostrar/ocultar tambien se verifico en una vista local de
375 px de ancho, sin desbordamiento horizontal.
La prueba fisica de guardado de ambas modalidades y restablecimiento
con esta version esta pendiente; las
observaciones siguientes corresponden a versiones anteriores.

La version 0.3.3 se ha cargado por USB-UART con hash verificado. El arranque
confirma CPU a 240 MHz, PSRAM a 80 MHz y reloj RGB de 20 MHz. La web responde
tras reconectar Wi-Fi. El usuario informa de que el fallo visual no ha vuelto
a aparecer durante su observacion inicial y de que la actualizacion desde
la web parece haber funcionado. Falta una prueba prolongada. Un intento OTA
anterior agoto el tiempo de espera; no se ha determinado su causa.

En pruebas previas con un VMIII-3000 se confirmaron respuestas USB con CRC
valido, coherencia entre el panel y las respuestas crudas y resolucion mDNS.
De las 32 consultas intentadas, 29 devolvieron datos y `QWFS`, `QDOP` y
`QVFW2` devolvieron `NAK`. Tambien se verificaron las energias del reloj del
inversor y la conversion de tensiones `QBMS`. Estas medidas no se han
revalidado con 0.3.3: el Axpert estaba desconectado durante la prueba de LCD.

El monitor serie registra el arranque, la enumeracion, los errores HID y
la validacion CRC. La compilacion comprobada utiliza ESP-IDF 5.5.4 y LVGL 8.4.

Para repetir la prueba de adquisicion real, sin guardar credenciales:

```powershell
.\tests\test-live-telemetry.ps1 -Credential (Get-Credential -UserName admin)
```

Comprueba que se intentan las consultas, los campos de medidas coinciden
con los tokens recibidos, la escala de tensiones BMS es correcta, los `NAK`
se distinguen de datos actuales y la conexion permanece activa.

## Licencia

Copyright (C) 2026 Restaun.

El codigo original de este proyecto se distribuye bajo la
[GNU General Public License v3.0](LICENSE). Las dependencias y referencias
de terceros conservan sus respectivas licencias y atribuciones; esta
licencia no sustituye las suyas. El proyecto no esta afiliado a Waveshare
ni a Voltronic.

## Invitame a un cafe

Si este proyecto te resulta util, puedes apoyar su desarrollo en
[Ko-fi: restaun](https://ko-fi.com/restaun). Gracias por tu apoyo.
