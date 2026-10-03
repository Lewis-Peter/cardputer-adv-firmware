#pragma once

// Shared one-row list navigation.  Callers keep ownership of the list data;
// this only maintains the selected index and, when present, its scroll window.
static inline void listMoveIndex(int& index, int count, int dir) {
  if (count <= 0) return;
  index = (index + (dir < 0 ? count - 1 : 1)) % count;
}

static inline void listClampScroll(int index, int& top, int count, int visible) {
  if (count <= 0) { top = 0; return; }
  if (top > count - visible) top = count > visible ? count - visible : 0;
  if (index < top) top = index;
  if (index >= top + visible) top = index - visible + 1;
}

static inline void listMove(int& index, int& top, int count, int visible, int dir) {
  if (count <= 0) return;
  listMoveIndex(index, count, dir);
  listClampScroll(index, top, count, visible);
}
