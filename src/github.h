// GitHub 贡献热力图（就是主页那片小绿框）。
// 数据源：github-contributions-api.jogruber.de —— 免 key，返回每天一个 {date,count,level}，
// level 0~4 正好是 GitHub 自己那五档颜色，不用自己分档。
// 用户名在 secrets.h 的 GITHUB_USER。
#pragma once
#include "globals.h"

void githubEnter();
void githubUpdate();
void githubKey(char k);
void drawGithub();
