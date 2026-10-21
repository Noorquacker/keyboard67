# Keyboard 67

A continuation of Keyboard 71 (itself a revival of Jormy's Nintype / Keyboard 69) that runs on
modern 64-bit-only Android phones. The original engine is a closed 32-bit ARM library; on arm64 and
x86_64 it runs inside ninbridge, an ARM32 JIT translation layer (see [PORTING.md](PORTING.md)).

This is the readme

Rules for contributing:

1. Don't make fun of my code
2. Make it better than you left it
3. PRs are welcome but are up to my discertion. Generally - if you change something, make it an option.


Architechture:

1. libgl2jni.so - The native library. Untouchable. (32-bit ARM only; on arm64/x86_64 it runs inside
   ninbridge, an ARM32 JIT layer in app/src/main/cpp. See PORTING.md.)
2. NINLib.kt -> interfaces from the app into the library. 
3. Methods marked @Api (by me, you're welcome). Methods that the library calls as the API.
    DO NOT CHANGE THEIR SIGNATURE
4.Ninview - where the displaying logic happens
5.Softkeyboard - where the keyboard logic happens

Good luck, make sure to ask me if you need to know anything.

-lurebat 
