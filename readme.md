![preview](./data/screenshot.png)

A toy gui application done for the purpose of studying immediate mode graphics UI's.

It's implemented on top of openGl and glfw.

There is support for:

 - A fairly comprehensive layout system.
 - A css-like styling mechanism.
 - Simple animation.
 - Basic widgets: buttons, scrollview, scrollbars, dialogs, grids/tables, ...
 - Keyboard focus navigation.
 - Blur effect.
 - Very basic text support (using harfbuzz and freetype; including emojis 😀).

Big thanks to the raddbg and orca projects for many of the ideas.

The project is written in C2y, so you need newer versions of gcc/clang.
You also need glfw, freetype, and harfbuzz devel packages.
