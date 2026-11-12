# Umbriel · edit personal

Este repositorio es un **edit personal** de [Umbriel](https://github.com/noctalia-dev/umbriel)
para mi escritorio Debian con el shell de `~/Shell`.

No es una versión oficial, no está afiliado a Noctalia y no pretende ser una
versión mejorada ni una alternativa general. Los cambios están pensados para
mis necesidades y mi forma de usar el escritorio.

## Base

- Proyecto original: Umbriel de Noctalia.
- Base: commit `b6c4b7d`.
- Este repositorio no comparte historia con el original: es una copia
  independiente, no un fork de GitHub.

## Qué incluye

- Barra de título nativa con botón de volumen y acciones propias del shell.
- Modos de escritorio mosaico/ventana integrados con la configuración nativa.
- Restauración de posición y tamaño de ventanas flotantes.
- Velocidad de animaciones y opción de desactivarlas.
- Desenfoque «Fondo» que se mantiene al arrastrar ventanas.
- Atajos del micrófono, incluidas teclas multimedia y botones laterales del
  ratón.
- Ajustes de integración con el shell: foco con Super, notificaciones, volumen
  por ventana y detalles de la barra.
- Piloto de **resolución de render fija** para juegos Xwayland
  (`steam_app_*`, apps que abren en pantalla completa o con tamaño fijo): el
  compositor nunca reconfigura el tamaño del cliente al redimensionar la
  ventana ni al entrar o salir de pantalla completa. El juego conserva su
  resolución interna (y puede seguir creyendo que está en pantalla completa) y
  el contenido se presenta escalado con la relación de aspecto del render,
  centrado y con barras negras donde sobra, como una ventana de vídeo. Así una
  ventana más pequeña no dispara los bugs de resize de los juegos.
  Si Wine/DXVK dibuja en una ventana hija (resolución o aspecto distintos del
  toplevel), el piloto recorta la superficie a esa hija y descarta las barras
  que pinta el propio juego. La ventana nace con el tamaño y aspecto del
  render, y los adopta en vivo cuando el juego cambia de resolución, sin
  sondear X por fotograma. Solo con escala de salida 1 y, por ahora, en
  ventanas flotantes.

## Licencia

El edit se distribuye bajo GNU GPL v3.0: es libre para usar, estudiar,
compartir y modificar, pero si distribuyes una versión modificada tiene que
seguir siendo GPL-3.0 y con el código disponible. Revisa `LICENSE`.

Umbriel es MIT; el aviso y la atribución del proyecto original se conservan en
`LICENSE-MIT`.

## Sin soporte

No hay soporte, garantías ni compromiso de mantener compatibilidad con el
Umbriel oficial. Si algo te sirve, úsalo; si no, usa el proyecto original.
