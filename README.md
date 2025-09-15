# ChompChamps — Instrucciones de uso

Este documento explica cómo compilar y ejecutar el proyecto **ChompChamps** (master / players / view) dentro del contenedor Docker provisto por la cátedra, y describe las opciones de ejecución corregidas y aclaradas.

Repositorio (ejemplo que compartiste):
[https://github.com/joacoabraldes/SO\_TP1](https://github.com/joacoabraldes/SO_TP1)

---

## Requisitos

* Docker (opcional, pero recomendado para reproducibilidad).
* Si no usás Docker: `make`, `gcc`/`clang` y build-essential en un sistema Linux.

Imagen Docker recomendada (proporcionada por la cátedra):

```
dagodio/itba-so-multi-platform:3.0
```

Comando de ejemplo para abrir un contenedor con el repo montado en `/root`:

```
docker run -v "${PWD}:/root" -w /root --privileged -ti agodio/itba-so-multi-platform:3.0 /bin/bash
```

Dentro del contenedor: compilar con `make` (suponiendo que el `Makefile` del repo está correcto):

```sh
make
```

Esto debe producir los binarios del proyecto (por ejemplo: `ChompChamps`, `view`, `player_trapper`, `player`, etc.).

---

## Archivos importantes

* `ChompChamps` (máster / orquestador). Lanza jugadores y (opcionalmente) la `view`, usa memoria compartida y semáforos.
* `view` (visualizador de tablero). Lee la memoria compartida y dibuja el estado.
* `player_trapper`, `player` (jugadores). Se conectan al `master` a través de memoria compartida y envían **un byte** por `stdout` con el movimiento.

---

## Ejecución del juego

Ejemplo de ejecución (igual que indicaste):

```sh
./master -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player_trapper -p ./player_trapper
```

### Descripción de opciones (corregidas y aclaradas)

* `-w <width>`: Ancho del tablero. Default: `10`. Mínimo recomendado: `10`.
* `-h <height>`: Alto del tablero. Default: `10`. Mínimo recomendado: `10`.
* `-d <delay>`: Delay en milisegundos que usa el máster para esperar entre impresiones / pacing. Default: `200` (ms).
* `-t <timeout>`: Timeout en segundos para recibir movimientos válidos (si no hay movimientos válidos en ese lapso, el juego termina). Default: `10` (s).
* `-s <seed>`: Semilla para generación del tablero. Default: `time(NULL)` (semilla por tiempo).
* `-v <view>`: Ruta al binario `view`. Si se omite, no se lanza la vista.
* `-p <player>`: Ruta a un binario jugador. Puede repetirse para añadir múltiples jugadores. Mínimo: `1`, Máximo: `9` (definido por `MAX_PLAYERS`).

> Nota importante: Los **jugadores** deben enviar exactamente **1 byte** (tipo `unsigned char`) con valores `0..7` por su `stdout` como solicitud de movimiento — **no** enviar texto ASCII `'0'..'7'` ni una cadena con `\n`. El `master` espera un *raw byte* con la dirección.

---

## Ejemplo de invocación paso a paso

1. Clonar el repositorio y entrar en la carpeta:

```sh
git clone https://github.com/joacoabraldes/SO_TP1.git
cd SO_TP1
```

2. Abrir contenedor Docker (opcional)

```sh
docker run -v "${PWD}:/root" -w /root --privileged -ti agodio/itba-so-multi-platform:3.0 /bin/bash
# dentro del contenedor:
make
```

3. Ejecutar el juego (ejemplo con view y 2 jugadores):

```sh
./master -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player_trapper -p ./player_trapper
```

4. Alternativa: arrancar `view` por separado (si no le pasaste `-v` al master):

```sh
# en otra terminal (o si arrancas manualmente):
./view 10 10
```

5. Alternativa: arrancar un `player` manualmente (debug):

```sh
./player 10 10
```

---

## Errores frecuentes y cómo arreglarlos

* **La "cara" (glifo ☺) no aparece en view**

  * Verificar que los jugadores envíen el byte raw `0..7` (tipo `unsigned char`) en vez de caracteres ASCII.
  * Revisar `stderr` del player para ver qué valor dice que envió (los players de ejemplo imprimen logs a `stderr`).

* **Master marca movimientos como inválidos**

  * El master comprueba: rango `0..7`, objetivo dentro del tablero y celda libre (`board[idx] > 0`).
  * Si el player envía ASCII `'3'` (valor 51), se contabiliza como inválido.

* **EPIPE / master cierra pipe**

  * Si el master termina, los players reciben `EPIPE` al escribir. Si un player cierra su `stdout`, el master detecta EOF y marca al jugador como bloqueado.

* **Problemas de sincronización (view se queda colgado)**

  * Asegurarse que el master y la view usan los mismos tamaños (anchos/altos) al abrir la memoria compartida.

---

## Sugerencias de mejora (opcionales)

* Añadir un `Makefile` target `run-demo` que ejecute `ChompChamps` junto con `view` y un par de `player` para pruebas.
* Agregar scripts de test que lancen el máster y prueben la comunicación de pipes verificando que el master interprete bytes `0..7` correctamente.
* Documentar en el código `player` la forma correcta de enviar el byte (ejemplo compacto en C incluido en la sección "Código cliente ejemplo" a continuación).

---

## Código cliente ejemplo (en C) — enviar un solo byte numeric 0..7

```c
unsigned char mv = (unsigned char)best_dir; // best_dir en 0..7
ssize_t w = write(STDOUT_FILENO, &mv, 1);
if (w != 1) { /* manejar error o EPIPE */ }
```

---

Si querés, genero un `README.md` en el repo o te dejo este archivo como `README_CHOMPCHAMPS.txt` listo para pegar en el proyecto. También puedo preparar:

* Un `run-demo.sh` que automatice la ejecución dentro del contenedor.
* Un `check_player_protocol.c` que lance un master de pruebas y valide que los players envíen bytes en formato correcto.

Dime cuál de esas opciones querés y lo genero.
