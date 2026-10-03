// 多子页 app 的翻页链。
//
// 以前每个子页都在 handleKey() 里手写一遍"往前跳谁 / 往后跳谁 / ` 折回谁"，
// 再在各自的 draw() 里手写一遍 drawPageDots(第几页, 共几页)——加一页要改 2N 处，
// 而且漏改不报错，只会静默画错页码点。现在两边都从 pages.cpp 那一张表查，
// 加/删页只动那张表。
#pragma once
#include "globals.h"

// 在 s 所属的链里前后挪一格（d=+1 往后 / -1 往前），到头环回。不在任何链里则原样返回。
Screen pageStep(Screen s, int d);

// s 所属链的第一页——后面几页按 ` 折回这里。不在任何链里则原样返回。
Screen pageFirst(Screen s);

// 查 s 在链里的序号(0起)和链长。不在任何链里返回 false。
bool pageIndex(Screen s, int& cur, int& count);
