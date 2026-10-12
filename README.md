# Servidor Web para Control de LED RGB WS2812B



## Descripción

Este proyecto implementa un servidor web embebido en un microcontrolador ESP32 para controlar un LED RGB. A través de una página web hospedada en el mismo dispositivo (page.html), el usuario puede interactuar y cambiar los colores y el estado del LED. El proyecto está desarrollado íntegramente utilizando el framework ESP-IDF, separando la lógica del control del LED en un componente independiente y verificable.

## Hardware utilizado

Placa de desarrollo ESP32.
1x LED RGB (WS2812B) conectado en el pin GPIO14.
Jumpers

## Versión de ESP-IDF y target

Framework: ESP-IDF v6.1.0
Target: esp32


## Cómo compilar y flashear

1. Configura el target (solo la primera vez):
```bash
   idf.py set-target esp32
```
2. Compila:
```bash
   idf.py build
```
3. Flashea y abre el monitor serial:
```bash
   idf.py -p COM9 flash monitor
```

## Cómo ejecutar las pruebas unitarias

Las pruebas viven en un proyecto ESP-IDF independiente dentro del componente,
en `components/led_app_logic/test_apps/`. Para correrlas en la tarjeta:

1. Entra a la carpeta del proyecto de pruebas:
```bash
   cd components/led_app_logic/test_apps
```
2. Compila, flashea y abre el monitor:
```bash
   idf.py -p COM9 flash monitor
```
3. En el monitor serial debe aparecer:
```
   2 Tests 0 Failures 0 Ignored
   OK
```
## Estructura del proyecto

## Estructura del proyecto

```
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   ├── main.c
│   └── page.html            # Interfaz web hospedada en el ESP32
├── components/
│   └── led_app_logic/       # Componente independiente para el control del LED
│       ├── CMakeLists.txt
│       ├── led_app_logic.c
│       └── test_apps/       # Carpeta con las pruebas unitarias en Unity
└── README.md
```

## Licencia

Este proyecto está bajo la licencia MIT. Ver el archivo [LICENSE](LICENSE) para más detalles.