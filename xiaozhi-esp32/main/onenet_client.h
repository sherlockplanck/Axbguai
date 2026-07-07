#ifndef ONENET_CLIENT_H
#define ONENET_CLIENT_H

#include <string>

// 启动 OneNET 后台任务：
// 1. 上传 STM32 通过串口发来的 fall_alarm；GPS 有效时同时上传 latitude、longitude。
// 2. 接收 OneNET 云端下发的 call_cmd、text_msg，并立即转发给 STM32。
void onenet_client_start();

// 上传小智语音采集到的拐杖消息到 OneNET，物模型属性名：guai_message。
bool onenet_publish_guai_message(const std::string& message);

#endif // ONENET_CLIENT_H
