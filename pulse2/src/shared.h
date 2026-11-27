// La memoria che il pannello divide col processo dei sensori («Local\Pulse2.Shared», 4096 byte). La crea il pannello, che gira
// senza diritti; il processo dei sensori, l'unico da amministratore, la apre. Ognuno scrive solo la sua parte: prima porta il suo
// contatore a un numero dispari, scrive, poi lo riporta pari; chi legge scarta una lettura col contatore dispari o cambiato.
// Le posizioni dei campi sono le stesse costanti di sensors\Program.cs: cambiarle qui vuol dire cambiarle anche là.
#pragma once
#include <stddef.h>

enum { SHARED_MAGIC = 0x32534C50, SHARED_VERSION = 1, SHARED_SIZE = 4096, NSLOT = 3 };
enum { SF_ELEVATED = 1, SF_PAWNIO = 2, SF_SAMPLING = 4, SF_NO_SAMPLING = 8, SF_FAKE = 16, SF_ALIVE = 256 };   // SF_ALIVE lo aggiunge il pannello

struct SharedPanel {                    // scritta dal pannello, una volta al secondo
    LONG seq; DWORD pid; ULONGLONG tick;                                // tick: GetTickCount64 dell'ultima scrittura, il segno che il pannello c'è
    int ncore;
    int slotCount[NSLOT]; wchar_t slotName[NSLOT][PROGNAME]; DWORD slotPid[NSLOT][MAXPID];   // i programmi da colorare nelle barre dei core, coi loro processi
};
struct SharedSensors {                  // scritta dal processo dei sensori
    LONG seq; DWORD pid; ULONGLONG tick;
    int flags;
    float cpuTemp, cpuWatt, cpuMhz;                                     // -1 = non c'è; cpuMhz è il core più veloce
    int shareCores; BYTE share[MAXCORE][NSLOT];                         // per core logico, la parte (0..255) dei campioni non idle di ciascun programma
};
struct Shared { DWORD magic, version; SharedPanel panel; SharedSensors sensors; };

static_assert(offsetof(Shared, panel) == 8 && offsetof(Shared, panel.tick) == 16 && offsetof(Shared, panel.ncore) == 24, "SharedPanel");
static_assert(offsetof(Shared, panel.slotCount) == 28 && offsetof(Shared, panel.slotName) == 40 && offsetof(Shared, panel.slotPid) == 280, "SharedPanel slots");
static_assert(offsetof(Shared, sensors) == 1048 && offsetof(Shared, sensors.tick) == 1056 && offsetof(Shared, sensors.flags) == 1064, "SharedSensors");
static_assert(offsetof(Shared, sensors.cpuTemp) == 1068 && offsetof(Shared, sensors.shareCores) == 1080 && offsetof(Shared, sensors.share) == 1084, "SharedSensors values");
static_assert(sizeof(Shared) <= SHARED_SIZE, "Shared");
