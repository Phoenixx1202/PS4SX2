# PCSX2 for PS4



## About the Project

This project is developing a **PlayStation 2 emulator for PlayStation 4**, based on the open-source **PCSX2** codebase. The goal is to allow PS2 games to run locally on the PS4, with all processing and rendering performed directly by the console.

The development adapts the PCSX2 core and uses the existing PS5 implementation in this repository as a starting point. The PS4-specific layer uses **OpenOrbis**, an open SDK for homebrew development, along with open-source libraries and a Vulkan graphics path using Mesa RADV adapted for the console. The work involves adapting memory management, emulated processors, graphics, input, file handling, and the other services required for execution.

The project is currently in an **experimental integration and proof-of-concept stage**. Diagnostic applications are already running successfully on a PS4 Pro, with video output, controller input, and parts of the graphics and memory subsystems confirmed to be working on real hardware. **The PS4 port has not yet booted the BIOS or executed any PS2 games.** The next major step is to integrate these components into the emulator's complete initialization flow.

The initial goal is to successfully boot the BIOS and run a test title. Compatibility across the PS2 game library, emulation performance, and support for other PS4 models or firmware versions will be evaluated as development progresses.

This is an independent project and is not officially affiliated with Sony or the PCSX2 team. BIOS files and games are not distributed with this project.

## Objective

Port PCSX2 so that it can run as a native homebrew application on the PS4 using OpenOrbis and open-source libraries.




https://github.com/user-attachments/assets/e48dccb5-cc8f-41e9-aa6f-295ed41142f7


https://github.com/user-attachments/assets/9d1bb0de-8055-4770-86a0-1e711ff021e8


https://youtu.be/ckPCVlBHaio
