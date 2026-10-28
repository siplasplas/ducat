#pragma once

#include <vector>

class QTreeView;

// Common behaviour of the lists of entries.

// Lets every column of `view` be resized with the mouse. Until the user
// does so, the columns fit their contents whenever the rows or the view's
// size change, and `fill_columns` share the width left. The header's
// context menu offers Fit Columns, which starts fitting again.
void make_columns_resizable(QTreeView* view, const std::vector<int>& fill_columns);

// Copy (Ctrl+C) and Copy with Column Names in the view's context menu: the
// selected rows, in the order shown, as tab-separated values of the shown
// columns.
void add_copy_menu(QTreeView* view);
