#ifndef ALARMAS_CONFIG_H
#define ALARMAS_CONFIG_H

#include <stdbool.h>

#define MAX_ALARM_NAME    48
#define MAX_SEQUENCE_LEN  7

typedef enum {
    DIFICULTAD_BAJA = 0,
    DIFICULTAD_MEDIA,
    DIFICULTAD_ALTA
} dificultad_t;

typedef enum {
    GESTO_IZQUIERDA = 0,
    GESTO_DERECHA,
    GESTO_ARRIBA,
    GESTO_ABAJO,
    GESTO_CERCA,
    GESTO_LEJOS,
    NUM_GESTOS
} gesto_t;

typedef struct {
    int id;
    bool enabled;
    int weekday;                 // 0=domingo, 1=lunes, ..., 6=sabado
    int hour;                    // 0-23
    int minute;                  // 0-59
    dificultad_t difficulty;
    char name[MAX_ALARM_NAME];
} alarma_config_t;

typedef struct {
    gesto_t gestos[MAX_SEQUENCE_LEN];
    int length;
} secuencia_t;

/*
 * Base de datos simple de alarmas.
 *
 * Campos:
 * id, enabled, weekday, hour, minute, difficulty, name
 *
 * weekday:
 * 0 = domingo
 * 1 = lunes
 * 2 = martes
 * 3 = miercoles
 * 4 = jueves
 * 5 = viernes
 * 6 = sabado
 *
 * difficulty:
 * DIFICULTAD_BAJA  -> 3 gestos
 * DIFICULTAD_MEDIA -> 5 gestos
 * DIFICULTAD_ALTA  -> 7 gestos
 */
static const alarma_config_t alarmas_config[] = {
    {1, true,  1, 6, 30, DIFICULTAD_BAJA,  "Alarma lunes manana"},
    {2, true,  2, 6, 30, DIFICULTAD_MEDIA, "Alarma martes manana"},
    {3, true,  3, 6, 30, DIFICULTAD_MEDIA, "Alarma miercoles manana"},
    {4, true,  4, 6, 30, DIFICULTAD_ALTA,  "Alarma jueves manana"},
    {5, true,  5, 6, 30, DIFICULTAD_ALTA,  "Alarma viernes manana"},
    {6, false, 6, 8,  0, DIFICULTAD_BAJA,  "Alarma sabado desactivada"}
};

static const int num_alarmas_config =
    sizeof(alarmas_config) / sizeof(alarmas_config[0]);

#endif
