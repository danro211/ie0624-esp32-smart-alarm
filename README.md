# IE0624 ESP32 Smart Alarm

Proyecto final del curso IE0624 Laboratorio de Microcontroladores.

## Descripción general

Este proyecto consiste en una alarma anti-snooze basada en microcontroladores ESP32. El sistema utiliza un ESP32 principal para manejar la lógica de alarma, hora actual, dificultad y comunicación, y una ESP32-CAM para capturar imágenes de la mano del usuario y ejecutar inferencia local con un modelo TinyML.

La alarma solo se apaga cuando el usuario completa correctamente una secuencia de gestos simples con la mano, como izquierda, derecha, arriba, abajo, cerca o lejos.

## Hardware previsto

- ESP32-WROOM-32
- ESP32-CAM con cámara OV2640
- Programador USB-Serial CH340 para ESP32-CAM
- LCD1602 con interfaz I2C
- Buzzer pasivo
- Pulsador
- Protoboard, jumpers y alimentación USB
