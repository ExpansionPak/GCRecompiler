# GCRecompiler

GCRecompiler is a **Work-In-Progress** tool to take [GameCube](https://en.wikipedia.org/wiki/GameCube) .dol and .iso files then recompile them into C code for any platform. It can be used to make native ports of GameCube games, as well as for simulating behaviors significantly faster than interpreters or dynamic recompilation can.

Of course. This is not the first project that uses static recompilation on game console binaries. A really well-known example is [N64Recomp](https://github.com/N64Recomp/N64Recomp) by [Mr-Wiseguy](https://github.com/Mr-Wiseguy), which targets N64 binaries. Additionally, this is not even the first project to apply static recompilation to the GameCube. [GCRecomp](https://github.com/KaiserGranatapfel/GameCubeRecompiled) by [Christopher Erleigh](https://github.com/KaiserGranatapfel) aims for the same goal as GCRecompiler. However, GCRecomp aims to recompile GameCube .dol files into optimized Rust code instead of C code like GCRecompiler.

## That's cool. But how does it work?
The recompiler uses a similar approach to N64Recomp, but it loads a GameCube main.dol file, maps its text, data, and BSS segments, and analyzes executable areas in order to discover functions and their control flow. It doesn't require a complete symbol table up front; at the moment, it begins its work from the entry point, scans for function prologue candidates in text, and additionally searches for what could be pointers to executable addresses in both text and data segments. Using this approach, it recovers not only normal functions, but also callbacks and other code reachable from jump tables.

## Okay... But. Does the GameCube really "need" this?
"**need**" is subjective. Because some already feel emulation is alerady good enough that a "Static Recompiler" isn't really necessary. However. When the game's main.dol file itself has been recompiled into C code. It can then be linked with a runtime and a graphics renderer, and once you can make a recomp functional, it provides a lot of rooms to enhancement. That includes widescreen support, higher FPS, 4k resolution, texture packs, mod support, and possibly even Ray-Tracing. However. Those graphics enhancements need to be provided by the graphics renderer itself. That being said. It's a much more "**customizable**" way to play GameCube games.

A pre-existing runtime already exists in GCRecompiler's src. Simply go into "include" and you should see recomp_runtime.h. Altough the runtime may need to be readjusted to work with the specific game you're recompiling

## This is cool! But how do I build this?
This project does not rely on external tools and is written purely in C++. It uses CMake, which is available for Windows, Linux, and MacOS. A much more detailed document on building this project with clear steps is being worked on. In the near future. Pre-built binaries will be available for the 3 previously mentioned Operating Systems.

## How can I contribute?
If you're interested in contributing to GCRecompiler. You can fork the project, start a new branch for organized work, commit and push your new changes, and make a Pull Request (PR) to the original project. If your PR is accepted and merged with the main project, your username will be added to [CONTRIBUTORS.md](CONTRIBUTORS.md)

## NOTE
Dolphin Emulator has been a huge help in researching the GameCube.
Be sure to check out their GitHub [here](https://github.com/dolphin-emu/dolphin)!