# Radio Garden PS3

Radio por internet estilo [Radio Garden](https://radio.garden) para PS3 con CFW/HEN (PSL1GHT).
Proyecto no oficial; usa la API publica no documentada de radio.garden.

## Controles
- Stick izq. / cruceta: mover el mapa. L1/R1: zoom. X: abrir emisoras de la ciudad bajo la mira.
- Lista: arriba/abajo, X reproducir, O volver. Triangulo: parar. L2/R2: volumen. Select: alternar verificacion TLS.

## Compilar
GitHub Actions compila en cada push (artefacto `radio-garden-ps3`: `.pkg` para instalar en CFW/HEN).
Local: `export PS3DEV=... PSL1GHT=...; make pkg` (requiere libcurl, mbedTLS y libmad de ps3libraries).

## Limitaciones
- Solo streams MP3 (AAC/HLS se muestran como "Formato no soportado").
- Fuente bitmap derivada de DejaVu Sans Mono.
