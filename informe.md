# **72.11 Sistemas Operativos**

# Trabajo Practico 1 \- ChompChamps

**Grupo 11**

 **Integrantes:** 

- Abraldes, Joaquin (65063)  
- Andraca, Mateo (65177)

 **Fecha de Entrega:** 14/9/2025

# 

# 1\. Objetivo

* Entregar tres binarios funcionales: master, view, player.  
* Garantizar sincronización entre procesos, consistencia del estado y salida ordenada.  
* Proveer herramientas reutilizables (shm\_manager) que simplifiquen uso de shm\_open/mmap y semáforos en memoria compartida.  
* Implementar un player con heurísticas y Monte Carlo simplificado.

# 2\. Decisiones tomadas durante el desarrollo

* **Arquitectura:** se respetó la división en 3 procesos: master, view, player. Además se añadió shm\_manager.c para centralizar shm\_open/mmap/sem\_init y reducir duplicación.

* **Sincronización:** se implementó el patrón lectores/escritor para evitar race conditions entre jugadores (lectores) y el master (escritor). Se usaron semáforos anónimos almacenados en la shm /game\_sync.

* **Semáforos:**

  - master\_mutex — marca intención o prioridad del master para escribir.  
  - state\_mutex — mutex que protege escritura del estado y lecturas exclusivas;  
  - reader\_count\_mutex \+ reader\_count — contador de lectores.  
  - player\_mutex\[i\] — tokens por jugador.  
  - master\_to\_view / view\_to\_master — sincronización de actualización/paint entre master y view.

* **Event-driven vs Round-robin:** elegimos un modelo event-driven usando select() sobre los pipes de los jugadores en lugar de un round-robin estricto. Motivos:

  - Mayor reactividad: el master procesa movimientos tan pronto llegan sin esperar un turno fijo.

  - Mejor uso de CPU: evita ciclos activos o sleeps innecesarios.

  - Mantuvimos fairness: el master da un token inicial a todos y, tras procesar la solicitud de un jugador, hace sem\_post(player\_mutex\[i\]) para que ese jugador pueda enviar la siguiente; la atención posterior no reinicia siempre desde el jugador 0, se sigue el flujo de sockets/pipes marcado por select() evitando sesgos sistemáticos.

* **Colocación de jugadores:** determinística (esquinas, centro y ejes medios) para reproducibilidad y margen de movimiento similar.

* **Política de validación:** el master valida rango \[0..7\], límites del tablero y que la célula destino sea libre; registra inválidos y válidos.

# 3\. Instrucciones de compilación y ejecución

* **Entorno** 


Usar la imagen provista por la cátedra:

- docker pull agodio/itba-so-multi-platform:3.0  
    
  - docker run \-v "${PWD}:/root" \-w /root \--privileged \-ti agodio/itba-so-multi-platform:3.0 /bin/bash  
      
* **Compilación**


Dentro del contenedor:

- make clean  
  - make  
    

El Makefile genera los ejecutables: master, view, player.

* **Ejemplo de ejecución**  
    
  - ./master \-w 10 \-h 10 \-d 200 \-t 10 \-s 123 \-v ./view \-p ./player ./player  
    

# 4\. Rutas relativas para torneo

* Vista: ./view

* Jugador: ./player

# 5\. Limitaciones

* El player que implementamos tiene heurísticas y MonteCarlo simplificado; contra variantes óptimas puede perder. Hacerlo “imbatible” requiere mayor coste computacional (MCTS UCT, tabla de transposición).

* Si el timeout (-t) es pequeño y los players usan muchas simulaciones, el master puede alcanzar timeout de juego.

* Se asume que los ejecutables se corren dentro del contenedor recomendado.

# 6\. Problemas encontrados y cómo se solucionaron  

* **Race conditions lectura/escritura de** game\_state:

  - Problema: lecturas inconsistentes por parte de view y players.

  - Solución: patrón lectores/escritor con reader\_count y semáforos (reader\_count\_mutex y state\_mutex).

* **Players bloqueados al finalizar:**

  - Problema: jugadores esperando indefinidamente si master terminaba sin avisar.

  - Solución: master marca game\_over \= true y hace sem\_post en todos player\_mutex\[i\] para desbloquear y permitir salida limpia.

* **Coste de desarrollar un player competitivo:**

  - Observación: diseñar un jugador fuerte (Monte Carlo, heurísticas y playouts realistas) aumentó mucho la complejidad y tiempo de CPU. Requiere:

    * Implementación de simulaciones (copiar tableros, jugadores).

    * Políticas de selección, Voronoi y anti-suicidio.

    * Ajuste del número de simulaciones para no exceder el timeout del master.

  - Impacto: tradeoff fuerza vs tiempo real; ajustar sims\_per\_candidate y heurísticas fue costoso en pruebas.