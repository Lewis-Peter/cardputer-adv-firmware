// ChatGPT 聊天页：输入一条问题，后台请求 OpenAI 兼容的 /v1/chat/completions
// 接口（走 secrets.h 里配的 base url / key / model），回复按屏幕宽度换行分页显示。
// 只保留最近几轮短上下文；无限积累历史会让请求体和 TLS 后的连续堆一起失控。
#pragma once
#include "globals.h"

extern String chatInput;    // 正在输入的问题
extern String chatError;    // 上一次请求的错误信息（空 = 没错）
extern int chatPage;        // 回复分页当前页

// 请求跑在单独的 FreeRTOS 任务里，主循环不会被 60s 的等待卡住。
void chatSend();            // 非阻塞：连好 Wi-Fi、起后台任务，立刻返回
void chatUpdate();          // loop() 里调：任务完成后在主线程做换行排版（要用 cv 量字宽）
bool chatBusy();            // 请求进行中——画"asking..."、并屏蔽再次回车
bool chatTakeReply();       // 有排好版的新回复待显示则返回 true（取走标志）
int  chatPageCount();
void chatReset();           // 进入 Chat app 时清掉上一段会话
void chatExit();            // 离开 Chat app 时调：在途请求跑完后把结果丢掉，别污染历史
void chatBackspace();       // 按 UTF-8 字符边界退格，避免中文删成半个字

void drawChatInput();
void drawChatReply();
void drawChatBusyDirect();
