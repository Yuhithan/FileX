# TExplorer

TExplorer is a terminal file browser with a two-pane ncurses interface, a file
viewer/editor, directory search, a context menu, and a command prompt for the
current directory.

## Build

Requires a C compiler, CMake, and ncurses development headers.

```sh
cmake -S . -B build
cmake --build build
```

## Run

```sh
./build/texplorer --tui [directory]
./build/texplorer --windows [directory]
```

Both interactive options open the two-pane terminal interface. The previous
directory-listing modes are also available:

```sh
./build/texplorer [directory]         # Plain listing
./build/texplorer --gui [directory]   # JSON listing
./build/texplorer --read FILE         # Print a file
cat input.txt | ./build/texplorer --write FILE
```

`-read` and `-write` are accepted as aliases for the double-dash options.

## File manager keys

- Arrow keys move through files; Enter opens directories or views files.
- Backspace moves to the parent directory.
- `/` searches names in the current directory; Escape clears the search.
- `e` edits the selected file; Ctrl+S saves and Escape exits the editor.
- `n` creates a file and opens it in the editor.
- Shift+Enter opens the context menu. Right-click is supported by terminals
  that report mouse events.
- `:` opens a shell command prompt in the current directory.
- `r` refreshes the listing; `q` quits.

Shift+Enter key reporting depends on terminal support for the Kitty keyboard
protocol; mouse right-click is an alternative on terminals with mouse support.
