#pragma once
#ifndef KWIFI_EVENTTOKEN_SHIM_H
#define KWIFI_EVENTTOKEN_SHIM_H
/* MinGW has no EventToken.h; WebView2.h only needs the struct */
typedef struct EventRegistrationToken {
    __int64 value;
} EventRegistrationToken;
#endif
