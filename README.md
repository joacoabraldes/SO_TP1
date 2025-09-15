# ChompChamps — Instrucciones de uso

Este documento explica cómo compilar y ejecutar el proyecto **ChompChamps** (master / players / view) dentro del contenedor Docker provisto por la cátedra, y describe las opciones de ejecución corregidas y aclaradas.

Repositorio:
(https://github.com/joacoabraldes/SO_TP1)

---

## Requisitos

* Docker.

Imagen Docker (proporcionada por la cátedra):

```
dagodio/itba-so-multi-platform:3.0
```

Comando de ejemplo para abrir un contenedor con el repo montado en `/root`:

```
docker run -v "${PWD}:/root" -w /root --privileged -ti agodio/itba-so-multi-platform:3.0 /bin/bash
```

Dentro del contenedor: compilar con `make` :

```sh
make
```

Esto debe producir los binarios del proyecto (por ejemplo: `view`, `player`, etc.).

---

## Archivos importantes

* `master` (máster / orquestador). Lanza jugadores y (opcionalmente) la `view`, usa memoria compartida y semáforos.
* `view` (visualizador de tablero). Lee la memoria compartida y dibuja el estado.
* `player` (jugador). Se conectan al `master` a través de memoria compartida y envían **un byte** por `stdout` con el movimiento.

---

## Ejecución del juego

Ejemplo de ejecución (igual que indicaste):

```sh
./master -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
```

### Descripción de opciones

* `-w <width>`: Ancho del tablero. Default: `10`. Mínimo recomendado: `10`.
* `-h <height>`: Alto del tablero. Default: `10`. Mínimo recomendado: `10`.
* `-d <delay>`: Delay en milisegundos que usa el máster para esperar entre impresiones / pacing. Default: `200` (ms).
* `-t <timeout>`: Timeout en segundos para recibir movimientos válidos (si no hay movimientos válidos en ese lapso, el juego termina). Default: `10` (s).
* `-s <seed>`: Semilla para generación del tablero. Default: `time(NULL)` (semilla por tiempo).
* `-v <view>`: Ruta al binario `view`. Si se omite, no se lanza la vista.
* `-p <player>`: Ruta a un binario jugador. Puede repetirse para añadir múltiples jugadores. Mínimo: `1`, Máximo: `9` (definido por `MAX_PLAYERS`).

---



