#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void UserApp_AppInit(void);   // sensor / rtc bring-up
void UserApp_TaskInit(void);  // 定时把值写进 ui_model 并刷 UI

#ifdef __cplusplus
}
#endif
