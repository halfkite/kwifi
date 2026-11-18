#pragma once
#include <windows.h>

/* native/server.c */
int  srv_write_wv2_loader(const wchar_t *path);
void srv_start(HINSTANCE inst, const wchar_t *exeDir, const wchar_t *dataDir, int port);
void srv_stop(void);

/* native/kwifi.c - exposed to the backend */
int  kwifi_autostart_get(wchar_t *out, int cch);
int  kwifi_autostart_set(int enable, wchar_t *out, int cch);
void kwifi_request_exit(void);
int  kwifi_is_admin(void);
int  kwifi_is_service(void);
int  kwifi_service_installed(void);
int  kwifi_service_running(void);
int  kwifi_service_install(void);
int  kwifi_service_remove(void);
void kwifi_request_elevation(void);
int  kwifi_task_state(void);
int  kwifi_task_install(void);
int  kwifi_task_remove(void);




